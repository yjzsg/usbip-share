/*
 * Management-API support (optional HTTP enrichment for the server).
 *
 * Worker thread responsibilities:
 *  1. GET  http://host:webport/api/devices     -> alias/remark/owner map
 *  2. POST http://host:webport/api/clients/heartbeat -> register this PC
 *
 * Results are delivered to the GUI thread with wxQueueEvent (thread-safe,
 * pending events are dropped if the window is destroyed meanwhile).
 *
 * Everything degrades gracefully: no management API => upstream behavior.
 */

#include <winsock2.h>
#include <ws2tcpip.h>

#include <windows.h>
#include <winhttp.h>

#include "wusbip.h"
#include "utils.h"
#include "wxutils.h"

#include <libusbip/vhci.h>

#include <wx/config.h>
#include <wx/dataview.h>
#include <wx/log.h>
#include <wx/msgdlg.h>
#include <wx/msw/registry.h>
#include <wx/textdlg.h>
#include <wx/thread.h>
#include <wx/utils.h>

#include <objbase.h>

#include <algorithm>
#include <cctype>
#include <charconv>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <limits>
#include <stdexcept>
#include <utility>

namespace
{

/*
 * 服务器备注/占用方两列的位置见 device_columns.h 的 mgmt_column_pos_t。
 * 此前这里是硬编码的 12/13,与 frame.cpp 的 AppendColumn 顺序隐式耦合;
 * 改为具名常量后,列顺序一旦调整会在此处编译期报错而非静默写错列。
 */
static_assert(usbip::MGMT_COL_REMARK == 12 && usbip::MGMT_COL_OWNER == 13 && usbip::MGMT_COL_QUEUE == 14,
              "列位置已变化,请同步 frame.cpp 的 AppendColumn 顺序");

struct mgmt_result
{
        bool ok{};
        std::string err; // 失败原因,交给 GUI 线程显示,便于排查
        mgmt::meta_map meta;
        // "轮到你了" 通知,由服务端在心跳响应里一次性下发(mailbox 已清空)。
        std::vector<mgmt::queue_notice> notices;
        /*
         * 被动发现:服务端回了 401 + accessRequired(启用了访问控制而本机未授权,
         * 或授权已过期且用保存的密码静默续期也没成功)。置位时 GUI 线程要走
         * 与主动连接同一套弹框流程,而不是把它当成普通的"管理接口不可用"。
         */
        bool access_required{};
};

/*
 * 「共享访问密码」相关的时间常量。
 *
 * kAccessProbeTtl      —— "这台服务器不需要密码"的缓存时长。纯粹是为了避免
 *                         一次批量 attach 里对同一台服务器反复探测(每次探测
 *                         都要一次 HTTP 往返);60 秒内管理员改了服务端配置
 *                         也只会晚一分钟生效,而被动发现(401)仍然会立刻纠正。
 * kAccessDefaultTtl    —— 服务端没回 expiresInSeconds 时的授权有效期。契约
 *                         默认 12 小时,这里取同一个值。
 * kAccessPromptSnooze  —— 用户在被动发现的弹框里点「取消」后的冷却时间。
 *                         没有它,后台每 30 秒一次的轮询会把同一个模态框不停
 *                         糊到用户脸上。用户主动连接/刷新不受此限制。
 */
constexpr auto kAccessProbeTtl = std::chrono::seconds(60);
constexpr auto kAccessDefaultTtl = std::chrono::hours(12);
constexpr auto kAccessPromptSnooze = std::chrono::minutes(5);

void lower_ascii(std::string &s)
{
        std::transform(s.begin(), s.end(), s.begin(), [](auto c) {
                return static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
        });
}

/*
 * 注册表根键。历史版本曾把 MgmtEnabled/WebPort 等写成 REG_SZ 字符串;而当前代码
 * 按 REG_DWORD 读取,类型错配会让 wxRegKey 抛 ERROR_MORE_DATA/类型错误,被 wx
 * 日志系统弹成 "USBip Error" 模态框(即用户看到的"打开就报错")。
 * 下面两个函数:读取时静默容忍 DWORD/REG_SZ 两种类型;再按 DWORD 自愈写回。
 *
 * 根键不再硬编码 —— 统一取 win::config_registry_root()(与 wxConfig 同源,见 utils.h)。
 * 旧版这里写死 "Software\\usbip-win2\\USBip",而 .rc 的 CompanyName/ProductName 早已
 * 改成 USB-SHARE,wxConfig 的实际根路径是 "Software\\USB-SHARE\\USB-SHARE":
 * 两个路径分裂后,MgmtEnabled/WebPort 全被写到已废弃的旧键上。
 */
const wxString& cfg_root()
{
        static const wxString root = win::config_registry_root();
        return root;
}

bool cfg_read_int(const wchar_t *name, int defval, int &out)
{
        wxLogNull no_log; // 注册表类型不干净时绝不能弹日志对话框
        wxRegKey key(wxRegKey::HKCU, cfg_root());
        if (!key.Exists() || !key.Open(wxRegKey::Read) || !key.HasValue(name)) {
                out = defval;
                return false;
        }
        const auto t = key.GetValueType(name);
        if (t == wxRegKey::Type_Dword) {
                long v = 0;
                if (key.QueryValue(name, &v)) {
                        out = static_cast<int>(v);
                        return true;
                }
        } else if (t == wxRegKey::Type_String) {
                wxString s;
                long v = 0;
                if (key.QueryValue(name, s) && s.ToLong(&v)) {
                        out = static_cast<int>(v);
                        return true;
                }
        }
        out = defval;
        return false;
}

void cfg_repair_int(const wchar_t *name, long val)
{
        wxLogNull no_log;
        wxRegKey key(wxRegKey::HKCU, cfg_root());
        const bool opened = key.Exists() ? key.Open(wxRegKey::Write) : key.Create();
        if (!opened) {
                return;
        }
        if (!key.HasValue(name) || key.GetValueType(name) != wxRegKey::Type_Dword) {
                key.SetValue(name, val); // RegSetValueEx 覆盖旧类型为 REG_DWORD
        }
}

/*
 * 删除 wxConfig 中的某个值。用于清理已知遗留/无效设置,避免下次启动再次读旧值。
 */
void cfg_delete_value(const wchar_t *name)
{
        wxLogNull no_log;
        wxRegKey key(wxRegKey::HKCU, cfg_root());
        if (!key.Exists() || !key.Open(wxRegKey::Write)) {
                return;
        }
        if (key.HasValue(name)) {
                key.DeleteValue(name);
        }
}

std::string trim(const std::string &s)
{
        auto b = s.find_first_not_of(" \t\r\n");
        if (b == std::string::npos) {
                return {};
        }
        auto e = s.find_last_not_of(" \t\r\n");
        return s.substr(b, e - b + 1);
}

/* 解析十进制端口;非法/越界时保持原值不变。 */
void parse_port(const std::string &digits, int &port)
{
        if (digits.empty()) {
                return;
        }
        int v{};
        if (auto [ptr, ec] = std::from_chars(digits.data(), digits.data() + digits.size(), v);
            ec == std::errc() && ptr == digits.data() + digits.size() && v > 0 && v <= 65535) {
                port = v;
        }
}

/*
 * Split "usbip://host[:port]" (or "host:port") into host and numeric port.
 *
 * 不能用 rfind(':') 一刀切: 裸 IPv6 字面量(如 fe80::1)含多个冒号,
 * 取最后一个会把 "fe80:" 当主机、把 "1" 当端口。规则改为:
 *   [ipv6] / [ipv6]:port  -> 方括号内整体是主机, 括号后才可能是端口
 *   恰好一个冒号           -> host:port
 *   其余(无冒号 / 裸 IPv6) -> 整体是主机, 不解析端口
 */
bool parse_host_port(const std::string &url, std::string &host, int &port)
{
        auto s = url;
        if (auto p = s.find("://"); p != std::string::npos) {
                s = s.substr(p + 3);
        }
        if (auto p = s.find('/'); p != std::string::npos) {
                s = s.substr(0, p);
        }
        if (s.empty()) {
                return false;
        }

        if (s.front() == '[') {
                auto close = s.find(']');
                if (close == std::string::npos) {
                        return false; // 方括号未闭合
                }
                host = s.substr(1, close - 1);
                if (auto rest = s.substr(close + 1); !rest.empty()) {
                        if (rest.front() != ':') {
                                return false;
                        }
                        parse_port(rest.substr(1), port);
                }
        } else if (auto p = s.rfind(':'); p != std::string::npos && s.find(':') == p) {
                host = s.substr(0, p); // 恰好一个冒号
                parse_port(s.substr(p + 1), port);
        } else {
                host = s; // 无冒号,或裸 IPv6
        }

        return !host.empty();
}

bool hostname_matches(const std::string &url, const std::string &wanted)
{
        std::string h;
        int p{};
        if (!parse_host_port(url, h, p)) {
                return false;
        }
        lower_ascii(h);
        return h == wanted;
}

/* ---- minimal blocking HTTP over WinSock (Connection: close) ---- */

/*
 * 解析响应状态行,取出三位状态码;无法解析时返回 0。
 * 不能再用 "HTTP/1.1 200" 前缀匹配:那样遇到 201/204 会误判失败,
 * 遇到 chunked 正文或重定向又完全看不出原因。
 */
int http_status_code(const std::string &response)
{
        auto eol = response.find('\n');
        if (eol == std::string::npos) {
                return 0;
        }
        auto line = response.substr(0, eol);
        auto sp = line.find(' ');
        if (sp == std::string::npos) {
                return 0;
        }
        int value = 0;
        int digits = 0;
        for (auto i = sp + 1; i < line.size(); ++i) {
                auto c = line[i];
                if (c < '0' || c > '9') {
                        break;
                }
                value = value * 10 + (c - '0');
                if (++digits == 3) {
                        break;
                }
        }
        return digits == 3 ? value : 0;
}

/*
 * 还原 Transfer-Encoding: chunked 的正文。
 * 服务端若走 Python 的 chunked 输出,原始正文里会混入 "1a\r\n" 这样的
 * 分块长度行,直接丢给 JSON 解析器必然失败——而失败是静默的,表现为
 * "设备名/备注/占用方一直不显示",极难排查。这里主动还原。
 */
std::string http_dechunk(const std::string &body)
{
        std::string out;
        size_t pos = 0;
        for (;;) {
                auto eol = body.find("\r\n", pos);
                if (eol == std::string::npos) {
                        break;
                }
                auto sizeLine = body.substr(pos, eol - pos);
                if (auto semi = sizeLine.find(';'); semi != std::string::npos) {
                        sizeLine.resize(semi); // 去掉分块扩展
                }
                size_t chunkSize = 0;
                auto [ptr, ec] = std::from_chars(sizeLine.data(), sizeLine.data() + sizeLine.size(), chunkSize, 16);
                if (ec != std::errc() || ptr != sizeLine.data() + sizeLine.size() || chunkSize == 0) {
                        break;
                }
                auto dataStart = eol + 2;
                if (dataStart + chunkSize > body.size()) {
                        out.append(body, dataStart, std::string::npos); // 正文被截断,尽量取回
                        break;
                }
                out.append(body, dataStart, chunkSize);
                pos = dataStart + chunkSize + 2; // 跳过块尾 CRLF
        }
        return out;
}

std::string http_exchange(const mgmt::settings &cfg, const char *method, const std::string &path, const std::string &body, bool &ok)
{
        ok = false;
        auto host = cfg.host;

        addrinfo hints{};
        hints.ai_family = AF_UNSPEC;
        hints.ai_socktype = SOCK_STREAM;
        hints.ai_protocol = IPPROTO_TCP;

        addrinfo *res = nullptr;
        auto service = std::to_string(cfg.web_port);
        if (getaddrinfo(host.c_str(), service.c_str(), &hints, &res) != 0 || !res) {
                return {};
        }

        std::string response;
        SOCKET sock = INVALID_SOCKET;
        for (auto ai = res; ai; ai = ai->ai_next) {
                sock = socket(ai->ai_family, ai->ai_socktype, ai->ai_protocol);
                if (sock == INVALID_SOCKET) {
                        continue;
                }
                u_long mode = 1;
                ioctlsocket(sock, FIONBIO, &mode);

                connect(sock, ai->ai_addr, static_cast<int>(ai->ai_addrlen));

                timeval tv{ 2, 0 };
                fd_set wfds;
                FD_ZERO(&wfds);
                FD_SET(sock, &wfds);
                auto rc = select(0, nullptr, &wfds, nullptr, &tv);
                if (rc == 1) {
                        int err = 0;
                        int len = static_cast<int>(sizeof(err));
                        getsockopt(sock, SOL_SOCKET, SO_ERROR, reinterpret_cast<char*>(&err), &len);
                        if (err == 0) {
                                break; // connected
                        }
                }
                closesocket(sock);
                sock = INVALID_SOCKET;
        }
        freeaddrinfo(res);
        if (sock == INVALID_SOCKET) {
                return {};
        }

        u_long mode = 0;
        ioctlsocket(sock, FIONBIO, &mode);

        std::string req;
        req += method;
        req += ' ';
        req += path;
        req += " HTTP/1.1\r\n";
        req += "Host: ";
        req += cfg.host;
        req += ':';
        req += service;
        req += "\r\n";
        req += "Accept: application/json\r\n";
        req += "User-Agent: usbip-win2-wusbip/1.0\r\n";
        req += "Connection: close\r\n";
        if (!cfg.token.empty()) {
                req += "X-Admin-Token: ";
                req += cfg.token;
                req += "\r\n";
        }
        if (!body.empty()) {
                req += "Content-Type: application/json; charset=utf-8\r\n";
                req += "Content-Length: ";
                req += std::to_string(body.size());
                req += "\r\n";
        }
        req += "\r\n";
        req += body;

        size_t sent = 0;
        while (sent < req.size()) {
                timeval tv{ 3, 0 };
                fd_set wfds;
                FD_ZERO(&wfds);
                FD_SET(sock, &wfds);
                if (select(0, nullptr, &wfds, nullptr, &tv) != 1) {
                        break;
                }
                auto n = send(sock, req.data() + sent, static_cast<int>(req.size() - sent), 0);
                if (n == SOCKET_ERROR || n == 0) {
                        break;
                }
                sent += static_cast<size_t>(n);
        }

        if (sent == req.size()) {
                char buf[8192];
                for (;;) {
                        timeval tv{ 3, 0 };
                        fd_set rfds;
                        FD_ZERO(&rfds);
                        FD_SET(sock, &rfds);
                        auto rc = select(0, &rfds, nullptr, nullptr, &tv);
                        if (rc != 1) {
                                break;
                        }
                        auto n = recv(sock, buf, sizeof(buf), 0);
                        if (n > 0) {
                                response.append(buf, static_cast<size_t>(n));
                        } else {
                                break;
                        }
                }
        }

        closesocket(sock);

        // 只要收到了完整的 HTTP 响应就算通信成功,具体状态码交给调用方判断
        ok = http_status_code(response) != 0;
        return response;
}

/*
 * 从原始 HTTP 响应里取出正文(必要时还原 chunked)。
 * @return false 表示响应里没有头部结束标记,正文不可信
 */
bool http_body(const std::string &response, _Out_ std::string &body)
{
        body.clear();

        auto hdrEnd = response.find("\r\n\r\n");
        if (hdrEnd == std::string::npos) {
                return false;
        }

        auto headers = response.substr(0, hdrEnd);
        body = response.substr(hdrEnd + 4);

        auto lowered = headers;
        lower_ascii(lowered); // 头字段名大小写不敏感
        if (lowered.find("transfer-encoding: chunked") != std::string::npos) {
                body = http_dechunk(body);
        }
        return true;
}

/*
 * 与 http_get 同源,但把 HTTP 状态码**以及非 200 时的正文**也带回来。
 *
 * 需要它的原因:「共享访问密码」的鉴权契约靠状态码和正文区分语义 ——
 *   401 + {"accessRequired":true}  -> 需要密码
 *   403 + {"error":"访问密码不正确"} -> 密码错
 * 而 http_get 的既有语义是"非 200 一律返回空串",那个语义要原样保留给所有
 * 老调用方(设备列表/占用方那一套),所以另开这个低层入口,http_get 改为
 * 调用它 —— 两条路径共享同一份取正文/去 chunked 的实现,不会再分叉。
 *
 * @param status 收到的 HTTP 状态码(0 = 连接失败或响应不完整)
 * @param err    失败原因(可选),保持与 http_get 一致的文案
 * @return 正文;只要收到了完整的 HTTP 响应就返回(哪怕状态码是 4xx/5xx)
 */
std::string http_get_raw(
        const mgmt::settings &cfg, const std::string &path, _Out_ int &status, std::string *err = nullptr)
{
        status = 0;

        bool ok = false;
        auto resp = http_exchange(cfg, "GET", path, {}, ok);
        if (!ok) {
                if (err) *err = "无法连接或响应不完整";
                return {};
        }

        status = http_status_code(resp);

        std::string body;
        if (!http_body(resp, body)) {
                if (err) *err = "响应缺少头部结束标记";
                return {};
        }
        return body;
}

/*
 * 取回正文。err(可选)用于把失败原因带回调用方,便于在状态栏/日志里显示——
 * 旧实现所有失败一律返回空串,用户只看到"设备名/备注/占用方不显示",
 * 无法区分是连不上、超时、非 200,还是正文编码问题。
 */
std::string http_get(const mgmt::settings &cfg, const std::string &path, std::string *err = nullptr)
{
        int status = 0;
        std::string raw_err;
        auto body = http_get_raw(cfg, path, status, &raw_err);

        if (!raw_err.empty()) {
                if (err) *err = raw_err;
                return {};
        }
        if (status != 200) {
                if (err) *err = "服务器返回 HTTP " + std::to_string(status);
                return {};
        }
        return body;
}

/*
 * 与 http_post_json 同源,但把状态码**以及非 200 时的正文**带回来
 * (理由同 http_get_raw:401/403 的正文是访问密码鉴权的判据)。
 *
 * @param ok true 表示收到了完整的 HTTP 响应(不要求状态码为 200)
 */
std::string http_post_raw(
        const mgmt::settings &cfg, const std::string &path, const std::string &body,
        _Out_ int &status, _Out_ bool &ok)
{
        status = 0;
        ok = false;

        auto resp = http_exchange(cfg, "POST", path, body, ok);
        if (!ok) {
                return {};
        }

        status = http_status_code(resp);
        ok = status != 0;

        std::string payload;
        http_body(resp, payload); // 头部不完整时返回空串,状态码仍然有效
        return payload;
}

/*
 * POST 并取回正文。心跳响应里带 queueNotifications(设备轮到你了),
 * 所以不能只判断状态码 —— 必须把正文交给调用方解析。
 *
 * @param ok  true 表示收到了完整且状态码为 200 的响应
 * @return 正文(可能为空字符串:服务端 200 + 空正文时)
 */
std::string http_post_json(const mgmt::settings &cfg, const std::string &path, const std::string &body, bool &ok)
{
        int status = 0;
        bool received = false;
        auto payload = http_post_raw(cfg, path, body, status, received);

        ok = received && status == 200;
        return ok ? payload : std::string();
}

/* ---- tiny JSON (objects/arrays/strings/numbers/bools) ---- */

struct json_value
{
        enum class kind { null, boolean, number, string, array, object };
        kind k{ kind::null };
        bool b{};
        double num{};
        std::string str;
        std::vector<std::string> keys;
        std::vector<json_value> vals;

        const json_value* find(const std::string &name) const
        {
                for (size_t i = 0; i < keys.size(); ++i) {
                        if (keys[i] == name) {
                                return &vals[i];
                        }
                }
                return nullptr;
        }
};

void json_skip_ws(const char *&p, const char *end)
{
        while (p < end && (*p == ' ' || *p == '\t' || *p == '\r' || *p == '\n')) {
                ++p;
        }
}

std::string json_unescape(const char *&p, const char *end)
{
        // p points just after the opening quote
        std::string out;
        while (p < end && *p != '"') {
                if (*p == '\\') {
                        ++p;
                        if (p >= end) {
                                break;
                        }
                        switch (*p) {
                        case 'n': out += '\n'; break;
                        case 'r': out += '\r'; break;
                        case 't': out += '\t'; break;
                        case 'b': out += '\b'; break;
                        case 'f': out += '\f'; break;
                        case '/': out += '/'; break;
                        case '\\': out += '\\'; break;
                        case '"': out += '"'; break;
                        case 'u': {
                                if (end - p < 5) {
                                        return out;
                                }
                                unsigned int code = 0;
                                for (int i = 1; i <= 4; ++i) {
                                        auto c = p[i];
                                        code <<= 4;
                                        if (c >= '0' && c <= '9') code |= static_cast<unsigned int>(c - '0');
                                        else if (c >= 'a' && c <= 'f') code |= static_cast<unsigned int>(c - 'a' + 10);
                                        else if (c >= 'A' && c <= 'F') code |= static_cast<unsigned int>(c - 'A' + 10);
                                        else return out;
                                }
                                p += 4;
                                // BMP only is enough for the server payloads used here.
                                if (code >= 0x80) {
                                        char utf8[4]{};
                                        int n = 0;
                                        if (code >= 0x10000) { utf8[0] = static_cast<char>(0xF0 | (code >> 18)); utf8[1] = static_cast<char>(0x80 | ((code >> 12) & 0x3F)); utf8[2] = static_cast<char>(0x80 | ((code >> 6) & 0x3F)); utf8[3] = static_cast<char>(0x80 | (code & 0x3F)); n = 4; }
                                        else if (code >= 0x800) { utf8[0] = static_cast<char>(0xE0 | (code >> 12)); utf8[1] = static_cast<char>(0x80 | ((code >> 6) & 0x3F)); utf8[2] = static_cast<char>(0x80 | (code & 0x3F)); n = 3; }
                                        else { utf8[0] = static_cast<char>(0xC0 | (code >> 6)); utf8[1] = static_cast<char>(0x80 | (code & 0x3F)); n = 2; }
                                        out.append(utf8, static_cast<size_t>(n));
                                } else if (code > 0) {
                                        out += static_cast<char>(code);
                                }
                                break;
                        }
                        default: break;
                        }
                } else {
                        out += *p;
                }
                ++p;
        }
        if (p < end && *p == '"') {
                ++p;
        }
        return out;
}

json_value json_parse_value(const char *&p, const char *end);

json_value json_parse_string(const char *&p, const char *end)
{
        json_value v;
        v.k = json_value::kind::string;
        v.str = json_unescape(p, end);
        return v;
}

json_value json_parse_object(const char *&p, const char *end)
{
        json_value v;
        v.k = json_value::kind::object;
        ++p; // consume '{'
        json_skip_ws(p, end);
        while (p < end && *p != '}') {
                json_skip_ws(p, end);
                if (p >= end || *p != '"') {
                        break;
                }
                ++p; // opening quote
                auto key = json_unescape(p, end);
                json_skip_ws(p, end);
                if (p >= end || *p != ':') {
                        break;
                }
                ++p;
                json_skip_ws(p, end);
                auto value = json_parse_value(p, end);
                v.keys.push_back(std::move(key));
                v.vals.push_back(std::move(value));
                json_skip_ws(p, end);
                if (p < end && *p == ',') {
                        ++p;
                }
        }
        if (p < end && *p == '}') {
                ++p;
        }
        return v;
}

json_value json_parse_array(const char *&p, const char *end)
{
        json_value v;
        v.k = json_value::kind::array;
        ++p; // consume '['
        json_skip_ws(p, end);
        while (p < end && *p != ']') {
                auto value = json_parse_value(p, end);
                v.vals.push_back(std::move(value));
                json_skip_ws(p, end);
                if (p < end && *p == ',') {
                        ++p;
                }
        }
        if (p < end && *p == ']') {
                ++p;
        }
        return v;
}

json_value json_parse_value(const char *&p, const char *end)
{
        json_skip_ws(p, end);
        if (p >= end) {
                return {};
        }
        json_value v;
        switch (*p) {
        case '{': return json_parse_object(p, end);
        case '[': return json_parse_array(p, end);
        case '"': ++p; return json_parse_string(p, end);
        case 't': if (end - p >= 4 && std::strncmp(p, "true", 4) == 0) { p += 4; v.k = json_value::kind::boolean; v.b = true; } return v;
        case 'f': if (end - p >= 5 && std::strncmp(p, "false", 5) == 0) { p += 5; v.k = json_value::kind::boolean; v.b = false; } return v;
        case 'n': if (end - p >= 4 && std::strncmp(p, "null", 4) == 0) { p += 4; } return v;
        default: {
                auto start = p;
                while (p < end && *p != ',' && *p != '}' && *p != ']'
                       && *p != ' ' && *p != '\t' && *p != '\r' && *p != '\n') {
                        ++p;
                }
                auto text = std::string(start, p);
                v.k = json_value::kind::number;
                v.num = std::strtod(text.c_str(), nullptr);
                return v;
        }
        }
}

bool json_parse(const std::string &text, json_value &root)
{
        if (trim(text).empty()) {
                return false;
        }
        auto p = text.data();
        auto end = p + text.size();
        try {
                root = json_parse_value(p, end);
        } catch (...) {
                return false;
        }
        return root.k == json_value::kind::object || root.k == json_value::kind::array;
}

std::string json_escape(const std::string &s)
{
        std::string out;
        for (auto c : s) {
                switch (c) {
                case '"': out += "\\\""; break;
                case '\\': out += "\\\\"; break;
                case '\n': out += "\\n"; break;
                case '\r': out += "\\r"; break;
                case '\t': out += "\\t"; break;
                default:
                        if (static_cast<unsigned char>(c) < 0x20) {
                                out += ' ';
                        } else {
                                out += c;
                        }
                }
        }
        return out;
}

/* ---- 「共享访问密码」(/api/access) ---- */

/*
 * GET /api/access 的探测结果。
 *
 * reachable == false 表示这台服务器**没有**这个端点(老版本服务端、或管理接口
 * 根本没开)。调用方必须据此放行 —— 这是向后兼容的硬要求:老服务器、没设
 * 密码的服务器上,连接行为必须与改造前完全一致。
 */
struct access_probe
{
        bool reachable{};
        bool required{};
};

access_probe probe_access(_In_ const mgmt::settings &cfg)
{
        access_probe r;

        int status = 0;
        std::string err;
        auto body = http_get_raw(cfg, "/api/access", status, &err);

        // 非 200(含老服务端的 404)、空正文、非 JSON —— 一律当作"没有访问控制"。
        // 绝不能让一次探测失败变成"连不上服务器"。
        if (status != 200 || body.empty()) {
                return r;
        }

        json_value root;
        if (!json_parse(body, root)) {
                return r;
        }

        r.reachable = true;

        if (auto v = root.find("required"); v && v->k == json_value::kind::boolean) {
                r.required = v->b;
        }
        return r;
}

/*
 * POST /api/access/authorize —— 用给定的密码换取一次本机授权。
 *
 * 服务端契约:
 *   200 {"ok":true,"authorized":true,"required":true,"expiresInSeconds":43200,...}
 *   403 {"ok":false,"error":"访问密码不正确"}
 *   200 {"ok":true,"authorized":true,"required":false}   // 未启用访问控制
 *
 * @param err         失败原因(服务端文案优先,交给弹框/日志显示)
 * @param expires_sec 成功时服务端给的授权有效期(秒);<=0 表示服务端没给
 * @param rejected     可选。仅当服务端**明确拒绝了密码**(HTTP 401/403)时置 true;
 *                    网络不通、5xx、响应格式错等一律保持 false。调用方靠它区分
 *                    "密码真的不对"(可以清掉本地保存的密码)和"这次没连上"
 *                    (绝不能清 —— 否则服务器一抖动就把用户存好的密码删了)。
 * @return true 仅当服务端明确回了 authorized:true
 */
bool authorize_access(
        _In_ const mgmt::settings &cfg, _In_ const std::string &password,
        _Out_ std::string &err, _Out_ int &expires_sec, _Out_ bool *rejected = nullptr)
{
        err.clear();
        expires_sec = 0;
        if (rejected) {
                *rejected = false;
        }

        std::string body = "{";
        body += "\"password\":\"" + json_escape(password) + "\"";
        body += ",\"clientId\":\"" + json_escape(cfg.client_id) + "\"";
        body += ",\"clientName\":\"" + json_escape(cfg.client_name) + "\"";
        body += "}";

        int status = 0;
        bool received = false;
        auto payload = http_post_raw(cfg, "/api/access/authorize", body, status, received);

        if (!received) {
                err = "无法连接服务器";
                return false;
        }

        json_value root;
        const bool parsed = json_parse(payload, root);

        auto server_error = [&root, parsed]() -> std::string
        {
                if (parsed) {
                        if (auto v = root.find("error"); v && v->k == json_value::kind::string && !v->str.empty()) {
                                return v->str; // 例如"访问密码不正确"
                        }
                }
                return {};
        };

        if (status != 200) {
                if (rejected && (status == 401 || status == 403)) {
                        *rejected = true; // 服务端明确拒绝了这个密码
                }
                err = server_error();
                if (err.empty()) {
                        err = "服务器返回 HTTP " + std::to_string(status);
                }
                return false;
        }

        if (!parsed) {
                err = "服务器响应不是有效 JSON";
                return false;
        }

        auto authorized = root.find("authorized");
        if (!authorized || authorized->k != json_value::kind::boolean || !authorized->b) {
                err = server_error();
                if (err.empty()) {
                        err = "服务器未确认授权";
                }
                return false;
        }

        if (auto v = root.find("expiresInSeconds"); v && v->k == json_value::kind::number && v->num > 0) {
                // 夹到 int 范围再转:double -> int 的越界转换是 UB,而 expiresInSeconds
                // 是服务端给的值,不能假设它一定是个合理的秒数。
                constexpr auto max_ttl = static_cast<double>(std::numeric_limits<int>::max());
                expires_sec = v->num >= max_ttl ? std::numeric_limits<int>::max() : static_cast<int>(v->num);
        }
        return true;
}

/*
 * 把"某台服务器已授权到什么时候"记进会话缓存。
 * 服务端没给 expiresInSeconds 时退回到契约默认值(12 小时)。
 */
void note_access_granted(
        _Inout_ std::map<std::string, std::chrono::steady_clock::time_point> &granted,
        _In_ const std::string &host, _In_ int expires_sec)
{
        auto ttl = expires_sec > 0 ? std::chrono::seconds(expires_sec) : kAccessDefaultTtl;
        granted[host] = std::chrono::steady_clock::now() + ttl;
}

/* Build the device metadata map from GET /api/devices. */
mgmt::meta_map parse_devices(const json_value &root, const std::string &self_client_id, const std::string &self_name)
{
        mgmt::meta_map meta;

        auto ok_val = root.find("ok");
        if (!ok_val || ok_val->k != json_value::kind::boolean || !ok_val->b) {
                return meta;
        }
        auto devs = root.find("devices");
        if (!devs || devs->k != json_value::kind::array) {
                return meta;
        }

        for (const auto &item : devs->vals) {
                if (item.k != json_value::kind::object) {
                        continue;
                }
                auto busid = item.find("busid");
                if (!busid || busid->k != json_value::kind::string || busid->str.empty()) {
                        continue;
                }
                mgmt::meta_device dev;
                dev.busid = busid->str;

                auto to_wx = [](const json_value *v) {
                        if (!v || v->k != json_value::kind::string) {
                                return wxString();
                        }
                        return wxString::FromUTF8(v->str.data(), static_cast<size_t>(v->str.size()));
                };

                dev.display_name = to_wx(item.find("displayName"));
                dev.alias = to_wx(item.find("alias"));
                dev.remark = to_wx(item.find("remark"));
                dev.has_alias = !dev.alias.empty();

                /*
                 * 服务端"本机 USB 设备"列表包含未共享的设备(管理页上要显示
                 * 「开始共享」按钮),但客户端只应该看到已经共享出来的那些 ——
                 * USB/IP 的 DEVLIST 本来也只返回已 bind 的设备,这里必须保持
                 * 一致,否则客户端会凭空多出一批点不通的设备。
                 * 字段缺失时不置位(meta_device::shared 缺省 false),即"无法
                 * 证明已共享就不补行",宁可少显示也不显示错的。
                 */
                if (auto flag = item.find("shared"); flag && flag->k == json_value::kind::boolean) {
                        dev.shared = flag->b;
                }

                auto conns = item.find("connections");
                wxString owner;
                if (conns && conns->k == json_value::kind::array) {
                        /*
                         * attached_count 用服务端给的数组长度。本机 attached 时
                         * 一定在里面(count >= 1);unplugged + busy 时他人占用
                         * 也至少为 1。UI 用它拼"已连接 (N)"/"他人占用 (N)"。
                         */
                        dev.attached_count = conns->vals.size();

                        for (const auto &c : conns->vals) {
                                if (c.k != json_value::kind::object) {
                                        continue;
                                }
                                auto id = c.find("clientId");
                                auto name = c.find("name");
                                auto addr = c.find("address");
                                auto cid = (id && id->k == json_value::kind::string) ? id->str : std::string();
                                auto cname = (name && name->k == json_value::kind::string) ? name->str : std::string();
                                auto caddr = (addr && addr->k == json_value::kind::string) ? addr->str : std::string();

                                wxString one;
                                if (cid == self_client_id) {
                                        one = L"本机";
                                        if (!self_name.empty()) {
                                                one += L" (";
                                                one += wxString::FromUTF8(self_name);
                                                one += L")";
                                        }
                                } else {
                                        one = wxString::FromUTF8(cname);
                                        if (!caddr.empty()) {
                                                one += L" (";
                                                one += wxString::FromUTF8(caddr);
                                                one += L")";
                                        }
                                }
                                if (!owner.empty()) {
                                        owner += L"、";
                                }
                                owner += one;
                        }
                }
                dev.owner = std::move(owner);

                /*
                 * currentHolder is the single owner the server picked for us;
                 * prefer it when `connections` was absent (an older server) so
                 * "who do I have to wait for" still has an answer.
                 */
                if (auto holder = item.find("currentHolder");
                    holder && holder->k == json_value::kind::object) {
                        if (auto id = holder->find("clientId");
                            id && id->k == json_value::kind::string) {
                                dev.holder_id = id->str;
                        }
                        if (dev.owner.empty()) {
                                dev.owner = to_wx(holder->find("name"));
                        }
                }
                dev.busy = !dev.holder_id.empty() && dev.holder_id != self_client_id;

                // Waitlist, FIFO. Index 0 is the client the server will wake
                // next, so the column can say "下一位" instead of just a list.
                wxString queue;
                bool self_queued = false;
                if (auto pending = item.find("pendingQueue");
                    pending && pending->k == json_value::kind::array) {
                        int index = 0;
                        for (const auto &w : pending->vals) {
                                if (w.k != json_value::kind::object) {
                                        continue;
                                }
                                auto id = w.find("clientId");
                                auto wid = (id && id->k == json_value::kind::string) ? id->str : std::string();
                                wxString one;
                                if (wid == self_client_id) {
                                        one = index == 0 ? L"本机(下一位)" : L"本机";
                                        self_queued = true;
                                } else {
                                        auto name = to_wx(w.find("name"));
                                        if (name.empty()) {
                                                name = L"未命名客户端";
                                        }
                                        one = index == 0 ? name + L"(下一位)" : name;
                                }
                                if (!queue.empty()) {
                                        queue += L"、";
                                }
                                queue += one;
                                ++index;
                        }
                }
                dev.queue = std::move(queue);
                dev.self_queued = self_queued;
                meta.emplace(std::move(dev.busid), std::move(dev));
        }

        return meta;
}

/*
 * 解析心跳响应里的 queueNotifications:
 *   [{"busid":"1-1","action":"attach"}, {"busid":"1-2","action":"dropped"}]
 * action == "attach" 表示"设备空出来了,轮到你了",GUI 线程据此发起连接;
 * action == "dropped" 表示设备被管理员取消共享,只需提示、不要重试。
 *
 * 服务端已经在下发时把这个 mailbox 清空(一次性),所以同一个通知不会重复触发;
 * 这里只负责语法层面的容错,遇到不认识的结构直接跳过而不是整包丢弃。
 */
std::vector<mgmt::queue_notice> parse_queue_notices(const json_value &root)
{
        std::vector<mgmt::queue_notice> notices;

        auto list = root.find("queueNotifications");
        if (!list || list->k != json_value::kind::array) {
                return notices;
        }

        for (const auto &item : list->vals) {
                if (item.k != json_value::kind::object) {
                        continue;
                }
                auto busid = item.find("busid");
                auto action = item.find("action");
                if (!busid || busid->k != json_value::kind::string || busid->str.empty()) {
                        continue;
                }
                if (!action || action->k != json_value::kind::string) {
                        continue;
                }
                if (action->str != "attach" && action->str != "dropped") {
                        continue;
                }
                notices.push_back(mgmt::queue_notice { busid->str, action->str });
        }

        return notices;
}

struct winhttp_handle
{
        HINTERNET value{};

        ~winhttp_handle()
        {
                if (value) {
                        WinHttpCloseHandle(value);
                }
        }

        winhttp_handle() = default;
        explicit winhttp_handle(HINTERNET handle) : value(handle) {}
        winhttp_handle(const winhttp_handle&) = delete;
        winhttp_handle& operator = (const winhttp_handle&) = delete;

        operator HINTERNET() const noexcept { return value; }
};

bool is_ip_literal(const std::string &value)
{
        IN_ADDR v4{};
        IN6_ADDR v6{};
        return InetPtonA(AF_INET, value.c_str(), &v4) == 1 ||
               InetPtonA(AF_INET6, value.c_str(), &v6) == 1;
}

/*
 * Ask a small HTTPS "what is my IP" service for the address seen outside the
 * local NAT. This is intentionally best-effort: the USB/IP connection and
 * server heartbeat must keep working when the public-IP service is blocked.
 *
 * Query only on the first successful management connection in this process.
 * Later heartbeats reuse the result, including an empty failed result.
 */
std::string fetch_public_ip_from(const wchar_t *host)
{
        auto fail = [host](const wchar_t *stage, DWORD err = GetLastError()) -> std::string {
                wxLogVerbose(L"公网 IP 查询 %s 失败(%s):Win32=%lu",
                        host, stage, err);
                SetLastError(0);
                return {};
        };

        winhttp_handle session {
                WinHttpOpen(L"USB-SHARE/0.9.8.0",
                        WINHTTP_ACCESS_TYPE_AUTOMATIC_PROXY,
                        WINHTTP_NO_PROXY_NAME, WINHTTP_NO_PROXY_BYPASS, 0)
        };
        if (!session) {
                return fail(L"WinHttpOpen");
        }

        WinHttpSetTimeouts(session, 2000, 2000, 3000, 3000);

        winhttp_handle connection {
                WinHttpConnect(session, host, INTERNET_DEFAULT_HTTPS_PORT, 0)
        };
        if (!connection) {
                return fail(L"WinHttpConnect");
        }

        winhttp_handle request {
                WinHttpOpenRequest(connection, L"GET", L"/", nullptr,
                        WINHTTP_NO_REFERER, WINHTTP_DEFAULT_ACCEPT_TYPES,
                        WINHTTP_FLAG_SECURE)
        };
        if (!request) {
                return fail(L"WinHttpOpenRequest");
        }

        constexpr wchar_t headers[] = L"Accept: text/plain\r\n";
        if (!WinHttpSendRequest(request, headers, static_cast<DWORD>(-1L),
                WINHTTP_NO_REQUEST_DATA, 0, 0, 0)) {
                return fail(L"WinHttpSendRequest");
        }
        if (!WinHttpReceiveResponse(request, nullptr)) {
                return fail(L"WinHttpReceiveResponse");
        }

        DWORD status = 0;
        DWORD status_size = sizeof(status);
        if (WinHttpQueryHeaders(request,
                WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER,
                WINHTTP_HEADER_NAME_BY_INDEX, &status, &status_size,
                WINHTTP_NO_HEADER_INDEX)) {
                if (status != 200) {
                        wxLogVerbose(L"公网 IP 查询 %s 非 200 状态:HTTP %lu",
                                host, status);
                        return {};
                }
        }
        // 没读到状态头也放行:某些服务用 chunked 不带 status,后面会校验
        // 响应是不是合法 IP literal。

        std::string response;
        response.reserve(64);
        for (;;) {
                DWORD available = 0;
                if (!WinHttpQueryDataAvailable(request, &available) || !available) {
                        break;
                }

                constexpr DWORD kMaxResponse = 128;
                auto remaining = kMaxResponse - static_cast<DWORD>(response.size());
                if (!remaining) {
                        break;
                }
                auto to_read = std::min(available, remaining);
                char buffer[kMaxResponse]{};
                DWORD read = 0;
                if (!WinHttpReadData(request, buffer, to_read, &read) || !read) {
                        break;
                }
                response.append(buffer, read);
        }

        response = trim(response);
        if (!is_ip_literal(response)) {
                wxLogVerbose(L"公网 IP 查询 %s 响应不是合法 IP literal: %s",
                        host, wxString::FromUTF8(response));
                return {};
        }
        wxLogVerbose(L"公网 IP 查询 %s 成功: %s", host, wxString::FromUTF8(response));
        return response;
}

/*
 * Try several "what is my IP" services in order until one returns a valid
 * global IP literal. Different networks block different hosts (DNS pollution,
 * captive portals, corporate proxies), so a single endpoint is fragile.
 *
 * Order rationale:
 *   - api64.ipify.org first: returns the actual outbound address, IPv6 if
 *     the network has it, IPv4 otherwise
 *   - api.ipify.org: same operator's IPv4-only endpoint
 *   - ifconfig.co / icanhazip.com / ifconfig.me: independent operators with
 *     different hosting/CDN footprints, gives us coverage when ipify is
 *     blocked but the others aren't
 */
std::string fetch_public_ip()
{
        static constexpr const wchar_t *kEndpoints[] = {
                L"api64.ipify.org",
                L"api.ipify.org",
                L"ifconfig.co",
                L"icanhazip.com",
                L"ifconfig.me",
        };

        for (const wchar_t *host : kEndpoints) {
                if (auto ip = fetch_public_ip_from(host); !ip.empty()) {
                        return ip;
                }
        }
        wxLogVerbose(L"公网 IP 查询全部 %zu 个端点失败,后续心跳不带 publicIp 字段",
                std::size(kEndpoints));
        return {};
}

std::string make_heartbeat_body(
        const mgmt::settings &cfg,
        const std::map<std::string, bool> &attached,
        const std::set<std::string> &queued)
{
        std::string body = "{\"clientId\":\"";
        body += json_escape(cfg.client_id);
        body += "\",\"clientName\":\"";
        body += json_escape(cfg.client_name);
        if (!cfg.public_ip.empty()) {
                body += "\",\"publicIp\":\"";
                body += json_escape(cfg.public_ip);
        }
        body += "\",\"dataPort\":";
        body += std::to_string(cfg.data_port);
        body += ",\"busids\":[";
        bool first = true;
        for (const auto &[busid, attached_flag] : attached) {
                if (!attached_flag) {
                        continue;
                }
                if (!first) {
                        body += ',';
                }
                first = false;
                body += '"';
                body += json_escape(busid);
                body += '"';
        }
        body += ']';

        /*
         * 排队意愿:这些设备我想用,但当前被别的客户端占用,请在服务端排一个
         * FIFO 位。客户端每拍心跳都重复声明,服务端按 clientId 去重并维持顺序;
         * 设备空出来时服务端会在心跳响应里回发 queueNotifications,由
         * mgmt_loop 解析后触发真正的 attach。
         *
         * 空数组也要发:它同时是"我已不再排队"的隐含表达(设备一 attach 成功,
         * 服务端就会凭 busids 把该 clientId 从队列里摘掉)。
         */
        body += ",\"enqueueBusids\":[";
        bool first_queued = true;
        for (const auto &busid : queued) {
                if (!first_queued) {
                        body += ',';
                }
                first_queued = false;
                body += '"';
                body += json_escape(busid);
                body += '"';
        }
        body += "]}";
        return body;
}

/*
 * (构造主动告别包 + MainFrame::mgmt_send_goodbye 已移到下方匿名 namespace 外,
 * 成员函数不能在匿名 namespace 内定义 —— 编译错误 C2888。)
 */

std::string make_client_id()
{
        GUID guid{};
        if (CoCreateGuid(&guid) != S_OK) {
                return "wusbip-default-client";
        }
        const auto *p = reinterpret_cast<const unsigned char*>(&guid);
        char hex[33];
        for (int i = 0; i < 16; ++i) {
                std::snprintf(hex + i * 2, 3, "%02x", p[i]);
        }
        return std::string(hex, 32);
}

} // namespace

/*
 * 配置迁移实现,详见 mgmt.h 的声明。
 *
 * 放在匿名 namespace 之外(要对外导出),但仍可直接调用其内的 cfg_root() ——
 * 同一翻译单元内匿名 namespace 的成员在文件作用域可见。
 */
void mgmt::migrate_legacy_config()
{
        int migrated = 0;

        {
                wxLogNull no_log; // 注册表里有脏类型时不要弹日志对话框

                wxRegKey legacy(wxRegKey::HKCU, win::legacy_config_registry_root);
                if (!legacy.Exists() || !legacy.Open(wxRegKey::Read)) {
                        return; // 没有旧键:新电脑首次安装,无需迁移
                }

                wxRegKey now(wxRegKey::HKCU, cfg_root());
                if (now.Exists()) {
                        if (!now.Open(wxRegKey::Write)) {
                                return;
                        }
                } else if (!now.Create()) {
                        return;
                }

                // 字符串项
                for (auto name : { L"ServerAddr", L"ClientId", L"ClientName" }) {
                        if (now.HasValue(name) || !legacy.HasValue(name)) {
                                continue; // 已有值不覆盖;旧键没这一项就跳过
                        }
                        wxString s;
                        if (legacy.QueryValue(name, s) && !s.empty()) {
                                now.SetValue(name, s);
                                ++migrated;
                        }
                }

                // 整数项:容忍旧版写成 REG_SZ 的脏数据
                for (auto name : { L"ServerPort", L"MgmtEnabled", L"WebPort" }) {
                        if (now.HasValue(name) || !legacy.HasValue(name)) {
                                continue;
                        }

                        long v = 0;
                        bool ok = false;
                        switch (legacy.GetValueType(name)) {
                        case wxRegKey::Type_Dword:
                                ok = legacy.QueryValue(name, &v);
                                break;
                        case wxRegKey::Type_String: {
                                wxString s;
                                ok = legacy.QueryValue(name, s) && s.ToLong(&v);
                                break;
                        }
                        default:
                                break;
                        }

                        if (ok) {
                                now.SetValue(name, v);
                                ++migrated;
                        }
                }
        }

        if (migrated) {
                wxLogVerbose(
                        wxString::Format(L"已从旧品牌配置路径迁移 %d 项设置到 %s", migrated, cfg_root()));
        }
}

/*
 * 构造主动告别包 —— 在普通心跳 body 末尾追加 "shutdown":true。
 * 服务端 update_client() 收到该字段后会:
 *   - 立即从 clients.json 删除该 clientId
 *   - 对每个 claimed busid 调 kick_device() (unbind+bind)
 * 等同于在管理页面点一次「强制下线」,但 0 延迟。
 *
 * 不能只发空 busids + shutdown:true —— 服务端需要知道要 unbind 哪些设备,
 * 必须带上当前 m_mgmt_attached 里 attached=true 的 busid 清单。
 */
std::string make_goodbye_body(const mgmt::settings &cfg, const std::map<std::string, bool> &attached)
{
        // 告别包不声明排队意愿:进程马上退出,再排队也没有意义,服务端还会
        // 因为 shutdown 分支直接返回、不处理 enqueue 字段。
        auto body = make_heartbeat_body(cfg, attached, {});
        // heartbeat body 以 '}' 收尾,直接替换为 ',...}' 即可,不需要重序列化。
        body.pop_back();
        body += ",\"shutdown\":true}";
        return body;
}

/*
 * 主动告别:在 on_close() / ~MainFrame() 中调用,把当前仍 attached 的设备清单
 * + shutdown:true 一起 POST 给服务端。服务端 update_client() 看到 shutdown
 * 字段会立即把 clientId 从 clients.json 删除并对每个 busid 调 kick_device(),
 * 0 延迟释放设备。
 *
 * 同步阻塞,最多等 2s (http_exchange 内部 connect select 超时)。on_close
 * 流程能容忍这点延迟;但不能在 detach 线程里做 —— 主线程随后会 WSACleanup,
 * detached 线程再用 socket 会立刻崩。
 */
void MainFrame::mgmt_send_goodbye()
{
        mgmt::settings cfg;
        std::map<std::string, bool> attached;
        {
                std::lock_guard<std::mutex> lock(m_mgmt_mtx);
                cfg = m_mgmt_cfg;
                attached = m_mgmt_attached;
        }

        if (!cfg.enabled || cfg.host.empty()) {
                // 管理接口未启用 / 未配置服务器 —— 没什么可告别的。
                return;
        }

        // 候选端口:web_port (即服务端 HTTP 管理端口,与 data_port 同值),
        // 再加上 m_mgmt_loop 已经验证过的成功端口作为兜底。
        std::vector<int> candidates;
        candidates.push_back(cfg.web_port);
        if (std::find(candidates.begin(), candidates.end(), cfg.data_port) == candidates.end()) {
                candidates.push_back(cfg.data_port);
        }
        auto ok_port = m_mgmt_ok_port.load();
        if (ok_port > 0 && std::find(candidates.begin(), candidates.end(), ok_port) == candidates.end()) {
                candidates.push_back(ok_port);
        }

        // 同步发送 —— on_close 流程能容忍最多 2s 阻塞(connect select 超时)。
        // 不能 detach:主线程随后会 WSACleanup,detached 线程若仍在用 socket
        // 会立即收到 WSAEINPROGRESS/WSAESHUTDOWN,而且 detached 线程访问的
        // 任何 this 成员都是 use-after-free。
        for (auto port : candidates) {
                mgmt::settings c2 = cfg;
                c2.web_port = port;
                bool ok = false;
                http_post_json(c2, "/api/clients/heartbeat", make_goodbye_body(c2, attached), ok);
                if (ok) {
                        return; // 收到完整 200 响应即视为告别成功
                }
        }
}

void MainFrame::mgmt_start()
{
        if (m_mgmt_thread.joinable()) {
                return;
        }

        int enabled = 1;
        cfg_read_int(L"MgmtEnabled", 1, enabled); // 静默读取,容忍旧版 REG_SZ 脏数据
        m_mgmt_enabled = enabled != 0;

        if (auto itm = m_menu_file->FindItem(ID_MGMT_ENABLE)) {
                itm->Check(m_mgmt_enabled);
        }

        WSADATA wsa{};
        if (WSAStartup(MAKEWORD(2, 2), &wsa) != 0) {
                wxLogStatus(L"管理接口不可用:Winsock 初始化失败");
                return;
        }
        m_wsa_started = true; // 与 mgmt_stop() 的 WSACleanup 配对

        m_mgmt_stop = false;
        m_mgmt_thread = std::thread(&MainFrame::mgmt_loop, this);
        mgmt_capture_settings();
}

void MainFrame::mgmt_stop()
{
        if (m_mgmt_thread.joinable()) {
                m_mgmt_stop = true;
                m_mgmt_thread.join();
        }

        /*
         * WSAStartup 每调用一次都会给本进程的 Winsock 引用计数加一。
         * 旧代码只 Start 不 Clean,反复勾选/取消"服务器管理接口"会让计数
         * 一路累加(表现为句柄与内核资源缓慢泄漏)。这里做配对清理。
         */
        if (m_wsa_started) {
                m_wsa_started = false;
                WSACleanup();
        }
}

void MainFrame::mgmt_capture_settings()
{
        mgmt::settings cfg;
        cfg.enabled = m_mgmt_enabled;

        if (auto box = m_comboBoxServer) {
                cfg.host = box->GetValue().utf8_string();
                cfg.host = trim(cfg.host);
                lower_ascii(cfg.host);
        }
        if (auto spin = m_spinCtrlPort) {
                cfg.data_port = spin->GetValue();
        }

        bool changed_host = cfg.host != m_mgmt_cfg.host;

        /*
         * 默认 web_port 与数据端口一致:服务端 (Phase B 单端口网关) 把 USB/IP 协议
         * 与管理 HTTP 共用一个对外端口。客户端不需要再单独访问一个独立的"管理端口" —
         * 数据端口 + HTTP 协议首字节网关 已经在容器里分流。
         *
         * 老版默认 18080 是独立管理端口,迁到单端口方案后用户首次连接会一直连不上;
         * mgmt_loop 的 fallback 只在 web_port != data_port 时才生效,若两个都填错
         * 就完全收不到心跳 —— 服务端 clients.json 永远是空的,UI 自然看不到任何
         * 客户端/连接方/公网 IP。
         *
         * 遗留的 wxConfig WebPort=18080 直接当不存在处理,避免误导本次连接。
         */
        int web_port = cfg.data_port;
        if (!cfg_read_int(L"WebPort", cfg.data_port, web_port)) {
                web_port = cfg.data_port;
        }
        if (web_port == 18080 && web_port != cfg.data_port) {
                // 老版独立管理端口,单端口方案下等于无效 —— 丢弃
                web_port = cfg.data_port;
                cfg_delete_value(L"WebPort");
        }
        if (web_port < 1 || web_port > 65535) {
                web_port = cfg.data_port;
        }
        cfg.web_port = web_port;

        if (auto c = wxConfig::Get()) {
                wxLogNull no_log; // 字符串值遇到类型脏数据时也不弹日志对话框
                auto token = c->Read(L"WebToken", wxString());
                cfg.token = token.utf8_string();

                auto id = c->Read(L"ClientId", wxString());
                if (id.empty()) {
                        id = wxString::FromUTF8(make_client_id());
                        c->Write(L"ClientId", id);
                }
                cfg.client_id = id.utf8_string();
        }

        // 自愈:把旧版遗留的 REG_SZ 型数值规范化为 REG_DWORD,一劳永逸
        cfg_repair_int(L"MgmtEnabled", m_mgmt_enabled ? 1 : 0);
        cfg_repair_int(L"WebPort", cfg.web_port);

        auto name = wxGetHostName();
        if (auto c = wxConfig::Get()) {
                wxString saved;
                if (c->Read(L"ClientName", &saved) && !saved.empty()) {
                        name = saved; // 用户在「文件 → 本机名称…」设置的名字
                }
        }
        cfg.client_name = name.empty() ? std::string("Windows 客户端") : name.utf8_string();

        /*
         * 「共享访问密码」:从本机(DPAPI 加密)的存储里取一份,供两条路径用:
         *   1. 主动闸门 mgmt_ensure_access():本次会话还没授权时先静默试一次,
         *      省掉一次弹框;
         *   2. 后台心跳线程:服务端授权(默认 12 小时)过期后自动静默续期。
         * 没保存过就是空串 —— 连接前会弹框让用户输入。
         *
         * 只在 host 非空时读:空 host 会归一成 "AccessPwd_" 这个无意义的值名。
         */
        if (!cfg.host.empty()) {
                wxString saved_password;
                if (win::load_access_password(wxString::FromUTF8(cfg.host), saved_password)) {
                        cfg.access_password = saved_password.utf8_string();
                }
        }

        // 配置(服务器/端口/本机名称/开关)变化时唤醒后台线程立即拉取元数据,
        // 让设备名称/服务器备注/占用方尽快显示,不必等 10 秒轮询间隔
        m_mgmt_kick.store(true);

        {
                std::lock_guard<std::mutex> lock(m_mgmt_mtx);
                m_mgmt_cfg = std::move(cfg);
                if (changed_host) {
                        m_mgmt_attached.clear();
                        // 排队位和占用方元数据都属于旧服务器:换了地址以后
                        // 继续声明旧的 busid 只会往新服务器上写一批无意义的队列项。
                        m_mgmt_queued.clear();
                        m_mgmt_meta.clear();
                        // 补行记录同样作废:删除时要靠它精确回收,留着会误删新服务器
                        // 上恰好同名 busid 的行。
                        m_mgmt_only_rows.clear();
                        m_mgmt_ok_port.store(0); // 服务器变了,之前记住的端口不再适用
                }
        }
}

namespace
{

/*
 * 与 mgmt_capture_settings() 同源:保证本机 ClientId 已生成并落盘。
 * 闸门可能在 ClientId 还没落盘时被调用(理论上),这里兜一层底。
 */
std::string get_or_make_client_id()
{
        if (auto c = wxConfig::Get()) {
                wxLogNull no_log;
                auto id = c->Read(L"ClientId", wxString());
                if (id.empty()) {
                        id = wxString::FromUTF8(make_client_id());
                        c->Write(L"ClientId", id);
                }
                return id.utf8_string();
        }
        return make_client_id();
}

/*
 * 把 "host:port" 形式的服务器 URL 解析成一份可直接发 HTTP 的管理接口配置。
 *
 * 以 base(调用方缓存的 m_mgmt_cfg)为底 —— clientId / clientName / token 这些
 * "本机身份"字段沿用 —— 只覆盖 host 和端口。必须覆盖:闸门可能在"用户刚在
 * 地址栏敲了一个新地址、还没走完保存流程"时被调用,那时 m_mgmt_cfg.host 还是
 * 旧值,拿它去探测会问错服务器。
 *
 * 解析规则与 attach() 完全一致(都走 usbip::split_server_url),保证闸门问的
 * 就是接下来真正要连的那台服务器。
 */
mgmt::settings make_access_settings(_In_ const mgmt::settings &base, _In_ const wxString &url)
{
        auto cfg = base;

        wxString hostname;
        wxString service;
        if (!usbip::split_server_url(url, hostname, service)) {
                return cfg; // 没有端口:沿用 base 里的配置
        }

        auto host = trim(hostname.utf8_string());
        lower_ascii(host);
        if (host.empty()) {
                return cfg;
        }
        cfg.host = std::move(host);

        int port{};
        parse_port(service.utf8_string(), port);
        if (port > 0) {
                cfg.web_port = port;
                cfg.data_port = port;
        }

        if (cfg.client_id.empty()) {
                cfg.client_id = get_or_make_client_id();
        }
        if (cfg.client_name.empty()) {
                // m_mgmt_cfg 还没被 mgmt_capture_settings() 填过(理论上)时兜底:
                // 契约要求 clientName 非空,否则服务端管理页只能显示一个空占用方。
                auto name = wxGetHostName();
                cfg.client_name = name.empty() ? std::string("Windows 客户端") : name.utf8_string();
        }
        return cfg;
}

/* mgmt_ensure_access() 的重入保护:析构时复位,异常安全。 */
struct prompting_guard
{
        bool &flag;

        explicit prompting_guard(_Inout_ bool &f) : flag(f) { flag = true; }
        ~prompting_guard() { flag = false; }

        prompting_guard(const prompting_guard&) = delete;
        prompting_guard& operator=(const prompting_guard&) = delete;
};

} // namespace


/*
 * 「共享访问密码」闸门 —— 所有"要连服务器"的入口(拉设备列表、attach)在动手
 * 之前都必须先过这里。
 *
 * 硬要求(第一优先级):服务端没启用访问控制时,这里必须**完全无副作用地放行**
 * —— 不弹框、不改状态、不产生任何提示,行为与改造前逐字一致。因此所有"探测
 * 失败 / 看不懂响应"的分支都按"不需要密码"处理:老服务端根本没有 /api/access
 * 这个端点(返回 404),我们必须照旧连上去。
 *
 * @param url         "host:port" 形式的服务器地址
 * @param interactive 是否允许弹密码框。false 时只用已保存的密码静默尝试一次,
 *                    失败就直接返回 false(供不想弹模态框的调用方使用)。
 * @return true  = 可以继续(服务端未启用访问控制,或本机已授权)
 *         false = 需要密码但没拿到,调用方必须中止本次操作并给出明确提示,
 *                 不能静默失败
 */
bool MainFrame::mgmt_ensure_access(_In_ const wxString &url, _In_ bool interactive)
{
        if (url.empty()) {
                return true;
        }

        mgmt::settings base;
        {
                std::lock_guard<std::mutex> lock(m_mgmt_mtx);
                base = m_mgmt_cfg;
        }

        auto cfg = make_access_settings(base, url);
        if (cfg.host.empty()) {
                return true; // 没配服务器:交给原有逻辑去报错,这里不拦
        }

        /*
         * 重入保护。下面会弹模态对话框,而模态框跑的是嵌套事件循环 —— 期间
         * wxThreadEvent 仍会被派发,mgmt_apply_event() 可能再次走到这里。
         * 没有这个标志就会套娃出第二个框;更糟的是 usbip::run_cancellable()
         * 用的是进程级共享事件对象,嵌套使用会互相踩。
         */
        if (m_access_prompting) {
                return false;
        }

        const auto now = std::chrono::steady_clock::now();
        if (auto it = m_access_authorized.find(cfg.host);
            it != m_access_authorized.end() && now < it->second) {
                return true; // 本次会话已授权(或刚探测过"不需要密码")
        }

        prompting_guard guard{ m_access_prompting };

        // 1) 这台服务器要不要访问密码?
        access_probe probe;
        usbip::run_cancellable(this,
                wxString::Format(L"正在检查 %s 是否需要访问密码…", url),
                L"服务器访问密码",
                [&probe, &cfg] { probe = probe_access(cfg); });

        if (!probe.reachable || !probe.required) {
                /*
                 * 老服务器(没有 /api/access)或明确 required=false —— 直接放行。
                 * 只对"确实探测到了"的情况记一个短 TTL,避免一次批量 attach 对
                 * 同一台服务器反复探测;探测不到的(网络不通)不缓存,免得把一次
                 * 偶发失败记成"这台服务器永远不需要密码"。
                 */
                if (probe.reachable) {
                        m_access_authorized[cfg.host] = std::chrono::steady_clock::now() + kAccessProbeTtl;
                }
                return true;
        }

        // 2) 需要密码:先用本机保存过的密码静默试一次。这一步同时覆盖
        //    "本次会话还没授权"和"服务端授权(默认 12 小时)刚过期"两种情况。
        if (!cfg.access_password.empty()) {
                std::string err;
                int expires = 0;
                bool granted = false;
                bool rejected = false;

                usbip::run_cancellable(this,
                        wxString::Format(L"正在验证 %s 的访问密码…", url),
                        L"服务器访问密码",
                        [&] { granted = authorize_access(cfg, cfg.access_password, err, expires, &rejected); });

                if (granted) {
                        note_access_granted(m_access_authorized, cfg.host, expires);
                        return true;
                }

                /*
                 * 只有服务端**明确拒绝**了密码(401/403)才清掉本地保存的那一份 ——
                 * 管理员改过密码时它会一直失效,留着只会每次连接都拿废密码去撞。
                 *
                 * 网络不通 / 5xx / 响应格式错时绝不能清:服务器抖一下就把用户存好
                 * 的密码删了,下次还得重新输,属于把偶发故障变成永久损失。
                 */
                wxLogVerbose(L"%s 已保存的访问密码验证失败(%s):%s", url,
                             rejected ? L"服务端拒绝" : L"本次未能确认", wxString::FromUTF8(err));
                if (rejected) {
                        win::erase_access_password(wxString::FromUTF8(cfg.host));
                }
        }

        if (!interactive) {
                return false;
        }

        // 3) 弹框让用户输入。
        return mgmt_prompt_access_password(cfg);
}

/*
 * 弹一次访问密码输入框,把输入交给 /api/access/authorize 校验;校验通过就
 * 记住授权,并把密码经 DPAPI 加密后落盘。校验失败时显示服务端给的文案
 * (契约里 403 是"访问密码不正确")并允许重试或取消。
 *
 * 对话框风格与工程里已有的 wxTextEntryDialog / wxMessageDialog 用法一致
 * (见 wusbip.cpp 的 edit_column_dlg()),密码框用 wxTE_PASSWORD。
 *
 * 调用方必须已经持有 mgmt_ensure_access() 的重入保护。
 *
 * @return true = 拿到了有效授权;false = 用户取消
 */
bool MainFrame::mgmt_prompt_access_password(_In_ const mgmt::settings &cfg)
{
        const auto host = wxString::FromUTF8(cfg.host);
        const auto caption = wxString::Format(L"%s - 访问密码", host);

        for (;;) {
                wxTextEntryDialog dlg(this,
                        wxString::Format(L"服务器 %s 启用了共享访问密码。\n\n请输入访问密码:", host),
                        caption, wxEmptyString, wxTextEntryDialogStyle | wxTE_PASSWORD);

                if (dlg.ShowModal() != wxID_OK) {
                        return false; // 用户取消
                }

                auto password = dlg.GetValue();
                if (password.empty()) {
                        continue; // 空密码不可能通过,直接重新问
                }

                std::string err;
                int expires = 0;
                bool granted = false;
                auto password_u8 = password.utf8_string();

                usbip::run_cancellable(this,
                        wxString::Format(L"正在验证 %s 的访问密码…", host),
                        L"服务器访问密码",
                        [&] { granted = authorize_access(cfg, password_u8, err, expires); });

                if (granted) {
                        if (!win::save_access_password(host, password)) {
                                wxLogVerbose(L"访问密码保存失败,下次连接仍需重新输入");
                        }
                        /*
                         * 同步进内存配置:后台心跳线程的"授权到期静默续期"读的是
                         * m_mgmt_cfg.access_password(mgmt_loop 每轮加锁拷一份),
                         * 不更新的话它在本进程内一直是空串 —— 静默续期必然失败,
                         * 授权一过期就只能把用户再问一遍。
                         */
                        {
                                std::lock_guard<std::mutex> lock(m_mgmt_mtx);
                                m_mgmt_cfg.access_password = password_u8;
                        }
                        note_access_granted(m_access_authorized, cfg.host, expires);
                        set_status_text(wxString::Format(L"%s 的访问密码已验证", host));
                        return true;
                }

                auto reason = err.empty() ? wxString(L"访问密码不正确") : wxString::FromUTF8(err);

                /*
                 * 失败原因优先用服务端给的文案(契约里 403 就是"访问密码不正确"),
                 * 拿不到才用本地兜底。按钮文案用 SetOKCancelLabels 定制成
                 * 「重试 / 取消」;某些平台/主题下系统不认自定义文案,所以正文里
                 * 也把两个选择写清楚,不依赖按钮文字。
                 */
                wxMessageDialog md(this,
                        wxString::Format(L"%s\n\n是否重新输入访问密码?\n选「重试」继续输入,选「取消」放弃本次连接。", reason),
                        caption, wxOK | wxCANCEL | wxICON_ERROR);
                md.SetOKCancelLabels(L"重试", L"取消");

                if (md.ShowModal() != wxID_OK) {
                        return false;
                }
        }
}

/*
 * 被动发现:任何管理接口响应里出现 401 + accessRequired(心跳、设备列表)时,
 * 由 GUI 线程走这里 —— 不能只打一条日志了事。
 *
 * 与主动闸门共用同一套逻辑(mgmt_ensure_access),区别只有两点:
 *   1. 用户在这里取消后会"安静一会儿"(kAccessPromptSnooze),否则后台每 30 秒
 *      一次的轮询会不停把同一个模态框糊到用户脸上;用户主动连接/刷新不受此限。
 *   2. 通过后立刻踢一次心跳线程,让设备信息尽快恢复,不用等下一个轮询周期。
 */
void MainFrame::mgmt_on_access_required()
{
        mgmt::settings cfg;
        {
                std::lock_guard<std::mutex> lock(m_mgmt_mtx);
                cfg = m_mgmt_cfg;
        }

        if (cfg.host.empty() || m_access_prompting) {
                return;
        }

        if (auto now = std::chrono::steady_clock::now(); now < m_access_prompt_snooze) {
                set_status_text(L"服务器需要访问密码,设备信息暂不可用");
                return;
        }

        /*
         * 用后台线程最后成功过的那个端口(而不是配置里的 web_port)去探测:
         * 老服务端上管理接口可能挂在独立端口上,mgmt_loop 会在数据端口上做
         * 回退,这时只有 m_mgmt_ok_port 反映的是真正通的那一个。端口没记录
         * 过就退回 web_port(单端口模式下两者本来就相同)。
         */
        int port = m_mgmt_ok_port.load();
        if (port < 1 || port > 65535) {
                port = cfg.web_port;
        }

        auto url = wxString::Format(L"%s:%d", wxString::FromUTF8(cfg.host), port);

        /*
         * 先作废本机的会话授权缓存,再走闸门。
         *
         * 能走到这里说明服务端刚刚明确回过 401 —— 本机在服务端那边已经不算授权了
         * (典型:服务端重启或改密码,把已发出的授权作废了)。而缓存里那份"授权到
         * 某某时刻"最长有 12 小时(kAccessDefaultTtl),不删掉的话
         * mgmt_ensure_access() 会命中缓存直接 return true:既不弹框也不重新授权,
         * 表现为后台每 10s 一轮 401、状态栏反复显示"访问密码已验证,正在恢复设备
         * 信息",设备信息却永远回不来。
         *
         * 键的形态必须与闸门写入缓存时完全一致:闸门用的是 make_access_settings()
         * 从 url 解析出的 hostname(去端口、trim、转小写)。这里刻意调用同一个函数
         * 推导,而不是直接用 m_mgmt_cfg.host —— 后者可能带 ":port"(用户把端口一起
         * 敲进地址栏),形态不一致就 erase 不掉,缓存照样命中。
         */
        m_access_authorized.erase(make_access_settings(cfg, url).host);

        if (mgmt_ensure_access(url, true)) {
                m_mgmt_kick.store(true); // 立刻重试,不必等下一个轮询周期
                set_status_text(L"访问密码已验证,正在恢复设备信息");
        } else {
                m_access_prompt_snooze = std::chrono::steady_clock::now() + kAccessPromptSnooze;
                set_status_text(L"已取消:未提供服务器访问密码,设备信息暂不可用");
        }
}

void MainFrame::mgmt_note_state(const usbip::device_state &st)
{
        auto &loc = st.device.location;
        if (loc.hostname.empty() || loc.busid.empty()) {
                return;
        }

        auto attached = st.state == usbip::state::connected || st.state == usbip::state::plugged;

        /*
         * state::disconnected 是**二义**的:它既可能是"已经建立的会话断了"
         * (drivers/ude/device.cpp:766),也可能是"一次 attach 尝试失败了"
         * (drivers/ude/vhci_ioctl.cpp:517 的 workitem_cleanup —— 驱动先发
         * state::connected 再去 import_remote_device(),失败时最后一条事件
         * 就是这个 disconnected,见同文件 :299 与 :301 的先后关系)。
         *
         * 后者会把明明还挂着的设备记成"未连接":UI 显示空闲/未连接、心跳
         * busids 为空,用户再点「连接」只会再撞一次 ST_DEV_BUSY("设备忙"),
         * 而这个状态不会自愈(只有 on_reload() 会重新读驱动列表)。这里直接
         * 问驱动要真相:驱动列表说它还挂着,就保持 attached。
         *
         * 只对 disconnected 兜底:unplugging/unplugged 是驱动明确在拆除设备
         * (drivers/ude/device.cpp:590/600),一律按未连接处理 —— 否则真正断开
         * 时状态会卡在"已连接"。
         */
        if (!attached && st.state == usbip::state::disconnected) {
                attached = mgmt_sync_attached(loc.hostname, loc.busid);
        }

        std::string host = loc.hostname;
        lower_ascii(host);

        std::lock_guard<std::mutex> lock(m_mgmt_mtx);
        if (host != m_mgmt_cfg.host) {
                return; // heartbeat only concerns the configured server
        }
        m_mgmt_attached[loc.busid] = attached;
}

/*
 * 用驱动层的权威列表校正本机的"已连接"状态。
 *
 * 【为什么必须有这个函数】
 *
 * 本机"我连了哪些设备"此前只有两个来源:驱动的 DeviceStateEvent
 * (on_device_state -> mgmt_note_state)和 on_reload()。而那条事件流**不能**
 * 用来回答这个问题,因为驱动在 connected() 里先发 state::connected,之后才去
 * import_remote_device()(drivers/ude/vhci_ioctl.cpp:299 vs :301)——
 * state::connected 只代表"TCP 连上了",不代表"导入成功";一次失败的 attach
 * 由 workitem_cleanup() 以 state::disconnected 收尾(同文件 :517)。
 *
 * 于是"失败的 attach 尝试"事件流是:
 *      connecting -> connected -> disconnected
 * 而 mgmt_note_state() 与状态列都是"最后一条事件说了算",结果把**还挂着的**
 * 设备记成没连。
 *
 * 实测复现(2026-10-14 20:29/20:30,本机 1-8.2):
 *   对已经挂着的设备再发一次 attach -> 服务端回 ST_DEV_BUSY("设备忙")
 *   客户端 verbose 日志:
 *      1-8.2 connecting   -> state '挂载中'
 *      1-8.2 connected    -> state '已连接'
 *      1-8.2 disconnected -> state '空闲'        <- 最终状态
 *   之后心跳 busids 持续为空(服务端管理页显示"未上报的客户端"),
 *   而 usbip port 全程是 "Port 01: device in use ... usbip://.../1-8.2"。
 *   用户看到的就是"明明连着,界面说没连,再点连接还报设备忙"。
 *
 * 【为什么只"补上"、不"清除"】
 * 置 false 由事件流负责:真正断开时驱动一定会发 unplugging/unplugged
 * (drivers/ude/device.cpp:590/600),事件流会把状态清掉。这里只补"驱动其实
 * 还挂着、本机却记成没连"的那一半,避免与事件流抢方向,也不会把"正在连接中"
 * 的设备误标成已连接 —— GET_IMPORTED_DEVICES 只列出**已经占用 hub 端口**的
 * 设备(drivers/ude/vhci_ioctl.cpp:746),正是"已连接"的定义。
 *
 * 【为什么可以随便调】
 * 用 vhci::open() 另开句柄,不与 attach() 抢共享句柄(见
 * attach_persistent_devices() 的注释);GET_IMPORTED_DEVICES 走驱动的
 * parallel 队列(drivers/ude/vhci_ioctl.cpp:925),不会被正在进行的 attach
 * 排在后面。
 *
 * @return 驱动层是否挂着 host/busid(仅 busid 非空时有意义)
 */
bool MainFrame::mgmt_sync_attached(_In_ const std::string &host, _In_ const std::string &busid)
{
        auto imported = usbip::vhci::get_imported_devices(usbip::vhci::open().get());
        if (!imported) {
                return false; // 查询失败:保持现有模型,绝不凭猜测改动
        }

        auto wanted = host;
        lower_ascii(wanted);

        std::lock_guard<std::mutex> lock(m_mgmt_mtx);

        bool found = false;

        for (auto &d: *imported) {
                if (!busid.empty() && d.location.busid != busid) {
                        continue;
                }

                std::string h = d.location.hostname;
                lower_ascii(h);
                if (h != wanted) {
                        continue;
                }

                /*
                 * 只有"配置服务器"上的设备才登记进模型 —— 与 mgmt_note_state()
                 * 同一个过滤条件,保证心跳里的 busids 只描述当前服务器。
                 * 命中判断本身不依赖管理接口是否启用:即使关掉管理接口,
                 * attach() 的重复连接检查照样有效。
                 */
                if (h == m_mgmt_cfg.host) {
                        m_mgmt_attached[d.location.busid] = true;
                }

                found = true;
        }

        return busid.empty() ? false : found;
}

void MainFrame::mgmt_on_enable(wxCommandEvent&)
{
        if (auto itm = m_menu_file->FindItem(ID_MGMT_ENABLE)) {
                m_mgmt_enabled = itm->IsChecked();
        }
        if (auto cfg = wxConfig::Get()) {
                cfg->Write(L"MgmtEnabled", m_mgmt_enabled);
        }
        mgmt_capture_settings();

        wxLogStatus(m_mgmt_enabled ? L"已启用服务器管理接口" : L"已停用服务器管理接口");
}

/*
 * 加入/退出某台设备的排队。
 *
 * 只改本机内存里的"排队意愿"集合并踢一次心跳线程,由心跳把 enqueueBusids
 * 带给服务端 —— 刻意不做一次性的"排队"POST,原因是:
 *   1. 排队是持续意愿而非一次性动作。客户端必须每拍都声明,服务端才能区分
 *      "还在等"和"已经不在了"(后者会因为心跳消失被清出队列)。
 *   2. 只保留心跳这一条通道,排队状态和连接状态就永远不会互相矛盾。
 *
 * @return false 表示管理接口未启用/未配置服务器,调用方应回退到普通 attach
 */
bool MainFrame::mgmt_device_busy(_In_ const std::string &busid)
{
        if (busid.empty()) {
                return false;
        }

        std::lock_guard<std::mutex> lock(m_mgmt_mtx);
        auto it = m_mgmt_meta.find(busid);
        return it != m_mgmt_meta.end() && it->second.busy;
}

void MainFrame::mgmt_clear_all_queues()
{
        bool had_any = false;
        {
                std::lock_guard<std::mutex> lock(m_mgmt_mtx);
                had_any = !m_mgmt_queued.empty();
                m_mgmt_queued.clear();
        }

        if (had_any) {
                m_mgmt_kick.store(true); // 立刻把"我不等了"送出去
        }
}

bool MainFrame::mgmt_queue_device(_In_ const std::string &busid, _In_ bool join)
{
        if (busid.empty()) {
                return false;
        }

        {
                std::lock_guard<std::mutex> lock(m_mgmt_mtx);
                if (!m_mgmt_cfg.enabled || m_mgmt_cfg.host.empty()) {
                        return false;
                }
                if (join) {
                        m_mgmt_queued.insert(busid);
                } else {
                        m_mgmt_queued.erase(busid);
                }
        }

        // 立刻把意愿送出去,不等下一个 10s 轮询窗口,这样"点击排队"在界面
        // 上几乎是即时的(服务端下一次心跳就会把本机挂进 FIFO)。
        m_mgmt_kick.store(true);
        return true;
}

void MainFrame::mgmt_apply_event(wxThreadEvent &event)
{
        auto result = event.GetPayload<std::shared_ptr<mgmt_result>>();
        if (!result) {
                return;
        }

        auto &tree = *m_treeListCtrl;
        if (!tree.GetColumnCount()) {
                return;
        }

        mgmt::settings cfg;
        {
                std::lock_guard<std::mutex> lock(m_mgmt_mtx);
                cfg = m_mgmt_cfg;
        }
        if (!cfg.enabled || cfg.host.empty()) {
                return;
        }

        /*
         * 被动发现:服务端要求访问密码(通常是授权到期,且心跳线程用保存的
         * 密码静默续期也没成功)。交给同一套弹框流程处理,而不是当成普通的
         * "管理接口不可用"打一条日志 —— 后者用户根本不知道该做什么。
         *
         * 注意要在 !result->ok 分支之前处理:access_required 时 result->err
         * 是空的,落到下面只会被忽略。
         */
        if (result->access_required) {
                mgmt_on_access_required();
                return;
        }

        if (!result->ok) {
                // 只在原因变化时提示一次,避免每 30 秒刷屏
                if (!result->err.empty() && result->err != m_mgmt_last_err) {
                        m_mgmt_last_err = result->err;
                        set_status_text(wxString::FromUTF8("服务器管理接口不可用:" + result->err));
                }
                return;
        }
        m_mgmt_last_err.clear();

        /*
         * 缓存本次元数据:attach 路径要用它判断"设备是否被别人占用"。
         * 只在成功时覆盖 —— 失败时保留上一份,避免网络闪断让占用方信息
         * 突然消失、于是把一台正被占用的设备当成空闲去直连。
         */
        {
                std::lock_guard<std::mutex> lock(m_mgmt_mtx);
                m_mgmt_meta = result->meta;
        }

        /*
         * 排队通知先处理:它和元数据是两个独立的载荷,即使本次没有任何设备
         * 需要刷新,轮到我们使用设备这件事也必须执行 —— 否则用户会一直
         * 等一个已经空出来的设备。
         */
        if (!result->notices.empty()) {
                mgmt_handle_queue_notices(result->notices);
        }

        if (result->meta.empty()) {
                return;
        }

        /*
         * 本地驱动层 attach 状态快照:用于 COL_STATE union 渲染。
         *
         * on_device_state()/mgmt_sync_attached() 写入 m_mgmt_attached,这里在
         * cfg 锁的同一把锁里拷一份出来,后续循环锁外读 m —— result->meta 在本
         * 函数内是只读副本,不需要再加锁。
         *
         * 这里不再自己拉 vhci::get_imported_devices:补正已经由心跳线程在每一拍
         * 开始处做掉(mgmt_sync_attached),本函数只消费它的结果。见该函数的注释
         * —— 早先"m_mgmt_attached 由 DeviceStateEvent 完整维护"的假设是错的,
         * 一次失败的 attach 尝试会以 disconnected 收尾并把状态刷掉。
         */
        std::map<std::string, bool> attached_snapshot;
        {
                std::lock_guard<std::mutex> lock(m_mgmt_mtx);
                attached_snapshot = m_mgmt_attached;
        }

        for (auto server = tree.GetFirstItem(); server.IsOk(); server = tree.GetNextSibling(server)) {
                auto url = tree.GetItemText(server);
                if (!hostname_matches(url.utf8_string(), cfg.host)) {
                        continue;
                }
                for (auto dev = tree.GetFirstChild(server); dev.IsOk(); dev = tree.GetNextSibling(dev)) {
                        auto busid = tree.GetItemText(dev, 0).utf8_string();
                        auto it = result->meta.find(busid);
                        if (it == result->meta.end()) {
                                continue;
                        }
                        const auto &m = it->second;
                        if (m.has_alias) {
                                auto product = tree.GetItemText(dev, usbip::COL_PRODUCT);
                                if (product != m.alias) {
                                        tree.SetItemText(dev, usbip::COL_PRODUCT, m.alias);
                                }
                        }
                        auto remark = tree.GetItemText(dev, usbip::MGMT_COL_REMARK);
                        if (remark != m.remark) {
                                tree.SetItemText(dev, usbip::MGMT_COL_REMARK, m.remark);
                        }
                        auto owner = tree.GetItemText(dev, usbip::MGMT_COL_OWNER);
                        if (owner != m.owner) {
                                tree.SetItemText(dev, usbip::MGMT_COL_OWNER, m.owner);
                        }
                        auto queue = tree.GetItemText(dev, usbip::MGMT_COL_QUEUE);
                        if (queue != m.queue) {
                                tree.SetItemText(dev, usbip::MGMT_COL_QUEUE, m.queue);
                        }

                        /*
                         * 状态列做 union:本机驱动层(快照)+ 服务端 meta 的远端语义。
                         * 仅在文案实际变化时写一次,避免无谓的 SetItemText 重绘。
                         * m 在 result->meta 期间引用稳定,锁外安全。
                         */
                        bool is_attached = false;
                        if (auto ait = attached_snapshot.find(busid); ait != attached_snapshot.end()) {
                                is_attached = ait->second;
                        }
                        auto new_state_text = usbip::to_view_state(
                                is_attached ? usbip::state::plugged : usbip::state::unplugged,
                                &m,
                                cfg.client_id,
                                m.attached_count);

                        if (auto cur_state_text = tree.GetItemText(dev, usbip::COL_STATE);
                            cur_state_text != new_state_text) {
                                tree.SetItemText(dev, usbip::COL_STATE, new_state_text);
                        }
                }
        }

        /*
         * 为「管理接口才知道、但 DEVLIST 拿不到」的设备补行。
         *
         * add_mgmt_only_devices() 的设计意图就是这份元数据的补行入口 —— USB/IP
         * 的 DEVLIST 在某些服务端上对「正被别人占用」的设备不可见,只有管理接口
         * 会列出来。客户端启动时第一次 add_exported_devices 发生在管理接口线程
         * 第一次 http_get 完成之前(默认 10 秒),那时候 m_mgmt_meta 仍然是空,
         * add_mgmt_only_devices() 会因为 meta.empty() 直接早 return —— 等管理
         * 接口数据到位后再单独调一次,才能把这些行补上。
         *
         * 不在 add_exported_devices 里做"启动 N 秒后重试一次"的妥协是因为这条
         * 入口更直接:meta 落地本身就是补行的最佳时机,且不需要再开定时器。
         */
        add_mgmt_only_devices(cfg.host, std::to_string(cfg.data_port));
}

/*
 * 服务端告诉我们"设备空出来了"。这里在 GUI 线程上走和用户点击「连接」
 * 完全相同的路径去 attach:同一个 attach(url, busid, ...) 调用、同一套
 * 参数(serial / 接收模式都从树上取),不额外引入一条"影子连接"逻辑。
 *
 * - attach:真正发起连接。若在等待期间设备又被别人抢走,attach 会失败,
 *   此时重新声明排队意愿(m_mgmt_queued 保留),下一轮继续等。
 * - dropped:设备被管理员取消共享,排队已经没有意义,只提示一次并撤出队列。
 */
void MainFrame::mgmt_handle_queue_notices(const std::vector<mgmt::queue_notice> &notices)
{
        auto &tree = *m_treeListCtrl;
        if (!tree.GetColumnCount()) {
                return;
        }

        for (const auto &notice : notices) {
                if (notice.action == "dropped") {
                        {
                                std::lock_guard<std::mutex> lock(m_mgmt_mtx);
                                m_mgmt_queued.erase(notice.busid);
                        }
                        set_status_text(wxString::FromUTF8(
                                "服务器已停止共享 " + notice.busid + "，已退出该设备的排队"));
                        continue;
                }

                if (notice.action != "attach") {
                        continue;
                }

                for (auto server = tree.GetFirstItem(); server.IsOk(); server = tree.GetNextSibling(server)) {
                        for (auto dev = tree.GetFirstChild(server); dev.IsOk(); dev = tree.GetNextSibling(dev)) {
                                if (tree.GetItemText(dev, 0).utf8_string() != notice.busid) {
                                        continue;
                                }

                                auto url = tree.GetItemText(server);
                                auto serial = tree.GetItemText(dev, usbip::COL_SERIAL);
                                auto recv_mode = tree.GetItemText(dev, usbip::COL_RECEIVE_MODE);

                                wxLogVerbose(L"%s: 轮到本机使用 %s，开始连接", wxString::FromAscii(__func__), url);

                                if (auto err = attach(url, tree.GetItemText(dev), serial, recv_mode, false)) {
                                        if (err != ERROR_OPERATION_ABORTED && err != ERROR_CANCELLED) {
                                                // 又被别人抢先了 —— 排队意愿保留,下一轮继续;
                                                // 对方释放时服务端会再次唤醒本机。
                                                wxLogVerbose(L"排队后连接 %s 失败(错误 %lu),继续等待", url, err);
                                        }
                                } else {
                                        mgmt_queue_device(notice.busid, false);
                                }
                                break;
                        }
                }
        }
}

void MainFrame::mgmt_loop()
{
        std::string cached_public_ip;
        /*
         * 公网 IP 重试策略:首次成功连接管理接口后立刻查一次;失败后每
         * 10 分钟再试一次,直到拿到 IP(网络临时不通/被代理劫持时会自愈)。
         * 拿到 IP 后停止重试,避免每次心跳都消耗带宽。
         */
        auto last_public_ip_attempt = std::chrono::steady_clock::time_point{};
        constexpr auto kPublicIpRetryInterval = std::chrono::seconds(600);


        auto wait_or_stop = [this](int ms) {
                for (int waited = 0; waited < ms && !m_mgmt_stop; waited += 200) {
                        if (m_mgmt_kick.exchange(false)) {
                                return false; // 配置变化:提前唤醒立即刷新,不等满间隔
                        }
                        std::this_thread::sleep_for(std::chrono::milliseconds(200));
                }
                return m_mgmt_stop.load();
        };

        for (;;) {
                mgmt::settings cfg;
                {
                        std::lock_guard<std::mutex> lock(m_mgmt_mtx);
                        if (m_mgmt_stop) {
                                return;
                        }
                        cfg = m_mgmt_cfg;
                }

                if (!cfg.enabled || cfg.host.empty() || cfg.web_port < 1 || cfg.web_port > 65535) {
                        if (wait_or_stop(2000)) {
                                return;
                        }
                        continue;
                }

                /*
                 * 每拍用驱动层列表补正一次本机状态,必须在取 attached 快照**之前**
                 * —— 否则这一拍的心跳仍然带着被"失败的 attach 尝试"刷掉的状态
                 * (见 mgmt_sync_attached() 的注释)。只补上不清除:真正断开由
                 * DeviceStateEvent 负责置 false。
                 */
                mgmt_sync_attached(cfg.host, {});

                std::map<std::string, bool> attached;
                std::set<std::string> queued;
                {
                        std::lock_guard<std::mutex> lock(m_mgmt_mtx);
                        attached = m_mgmt_attached;
                        queued = m_mgmt_queued;
                }

                // Phase B 兼容:管理接口可能在新服务端(单端口 5555)或老服务端
                // (独立管理端口)上。先试上次成功的端口;不通再回退到 usbip 数据端口。
                std::string last_err;

                /*
                 * 被动发现「共享访问密码」:
                 *   服务端启用了访问控制而本机未授权(或授权已过期,契约默认 12 小时)
                 *   时,/api/devices 与 /api/clients/heartbeat 都会回
                 *   401 {"ok":false,"error":"需要访问密码","accessRequired":true}。
                 *
                 * 这里先用本机保存的密码静默续期一次,成功就重试同一个请求 ——
                 * 用户全程无感;只有静默续期也失败(没保存过密码/管理员改过密码)
                 * 才把 access_required 带回 GUI 线程去弹框。
                 */
                bool access_required = false;

                auto silent_reauth = [&cfg](const mgmt::settings &c)
                {
                        if (c.access_password.empty()) {
                                return false; // 没保存过密码,只能弹框
                        }

                        std::string err;
                        int expires = 0;
                        if (!authorize_access(c, c.access_password, err, expires)) {
                                wxLogVerbose(L"访问密码静默续期失败: %s", wxString::FromUTF8(err));
                                return false;
                        }

                        wxLogVerbose(L"访问密码静默续期成功(有效期 %d 秒)", expires);
                        return true;
                };

                auto reachable = [&](int port)
                {
                        auto c2 = cfg;
                        c2.web_port = port;

                        std::string err;
                        int status = 0;
                        auto body = http_get_raw(c2, "/api/devices", status, &err);

                        if (status == 401) {
                                access_required = true;
                                if (silent_reauth(c2)) {
                                        access_required = false;
                                        err.clear();
                                        status = 0;
                                        body = http_get_raw(c2, "/api/devices", status, &err);
                                        access_required = status == 401;
                                }
                        }

                        if (status != 200) {
                                last_err = "端口 " + std::to_string(port) + " "
                                         + (err.empty() ? "服务器返回 HTTP " + std::to_string(status) : err);
                                return std::string();
                        }
                        if (body.empty()) {
                                last_err = "端口 " + std::to_string(port) + " " + err;
                        }
                        return body;
                };

                std::string body;
                int use_port = m_mgmt_ok_port.load();
                if (use_port < 1 || use_port > 65535) {
                        use_port = cfg.web_port;
                }
                body = reachable(use_port);
                if (body.empty() && cfg.data_port != use_port) {
                        auto alt = reachable(cfg.data_port);
                        if (!alt.empty()) {
                                body = std::move(alt);
                                use_port = cfg.data_port;
                        }
                }

                auto result = std::make_shared<mgmt_result>();
                /*
                 * 只有"这一轮最终没拿到设备列表"时才算需要访问密码。
                 *
                 * access_required 是跨端口尝试累积的:先试 web_port 拿到 401、
                 * 再回退到 data_port 成功时它仍然是 true。若不与 body.empty() 相与,
                 * GUI 会在 mgmt_apply_event() 里走 401 分支提前 return,把这一轮
                 * 成功取回的设备元数据和"轮到你了"排队唤醒通知一起丢掉。
                 */
                result->access_required = access_required && body.empty();
                if (body.empty()) {
                        result->ok = false;
                        /*
                         * 需要访问密码时不写通用错误文案:那不是"管理接口不可用",
                         * 用户该看到的是密码弹框/明确的提示,而不是一句会误导排查
                         * 方向的"服务器管理接口不可用:端口 5555 服务器返回 HTTP 401"。
                         */
                        if (!access_required) {
                                result->err = last_err;
                        }
                } else {
                        json_value root;
                        if (json_parse(body, root)) {
                                result->ok = true;
                                result->meta = parse_devices(root, cfg.client_id, cfg.client_name);
                        } else {
                                result->err = "响应不是有效 JSON(" + std::to_string(body.size()) + " 字节)";
                        }
                }

                const bool ok = result->ok;
                if (ok) {
                        // 记住成功端口,下次直接用它,避免每次都等老端口超时
                        m_mgmt_ok_port.store(use_port);

                        // Register ourselves so the server UI shows the connection owner.
                        // The server can already see the HTTP peer address, but that may be
                        // a reverse proxy/VPN address. Query the public IP only once after
                        // the first successful management connection, then reuse the result
                        // for all later heartbeats in this client process.
                        // 没拿到公网 IP 时,每 kPublicIpRetryInterval 重试一次;
                        // 拿到后停手,避免每次心跳都去查 IP 服务。
                        const bool never_attempted = last_public_ip_attempt.time_since_epoch().count() == 0;
                        const bool retry_due = std::chrono::steady_clock::now() - last_public_ip_attempt >= kPublicIpRetryInterval;
                        if (cached_public_ip.empty() && (never_attempted || retry_due)) {
                                last_public_ip_attempt = std::chrono::steady_clock::now();
                                if (auto detected = fetch_public_ip(); !detected.empty()) {
                                        cached_public_ip = std::move(detected);
                                        wxLogVerbose(L"检测到本机公网 IP: %s", wxString::FromUTF8(cached_public_ip));
                                } else {
                                        wxLogVerbose(L"公网 IP 查询失败,%d 分钟后重试",
                                                static_cast<int>(kPublicIpRetryInterval.count() / 60));
                                }
                        }
                        cfg.public_ip = cached_public_ip;

                        auto c2 = cfg;
                        c2.web_port = use_port; // 心跳与 GET 走同一端口
                        bool heartbeat_ok = false;
                        int hb_status = 0;
                        auto hb_body = http_post_raw(
                                c2, "/api/clients/heartbeat",
                                make_heartbeat_body(cfg, attached, queued), hb_status, heartbeat_ok);

                        /*
                         * 心跳同样会被访问控制拦(401 + accessRequired)。这里走
                         * 与 /api/devices 完全相同的静默续期 + 重试策略 —— 授权
                         * 过期最常见的第一个症状就是心跳开始 401。
                         */
                        if (hb_status == 401) {
                                access_required = true;
                                if (silent_reauth(c2)) {
                                        access_required = false;
                                        hb_status = 0;
                                        hb_body = http_post_raw(
                                                c2, "/api/clients/heartbeat",
                                                make_heartbeat_body(cfg, attached, queued), hb_status, heartbeat_ok);
                                        access_required = hb_status == 401;
                                }
                        }

                        heartbeat_ok = heartbeat_ok && hb_status == 200;

                        /*
                         * 心跳响应是排队机制的交付通道:服务端在设备空出来时
                         * 把 attach 通知写进这个 clientId 的 mailbox,并在本次
                         * 响应里一次性下发。解析后交给 GUI 线程执行 —— 真正
                         * 的 attach 只能在 GUI 线程做(和用户点击同一条路径)。
                         */
                        if (heartbeat_ok && !hb_body.empty()) {
                                if (json_value root; json_parse(hb_body, root)) {
                                        result->notices = parse_queue_notices(root);
                                }
                        }
                }

                // 成功与失败都要送回 GUI 线程:失败时由界面提示原因,否则用户只看到
                // "设备名/备注/占用方不显示",无从判断是网络、端口还是编码问题。
                // 需要访问密码时同样要送回去 —— 那正是触发弹框的唯一通道。
                //
                // 注意:wxWidgets 的 QueueEvent 在任何情况下都接管 event 的所有权
                // (排队失败时会自行 delete,见 src/common/event.cpp),因此这里
                // 绝不能因为返回值而再 delete 一次,否则双重释放。
                if (ok || !result->err.empty() || result->access_required) {
                        auto evt = new wxThreadEvent(wxEVT_THREAD, wxID_ANY);
                        evt->SetPayload(result); // The queued event owns a shared_ptr, even if discarded.
                        wxQueueEvent(this, evt);
                }

                // The event retains its shared payload until delivered or discarded.
                if (wait_or_stop(ok ? 10000 : 30000)) {
                        return;
                }
        }
}
