/*
 * Copyright (c) 2023-2026 Vadym Hrynchyshyn <vadimgrn@gmail.com>
 */

#include "utils.h"
#include "resource.h"
#include "mgmt.h"

#include <libusbip/remote.h>
#include <libusbip/win_socket.h>
#include <libusbip/format_message.h>
#include <libusbip/src/usb_ids.h>
#include <libusbip/src/file_ver.h>

#include <wx/buffer.h>
#include <wx/log.h>
#include <wx/msw/registry.h>
#include <wx/translation.h>

#include <wincrypt.h> // DPAPI: CryptProtectData / CryptUnprotectData (crypt32.lib)

#include <cstring>

namespace
{

static_assert(UsbLowSpeed == 0);
static_assert(UsbFullSpeed == 1);
static_assert(UsbHighSpeed == 2);
static_assert(UsbSuperSpeed == 3);

const wchar_t *g_usb_speed_str[] { L"Low", L"Full", L"High", L"Super" }; // indexed by enum USB_DEVICE_SPEED

auto &msgtable_dll = L"resources"; // legacy resource-only DLL name

// The message table is embedded in the executable for the single-file build.
// Keep the external DLL as a compatibility fallback for older installations.
auto get_resource_module() noexcept -> HMODULE
{
        static usbip::HModule external(LoadLibraryEx(msgtable_dll, nullptr, LOAD_LIBRARY_SEARCH_APPLICATION_DIR));
        return external ? external.get() : GetModuleHandle(nullptr);
}

auto get_ids_data()
{
        win::Resource r(GetModuleHandle(nullptr), MAKEINTRESOURCE(IDR_USB_IDS), RT_RCDATA);
        wxASSERT(r);
        return r.str();
}

/*
 * DPAPI 的"附加熵"(secondary entropy)。密文里绑定了这串字节,别的程序即使
 * 拿到注册表里的密文,没有同样的熵也解不出明文 —— 防止同机其他程序拿通用
 * DPAPI 调用直接把本程序保存的密码解出来。
 *
 * 改动这个字符串会让所有已保存的访问密码失效(下次连接重新弹框输入一次即可),
 * 因此除非确有安全需要不要动它。
 */
constexpr auto access_pwd_entropy = "USB-SHARE/wusbip:access-password:v1";

/*
 * 注册表值名:值名里不能含反斜杠,而服务器地址可能是主机名或 IPv6 字面量。
 * 这里做一次字符白名单归一(其余字符一律替换成 '_',字母转小写),保证同一台
 * 服务器永远映射到同一个值名,也不会因为非法字符导致写盘静默失败。
 */
wxString access_pwd_value_name(_In_ const wxString &host)
{
        wxString name = L"AccessPwd_";

        for (auto c: host) {
                if (wxIsalnum(c) || c == L'.' || c == L':' || c == L'-' || c == L'_' ||
                    c == L'[' || c == L']') {
                        name += wxTolower(c);
                } else {
                        name += L'_';
                }
        }

        return name;
}

DATA_BLOB make_blob(_In_reads_bytes_(len) const void *data, _In_ size_t len)
{
        return DATA_BLOB {
                static_cast<DWORD>(len),
                reinterpret_cast<BYTE*>(const_cast<void*>(data))
        };
}

bool dpapi_encrypt(_In_ const std::string &plain, _Out_ wxMemoryBuffer &cipher)
{
        auto in = make_blob(plain.data(), plain.size());
        auto entropy = make_blob(access_pwd_entropy, std::strlen(access_pwd_entropy));

        DATA_BLOB out{};
        if (!CryptProtectData(&in, L"USB-SHARE 访问密码", &entropy, nullptr, nullptr,
                              CRYPTPROTECT_UI_FORBIDDEN, &out)) {
                return false;
        }

        cipher.AppendData(out.pbData, out.cbData);
        LocalFree(out.pbData);
        return true;
}

bool dpapi_decrypt(_In_reads_bytes_(len) const void *cipher, _In_ size_t len, _Out_ std::string &plain)
{
        auto in = make_blob(cipher, len);
        auto entropy = make_blob(access_pwd_entropy, std::strlen(access_pwd_entropy));

        DATA_BLOB out{};
        if (!CryptUnprotectData(&in, nullptr, &entropy, nullptr, nullptr,
                                CRYPTPROTECT_UI_FORBIDDEN, &out)) {
                return false;
        }

        plain.assign(reinterpret_cast<const char*>(out.pbData), out.cbData);
        LocalFree(out.pbData);
        return true;
}

/* 以读/写方式打开配置根键。@param create true 时键不存在就创建 */
bool open_config_key(_Out_ wxRegKey &key, _In_ bool create)
{
        key.SetName(wxRegKey::HKCU, win::config_registry_root());
        return create ? (key.Exists() ? key.Open(wxRegKey::Write) : key.Create())
                      : (key.Exists() && key.Open(wxRegKey::Read));
}

} // namespace


auto win::get_file_version() -> const FileVersion&
{
        static FileVersion v;
        wxASSERT(v);
        return v;
}

/*
 * 与 wxConfig 同一个根:HKCU\Software\<CompanyName>\<ProductName>。
 * 与 App::set_names() 同源(版本资源),因此永远不会和 wxConfig 分裂。
 * 详见 utils.h 的注释。
 */
wxString win::config_registry_root()
{
        const auto &v = get_file_version();

        wxString root = L"Software\\";
        root += wxString(v.GetCompanyName());
        root += L"\\";
        root += wxString(v.GetProductName());
        return root;
}

/*
 * 「共享访问密码」的本地持久化。存放位置、安全模型与失败语义见 utils.h 的注释。
 *
 * 这里刻意复用与 wxConfig 相同的根键(而不是另开一套配置体系),原因有二:
 *   1. 卸载/清理配置时用户只需要处理一个位置,不会留下孤儿凭据;
 *   2. 与 win::config_registry_root() 同源,以后改品牌不会再出现"读写的键
 *      不是一个地方"这类历史坑(见 utils.h 顶部关于旧品牌硬编码的说明)。
 */
bool win::save_access_password(_In_ const wxString &host, _In_ const wxString &password)
{
        if (host.empty() || password.empty()) {
                return false;
        }

        wxLogNull no_log; // 注册表读写失败绝不能弹日志对话框打断连接流程

        wxRegKey key;
        if (!open_config_key(key, true)) {
                return false;
        }

        wxMemoryBuffer cipher;
        if (!dpapi_encrypt(password.utf8_string(), cipher)) {
                // DPAPI 失败(极少见:用户配置文件损坏等)。此时宁可不保存,
                // 也绝不退化成明文落盘。
                wxLogVerbose(L"访问密码加密失败(DPAPI),本次不保存");
                return false;
        }

        return key.SetValue(access_pwd_value_name(host), cipher);
}

bool win::load_access_password(_In_ const wxString &host, _Out_ wxString &password)
{
        password.clear();
        if (host.empty()) {
                return false;
        }

        wxLogNull no_log;

        wxRegKey key;
        if (!open_config_key(key, false)) {
                return false;
        }

        const auto name = access_pwd_value_name(host);
        if (!key.HasValue(name) || key.GetValueType(name) != wxRegKey::Type_Binary) {
                return false;
        }

        wxMemoryBuffer cipher;
        if (!key.QueryValue(name, cipher) || cipher.GetDataLen() == 0) {
                return false;
        }

        std::string plain;
        if (!dpapi_decrypt(cipher.GetData(), cipher.GetDataLen(), plain)) {
                // 密文解不开:换了 Windows 账户/机器,或熵常量被改过。
                // 按"没有保存过密码"处理即可 —— 用户重新输入一次就恢复了。
                wxLogVerbose(L"已保存的访问密码无法解密(DPAPI),将重新询问");
                return false;
        }

        password = wxString::FromUTF8(plain);
        return !password.empty();
}

bool win::erase_access_password(_In_ const wxString &host)
{
        if (host.empty()) {
                return false;
        }

        wxLogNull no_log;

        wxRegKey key;
        if (!open_config_key(key, false)) {
                return false;
        }

        const auto name = access_pwd_value_name(host);
        return key.HasValue(name) && key.DeleteValue(name);
}

/*
 * Use for libusbip.dll errors only. 
 * @see wxSysErrorMsg
 */
wxString usbip::GetLastErrorMsg(_In_ DWORD msg_id)
{
        return wformat_message(get_resource_module(), msg_id);
}

auto usbip::get_vhci() -> Handle&
{
        static auto handle = vhci::open();
        return handle;
}

auto usbip::get_ids() -> const UsbIds&
{
        static UsbIds ids(get_ids_data());
        wxASSERT(ids);
        return ids;
}

auto usbip::get_event() -> NullableHandle& 
{
        static NullableHandle evt(CreateEvent(nullptr, true, false, nullptr));
        return evt;
}

bool usbip::init(_Inout_ wxString &err)
{
        wxASSERT(err.empty());

        if (!get_resource_module()) {
                auto ec = GetLastError();
                err = wxString::Format(L"无法加载 '%s.dll'\n错误 %lu\n%s", msgtable_dll, err, wformat_message(ec));
                return false;
        }

        static InitWinSock2 ws2;
        if (!ws2) {
                auto ec = GetLastError();
                err = wxString::Format(L"网络初始化(WSAStartup)失败 %lu\n%s", ec, wxSysErrorMsg(ec));
                return false;
        }

        if (!get_event()) {
                auto ec = GetLastError();
                err = wxString::Format(L"无法创建事件\n错误 %lu\n%s", ec, wxSysErrorMsg(ec));
                return false;
        }

        return static_cast<bool>(get_vhci());
}

const wchar_t* usbip::get_speed_str(_In_ USB_DEVICE_SPEED speed) noexcept
{
        auto idx = static_cast<int>(speed);
        return idx >= 0 && idx < std::ssize(g_usb_speed_str) ? g_usb_speed_str[idx] : L"?";
}

bool usbip::get_speed_val(_Out_ USB_DEVICE_SPEED &val, _In_ const wxString &speed) noexcept
{
        for (int i = 0; i < std::ssize(g_usb_speed_str); ++i) {
                if (speed.IsSameAs(g_usb_speed_str[i], false)) {
                        val = static_cast<USB_DEVICE_SPEED>(i);
                        return true;
                }
        }

        return false;
}

auto usbip::make_imported_device(
        _In_ std::string hostname, _In_ std::string service, _In_ const usb_device &dev) -> imported_device
{
        return imported_device {
                .location = {
                        .hostname = std::move(hostname), 
                        .service = std::move(service), 
                        .busid = dev.busid 
                },

                .devid = make_devid(dev.busnum, dev.devnum),
                .speed = dev.speed,

                .vendor = dev.idVendor,
                .product = dev.idProduct,
                .serial = {},
        };
}

wxString usbip::make_device_url(_In_ const device_location &loc)
{
        auto url = loc.hostname + ':' + loc.service + '/' + loc.busid;
        return wxString::FromUTF8(url);
}

wxString usbip::make_server_url(_In_ const device_location &loc)
{
        auto url = loc.hostname + ':' + loc.service;
        return wxString::FromUTF8(url);
}

wxString usbip::make_server_url(_In_ const wxString &hostname, _In_ const wxString &service)
{
        return hostname + L':' + service;
}

bool usbip::split_server_url(_In_ const wxString &url, _Inout_ wxString &hostname, _Inout_ wxString &service)
{
        auto pos = url.find_last_of(L':');
        auto found = pos != url.npos;
        
        if (found) {
                hostname = url.substr(0, pos);
                service = url.substr(++pos);
        }

        return found;
}

/*
 * 25FC black medium square
 * 26AA medium white circle, 26AB medium black circle
 */
wxString usbip::to_string(_In_ state state)
{
        wxString s;

        switch (state) {
        case state::unplugged:
        case state::disconnected:
                s = L"未连接";
                break;
        case state::plugged:
        case state::connected:
                s = L"已连接";
                break;
        case state::connecting:
                s = L"连接中";
                break;
        case state::unplugging:
                s = L"断开中";
                break;
        default:
                s = wxString::FromAscii(vhci::get_state_str(state));
        }

        return s;
}

/*
 * 把"本机驱动层 state + 服务端 meta_device + connections 数组长度"合并成
 * 一个显示字符串,详见 utils.h 的注释。
 *
 * 注意:不要因为 meta 里的 busy/self_queued 就覆盖本机的 plugged 状态——
 * 即便服务端暂时把 holder 写成别人(本机刚连上、心跳还没到服务端),本机视角
 * 仍然是"已连接"。自身一致性比远端一致性更重要。
 */
wxString usbip::to_view_state(
        _In_ state local,
        _In_ const mgmt::meta_device *meta,
        _In_ const std::string &self_client_id,
        _In_ size_t attached_count)
{
        using enum state;

        switch (local) {
        case plugged:
        case connected:
                // count==1:只有本机 -> "已连接";count>=2:多人共享 -> 加后缀
                return attached_count >= 2
                        ? wxString::Format(L"已连接 (%zu)", attached_count)
                        : wxString(L"已连接");

        case unplugging:
                return L"断开中";

        case connecting:
                return L"挂载中";

        case unplugged:
        case disconnected:
                break; // 见下方 meta 二次细分
        }

        if (!meta) {
                return L"未连接"; // 服务端管理接口未启用/还没拉到这台设备
        }

        if (meta->busy && (self_client_id.empty() || meta->holder_id != self_client_id)) {
                // "他人占用 (N)":N 表示所有连接方,1 也加后缀让"只剩自己 vs
                // 别人在用"在视觉上明显区分(本机 attached 时也是 "(1)",
                // 但本机 attached 时走的是上面 plugged 分支)。
                return attached_count >= 1
                        ? wxString::Format(L"他人占用 (%zu)", attached_count)
                        : wxString(L"他人占用");
        }
        if (meta->self_queued) {
                return L"排队中";
        }

        // meta 存在但既不忙也没人排(可能共享刚被撤销、或本机就是 holder 但驱动
        // 还没回事件)。这两种情况下"空闲"比"未连接"更贴近事实。
        return L"空闲";
}
