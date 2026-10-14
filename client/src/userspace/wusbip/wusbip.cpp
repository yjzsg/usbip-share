/*
 * Copyright (c) 2023-2026 Vadym Hrynchyshyn <vadimgrn@gmail.com>
 */

#include "wusbip.h"
#include "taskbaricon.h"
#include "resource.h"
#include "persist.h"
#include "wxutils.h"
#include "utils.h"
#include "font.h"
#include "log.h"
#include "app.h"
#include "mgmt.h"

#include <libusbip/remote.h>
#include <libusbip/persistent.h>
#include <libusbip/src/file_ver.h>

#include <wx/event.h>
#include <wx/msgdlg.h>
#include <wx/aboutdlg.h>
#include <wx/busyinfo.h>
#include <wx/dataview.h>
#include <wx/config.h>
#include <wx/textdlg.h>
#include <wx/headerctrl.h>
#include <wx/clipbrd.h>
#include <wx/persist/dataview.h>
#include <wx/dc.h>
#include <wx/itemattr.h>
#include <wx/aui/dockart.h>

#include <algorithm>
#include <cwchar>
#include <format>
#include <set>
#include <string>

namespace
{

using namespace usbip;

/*
 * 设备唯一键 "host:service/busid"(全小写)。
 *
 * 用来判断"驱动里挂着的设备"与"持久化清单里的设备"是不是同一台。
 * host/service 是用户手填的,大小写未必一致,所以统一转小写再比。
 */
std::string device_key(
        _In_ const std::string &host, _In_ const std::string &service, _In_ const std::string &busid)
{
        std::string k = host + ':' + service + '/' + busid;

        for (auto &c: k) {
                if (c >= 'A' && c <= 'Z') c = char(c - 'A' + 'a');
        }

        return k;
}

class FlatToolBarArt final : public wxAuiGenericToolBarArt
{
public:
        wxAuiToolBarArt* Clone() override { return new FlatToolBarArt(*this); }
        void DrawBackground(wxDC& dc, wxWindow* wnd, const wxRect& rect) override
        {
                dc.SetPen(*wxTRANSPARENT_PEN);
                dc.SetBrush(wxBrush(wnd->GetBackgroundColour()));
                dc.DrawRectangle(rect);
        }
        void DrawPlainBackground(wxDC& dc, wxWindow* wnd, const wxRect& rect) override
        {
                DrawBackground(dc, wnd, rect);
        }
};

struct ui_palette
{
        wxColour window;
        wxColour surface;
        wxColour surface_alt;
        wxColour table;
        wxColour alternate_row;
        wxColour text;
        wxColour muted_text;
        wxColour header_text;
        wxColour accent;
        wxColour accent_text;
        wxColour border;
};

/*
 * 统一的中性蓝灰配色。
 *
 * 旧界面把表头设成 MEDIUM FOREST GREEN,深色模式的工具栏图标又是亮黄色,
 * 再叠加 Windows 原生深色控件,最终看起来像几套主题拼在一起。这里不改变
 * "跟随系统/浅色/深色"三种外观选择,只把两种模式的自定义颜色收敛到同一套
 * 蓝灰语义:蓝色表示可操作/焦点,红色仍只给"全部断开"这种破坏性动作。
 */
ui_palette make_ui_palette()
{
        if (wxSystemSettings::GetAppearance().IsDark()) {
                return {
                        wxColour(30, 36, 46),     // window
                        wxColour(39, 47, 59),     // surface
                        wxColour(45, 54, 68),     // surface_alt
                        wxColour(24, 29, 38),     // table
                        wxColour(31, 38, 49),     // alternate_row
                        wxColour(230, 237, 243),  // text
                        wxColour(165, 177, 192),  // muted_text
                        wxColour(190, 201, 214),  // header_text
                        wxColour(57, 76, 99),      // accent (soft action button)
                        wxColour(230, 237, 243),   // accent_text
                        wxColour(63, 75, 91),     // border
                };
        }

        return {
                wxColour(245, 247, 250),          // window
                wxColour(255, 255, 255),          // surface
                wxColour(237, 242, 247),          // surface_alt
                wxColour(255, 255, 255),          // table
                wxColour(247, 249, 252),          // alternate_row
                wxColour(31, 41, 55),             // text
                wxColour(100, 116, 139),          // muted_text
                wxColour(71, 85, 105),            // header_text
                wxColour(229, 238, 248),          // accent (soft action button)
                wxColour(30, 76, 112),             // accent_text
                wxColour(214, 222, 232),          // border
        };
}

/*
 * Windows 11 默认把新注册的传统 NotifyIcon 放进"隐藏图标"弹窗,所以用户
 * 会误以为右下角图标消失。wxTaskBarIcon 使用的是传统 UID 方式;Shell 会在
 * HKCU\\Control Panel\\NotifyIconSettings\\<id> 下为它创建注册项。把当前进程
 * 对应项标记为 IsPromoted=1 后,图标会回到通知区域主栏。
 *
 * 这是用户明确需要的客户端常驻图标,不是修改系统全局设置:只匹配当前 exe 的
 * 单个注册项,不触碰其他程序的通知图标。Windows 10 没有该值时会忽略它。
 *
 * 返回 true 表示这次真的把值从"非 1"改成了 1 —— 实测只写注册表并不会让已经在
 * 折叠区里的图标挪出来,必须再让 Shell 重新加载一次该图标(RemoveIcon+SetIcon)。
 * 调用方据此做一次性的"重加图标"。
 */
bool promote_taskbar_icon()
{
        wchar_t module_path[32768]{};
        auto length = ::GetModuleFileNameW(nullptr, module_path, ARRAYSIZE(module_path));
        if (!length || length >= ARRAYSIZE(module_path)) {
                return false;
        }

        HKEY root{};
        constexpr auto subkey = L"Control Panel\\NotifyIconSettings";
        if (::RegOpenKeyExW(HKEY_CURRENT_USER, subkey, 0, KEY_READ, &root) != ERROR_SUCCESS) {
                return false;
        }

        bool changed = false;
        for (DWORD index = 0;; ++index) {
                wchar_t key_name[256]{};
                DWORD key_name_len = ARRAYSIZE(key_name);
                auto result = ::RegEnumKeyExW(root, index, key_name, &key_name_len,
                        nullptr, nullptr, nullptr, nullptr);
                if (result == ERROR_NO_MORE_ITEMS) {
                        break;
                }
                if (result != ERROR_SUCCESS) {
                        continue;
                }

                HKEY item{};
                if (::RegOpenKeyExW(root, key_name, 0, KEY_QUERY_VALUE | KEY_SET_VALUE, &item) != ERROR_SUCCESS) {
                        continue;
                }

                wchar_t executable[32768]{};
                DWORD type = 0;
                DWORD bytes = sizeof(executable);
                auto query = ::RegQueryValueExW(item, L"ExecutablePath", nullptr, &type,
                        reinterpret_cast<BYTE*>(executable), &bytes);
                if (query == ERROR_SUCCESS && type == REG_SZ &&
                    _wcsicmp(executable, module_path) == 0) {
                        /*
                         * IsPromoted 的三个可能状态及处理:
                         *   - 不存在 (首次启动)            -> 写 1,让图标进入主通知栏;
                         *   - 存在且 == 1 (用户没动过)    -> 已 OK,不动;
                         *   - 存在且 == 0 (用户拖到隐藏区) -> 尊重用户选择,绝不再写 1。
                         * 关键点:Windows 11 上,用户把图标拖到 "^" 折叠区时,
                         * Shell 会把 IsPromoted 改回 0。如果这里继续把 0 改回 1,
                         * 会在 ≤ 5 秒内(看门狗周期)把图标弹回主栏 —— 这就是
                         * 用户看到的"图标一直在主栏,拖到隐藏区也会自动跳出来"。
                         */
                        DWORD current = 0;
                        DWORD current_type = 0;
                        DWORD current_bytes = sizeof(current);
                        auto read = ::RegQueryValueExW(item, L"IsPromoted", nullptr, &current_type,
                                reinterpret_cast<BYTE*>(&current), &current_bytes);
                        if (read != ERROR_SUCCESS) {
                                DWORD promoted = 1;
                                ::RegSetValueExW(item, L"IsPromoted", 0, REG_DWORD,
                                        reinterpret_cast<const BYTE*>(&promoted), sizeof(promoted));
                                changed = true;
                        }
                        ::RegCloseKey(item);
                        break;
                }
                ::RegCloseKey(item);
        }

        ::RegCloseKey(root);
        return changed;
}

const auto g_persistent_mark = L'\u2713'; // CHECK MARK, 2714 HEAVY CHECK MARK
auto &g_key_devices = L"/devices";
auto &g_key_url = L"url";

/*
 * Menu/View/Appearance, radio items
 */
inline auto get_appearance_itemid(_In_ int appearance) noexcept
{
        return wxID_FILE1 + appearance;
}

inline auto get_appearance_value(_In_ int itemid) noexcept
{
        wxASSERT(itemid >= wxID_FILE1);
        return itemid - wxID_FILE1;
}

/*
 * COL_PERSISTENT is not saved with these GUI flags.
 */
consteval auto get_saved_keys()
{
        using key_val = std::pair<const wchar_t* const, column_pos_t>;

        return std::to_array<key_val>({
                { L"busid", COL_BUSID },
                { L"devid", COL_DEVID },
                { L"speed", COL_SPEED },
                { L"vendor", COL_VENDOR },
                { L"product", COL_PRODUCT },
                { L"serial", COL_SERIAL },
                { L"receive_mode", COL_RECEIVE_MODE },
                { L"notes", COL_NOTES },
        });
}

consteval auto get_saved_flags()
{
        unsigned int flags{};

        for (auto [key, col]: get_saved_keys()) {
                flags |= mkflag(col);
        }

        return flags;
}

constexpr auto is_port_residual(_In_ state st)
{
        switch (st) {
        using enum state;
        case unplugged:  // port > 0 if previous state was unplugging
        case connecting: // port is zero
        case connected:  // ...
                return true;
        case disconnected: // port > 0 if previous state was plugged
        case plugged:      // port > 0
        case unplugging:   // ...
        default:
                return false;
        }
}

template<typename T>
requires std::three_way_comparable<T>
auto as_set(_In_ std::vector<T> v)
{
        return std::set<T>(std::make_move_iterator(v.begin()),
                           std::make_move_iterator(v.end()));
}

void log(_In_ const device_state &st)
{
        auto &d = st.device;
        auto &loc = d.location;

        auto s = std::format("{}:{}/{} {}, port {}, devid {:x}, speed {}, vid {:x}, pid {:x}, serial '{}', {}, source {:x}",
                              loc.hostname, loc.service, loc.busid, vhci::get_state_str(st.state), d.port, d.devid,
                              static_cast<int>(d.speed), d.vendor, d.product, d.serial,
                              to_string(d.recv_mode).utf8_string(), st.source_id);

        wxLogVerbose(wxString::FromUTF8(s));
}

void log(_In_ const wxTreeListCtrl &tree, _In_ wxTreeListItem dev, _In_ const wxString &prefix)
{
        auto server = tree.GetItemParent(dev);
        auto &url = tree.GetItemText(server);

        auto s = wxString::Format(
                        L"%s:%s/%s, port '%s', devid '%s', speed '%s', vendor '%s', product '%s', "
                        L"serial '%s', state '%s', auto '%s', mode '%s', notes '%s', source '%s'", 
                        prefix, url,
                        tree.GetItemText(dev, COL_BUSID),
                        tree.GetItemText(dev, COL_PORT),
                        tree.GetItemText(dev, COL_DEVID),
                        tree.GetItemText(dev, COL_SPEED),
                        tree.GetItemText(dev, COL_VENDOR), 
                        tree.GetItemText(dev, COL_PRODUCT), 
                        tree.GetItemText(dev, COL_SERIAL), 
                        tree.GetItemText(dev, COL_STATE), 
                        tree.GetItemText(dev, COL_PERSISTENT), 
                        tree.GetItemText(dev, COL_RECEIVE_MODE), 
                        tree.GetItemText(dev, COL_NOTES),
                        tree.GetItemText(dev, COL_SOURCE_ID));

        wxLogVerbose(s);
}

auto load_license()
{
        wxString s;

        auto hres = FindResource(nullptr, MAKEINTRESOURCE(IDR_LICENSE), RT_RCDATA);
        if (!hres) {
                auto err = GetLastError();
                return s = wxString::Format(L"FindResource 错误 %lu\n%s", err, wxSysErrorMsg(err));
        }

        auto hmem = LoadResource(nullptr, hres);
        if (!hmem) {
                auto err = GetLastError();
                return s = wxString::Format(L"LoadResource 错误 %lu\n%s", err, wxSysErrorMsg(err));
        }

        auto txt = LockResource(hmem);
        auto len = SizeofResource(nullptr, hres);

        return s = wxString::FromAscii(static_cast<const char*>(txt), len);
}

/*
 * COL_SERIAL, COL_RECEIVE_MODE are present in saved and persistent.
 */
enum columns : unsigned int {
        saved_read_only = 1,
        saved_notes = 2,
        saved_serial = 4,
        saved_recv_mode = 8,
        saved_read_write = saved_notes | saved_serial | saved_recv_mode,
        saved_all = saved_read_only | saved_read_write,

        pers_persistent = 16,
        pers_serial = 32,
        pers_recv_mode = 64,
        pers_all = pers_persistent | pers_serial | pers_recv_mode
};

constexpr auto get_saved_rw_pairs()
{
        return std::to_array<std::pair<columns, column_pos_t>>({
                { columns::saved_notes, COL_NOTES },
                { columns::saved_serial, COL_SERIAL },
                { columns::saved_recv_mode, COL_RECEIVE_MODE },
        });
}

constexpr auto get_saved_rw_flags()
{
        unsigned int flags{};

        for (auto [dummy, col]: get_saved_rw_pairs()) {
                flags |= mkflag(col);
        }

        return flags;
}

auto set_saved_read_only(_Inout_ device_columns &dc, _In_ const device_columns &saved)
{
        unsigned int flags{};
        constexpr auto rw_flags = get_saved_rw_flags();

        for (auto [dummy, col]: get_saved_keys()) {
                if (auto flag = mkflag(col); !(flag & rw_flags)) {
                        dc[col] = saved[col];
                        flags |= flag;
                }
        }

        return flags;
}

auto update_from_registry(
        _Inout_ device_columns &dc, _In_ unsigned int flags, _In_ unsigned int columns,
        _In_ const std::set<persistent_device> &persistent)
{
        if (persistent.empty()) {
                return flags;
        }

        wxASSERT(columns & columns::pers_all);
        static_assert(!(get_saved_flags() & mkflag(COL_PERSISTENT)));

        auto key = make_persistent_device(dc);

        if (auto pd = persistent.find(key); pd != persistent.end()) {

                if (columns & columns::pers_persistent) {
                        dc[COL_PERSISTENT] = g_persistent_mark;
                        wxASSERT(!(flags & mkflag(COL_PERSISTENT)));
                        flags |= mkflag(COL_PERSISTENT);
                }

                if (columns & columns::pers_serial) {
                        dc[COL_SERIAL] = wxString::FromUTF8(pd->serial);
                        flags |= mkflag(COL_SERIAL);
                }

                if (columns & columns::pers_recv_mode) {
                        dc[COL_RECEIVE_MODE] = to_string(pd->recv_mode);
                        wxASSERT(flags & mkflag(COL_RECEIVE_MODE));
                }
        }

        return flags;
}

auto update_from_registry(
        _Inout_ device_columns &dc, _In_ unsigned int flags, _In_ unsigned int columns,
        _In_ const std::set<device_columns> &saved, _In_ const std::set<persistent_device> &persistent)
{
        wxASSERT(columns & columns::saved_all);

        if (auto i = saved.find(dc); i != saved.end()) {

                if (columns & columns::saved_read_only) {
                        flags |= set_saved_read_only(dc, *i);
                }

                for (auto [col_bit, col] : get_saved_rw_pairs()) {
                        if (columns & col_bit) {
                                dc[col] = (*i)[col];
                                wxASSERT((col == COL_RECEIVE_MODE) == bool(flags & mkflag(col)));
                                flags |= mkflag(col);
                        }
                }
        }

        return update_from_registry(dc, flags, columns, persistent);
}

using predicate_f = std::function<bool(const wxTreeListCtrl&, wxTreeListItem)>;

auto is_server(_In_ const wxTreeListCtrl &tree, _In_ wxTreeListItem item) // predicate_f
{
        return tree.GetItemParent(item) == tree.GetRootItem();
}

/*
 * @see is_empty(_In_ const device_columns &dc)
 * @see is_empty(_In_ const imported_device &d)
 */
auto is_empty(_In_ const wxTreeListCtrl &tree, _In_ wxTreeListItem dev) // predicate_f
{
        auto &s = tree.GetItemText(dev, COL_DEVID); 
        return s.empty();
}

auto is_server_or_empty(_In_ const wxTreeListCtrl &tree, _In_ wxTreeListItem item) // predicate_f
{
        return is_server(tree, item) || is_empty(tree, item);
}

auto get_devices(
        _In_ const wxTreeListCtrl &tree, _In_ const predicate_f &except_for = is_server_or_empty)
{
        wxTreeListItems v;

        for (auto item = tree.GetFirstItem(); item.IsOk(); item = tree.GetNextItem(item)) {
                if (!except_for(tree, item)) {
                        v.push_back(item);
                }
        }

        return v;
}

auto get_selected_devices(
        _In_ const wxTreeListCtrl &tree, _In_ const predicate_f &except_for = is_server_or_empty)
{
        wxTreeListItems v;
        tree.GetSelections(v);

        auto pred = [except_for, &tree] (auto &item) { return except_for(tree, item); };
        std::erase_if(v, pred);

        return v;
}

auto make_persistent_device(
        _In_ const wxTreeListCtrl &tree, _In_ wxTreeListItem server, _In_ wxTreeListItem device)
{
        auto &url = tree.GetItemText(server);
        auto &busid = tree.GetItemText(device);

        return usbip::make_persistent_device(url, busid,
                        tree.GetItemText(device, COL_SERIAL),
                        tree.GetItemText(device, COL_RECEIVE_MODE));
}

auto get_persistent(_In_ const Handle &vhci = get_vhci())
{
        std::set<persistent_device> result;

        if (auto v = vhci::get_persistent(vhci.get())) {
                result = as_set(*v);
        } else {
                auto err = GetLastError();
                wxLogVerbose(L"无法读取自动连接(Auto)设备信息\n错误 %lu\n%s", err, GetLastErrorMsg(err));
        }

        return result;
}

auto get_saved()
{
        auto &cfg = *wxConfig::Get();

        auto path = cfg.GetPath();
        cfg.SetPath(g_key_devices);

        std::vector<device_columns> result;
        wxString name;
        long idx;

        for (auto ok = cfg.GetFirstGroup(name, idx); ok; cfg.SetPath(L".."), ok = cfg.GetNextGroup(name, idx)) {

                cfg.SetPath(name);

                device_columns dev;
                auto &url = get_url(dev);

                url = cfg.Read(g_key_url);
                if (url.empty()) {
                        continue;
                }

                for (auto [key, col] : get_saved_keys()) {
                        dev[col] = cfg.Read(key);
                }

                if (dev[COL_BUSID].empty()) {
                        continue;
                }

                validate_receive_mode(dev[COL_RECEIVE_MODE]);
                result.push_back(std::move(dev));
        }

        cfg.SetPath(path);
        return result;
}

auto to_string(_In_ const wxTreeListCtrl &tree, _In_ wxTreeListItem dev)
{
        wxASSERT(tree.GetColumnCount() > COL_LAST_VISIBLE);

        wxString s;
        for (unsigned int n = COL_LAST_VISIBLE, i = 0; i <= n; ++i) {
                if (i) {
                        s += L',';
                } else if (auto server = tree.GetItemParent(dev); server.IsOk()) {
                        s += tree.GetItemText(server);
                        s += L'/';
                } else {
                        wxFAIL_MSG("server.IsOk");
                }
                s += tree.GetItemText(dev, i);
        }
        return s;
}

auto get_servers(_In_ const std::vector<device_columns> &devices)
{
        std::set<wxString> servers;

        for (wxString hostname, service; auto &dc: devices) {
                if (auto url = get_url(dc); split_server_url(url, hostname, service)) {
                        servers.insert(std::move(hostname));
                }
        }

        return servers;
}

} // namespace


class DeviceStateEvent : public wxEvent
{
public:
        DeviceStateEvent(_In_ device_state st) : 
                wxEvent(0, EVT_DEVICE_STATE),
                m_state(std::move(st)) {}

        wxEvent *Clone() const override { return new DeviceStateEvent(*this); }

        auto& get() const noexcept { return m_state; }
        auto& get() noexcept { return m_state; }

private:
        device_state m_state;
};
wxDEFINE_EVENT(EVT_DEVICE_STATE, DeviceStateEvent);


MainFrame::MainFrame(_In_ Handle read, _In_ int appearance) : 
        Frame(nullptr),
        m_log(new LogWindow(this, 
                m_menu_log->FindItem(ID_TOGGLE_LOG_WINDOW),
                m_menu_view->FindItem(wxID_ZOOM_IN),
                m_menu_view->FindItem(wxID_ZOOM_OUT),
                m_menu_view->FindItem(wxID_ZOOM_100))),
        m_read(std::move(read))
{
        wxASSERT(m_read);
        check_view_appearance(appearance);
        
        init();
        restore_state();
        configure_workspace_ui();
        sync_auto_connect_menu();
        post_refresh();
        schedule_auto_connect();
        mgmt_start();

        // Restore historic minimized state before accepting user actions. Do not
        // schedule delayed Show(): it races with a user's close/minimize click.
        if (IsIconized()) {
                Iconize(false);
        }
}

MainFrame::~MainFrame()
{
        m_window_fix_timer.Stop();
        m_taskbar_watchdog_timer.Stop();

        /*
         * 防御性线程清理(兜底)。
         *
         * 正常路径下 on_close() 已经执行过 break_read_loop()/join()/mgmt_stop(),
         * 两个线程均不再 joinable,这里什么都不做。
         *
         * 但若 on_close() 中途抛异常(例如 m_read_thread.join() 抛 std::system_error),
         * 或者窗口被非正常销毁而根本没有收到 wxEVT_CLOSE_WINDOW,线程就会残留:
         *   - std::thread 析构遇到 joinable 线程 -> std::terminate()
         *   - 管理接口线程继续运行 -> 向已销毁的窗口 wxQueueEvent() -> 访问违例 0xc0000005
         * 这两条都曾表现为"打开几秒即闪退"。此处兜底,使任何退出路径都安全。
         */
        m_mgmt_stop = true; // 先置停止标志,让管理线程尽快从循环退出

        /*
         * 兜底告别:如果 on_close() 因为异常被绕过(任务管理器 kill、
         * join 抛 system_error 直接进析构等),管理接口线程可能还在
         * 跑,m_mgmt_attached 也还有效 —— 这时再发一次告别,
         * 让服务端 0 延迟释放设备。正常 on_close() 已经发过,
         * 服务端会幂等处理(kick_device 在设备不存在时返回 False,
         * handle_client_goodbye 在 clientId 缺失时直接返回)。
         */
        try {
                mgmt_send_goodbye();
        } catch (...) {
                // 析构期不能再抛
        }

        try {
                if (m_read_thread.joinable()) {
                        try {
                                break_read_loop();
                        } catch (...) {
                        }
                        m_read_thread.join();
                }
        } catch (...) {
                // join 失败时不吞掉更严重的问题:记录后继续,避免析构抛出
                wxLogWarning(L"读线程未能正常结束");
        }

        try {
                if (m_mgmt_thread.joinable()) {
                        m_mgmt_thread.join();
                }
        } catch (...) {
                wxLogWarning(L"管理接口线程未能正常结束");
        }

        if (m_treeListCtrl) {
                m_treeListCtrl->SetItemComparator(nullptr);
        }
}

void MainFrame::check_view_appearance(_In_ int appearance)
{
        auto id = get_appearance_itemid(appearance);

        if (auto itm = m_view_appearance->FindItem(id); itm && itm->GetKind() == wxITEM_RADIO) {
                itm->Check();
        } else {
                wxLogError(L"视图/外观: 无效的外观值 %d", appearance);
        }
}

void MainFrame::init()
{
        m_log->SetVerbose(true); // produce messages for wxLOG_Info
        m_log->SetLogLevel(DEFAULT_LOGLEVEL);

        SetTitle(wxString(L"USB-SHARE 客户端"));

        SetIcon(wxIcon(L"USBip")); // see wxwidgets.rc
        
        if (auto cfg = wxConfig::Get()) {
                cfg->SetStyle(wxCONFIG_USE_LOCAL_FILE);
        }

        set_menu_columns_labels();
        m_comboBoxServer->SetMaxLength(NI_MAXHOST);

        /*
         * 服务器地址与端口的持久化:ComboBox 的当前值与 SpinCtrl 的端口都没有内置
         * 持久化机制,关闭再开就回到 get_tcp_port() 默认 3240。用户服务端 NAS 用的是
         * 单端口 5555,每次启动端口不对就连不上、看不到设备,服务端也看不到心跳。
         *
         * 启动时优先从 wxConfig 读 ServerAddr / ServerPort 覆盖默认;控件文字或数值
         * 变化时立即写回 wxConfig,这样不需要走"读取设备"按钮也能保存。
         *
         * 注意:Bind 顺序很关键 —— 必须先 Bind wxEVT_TEXT,再 SetValue。否则 wx
         * 在 SetValue 时会触发一次 wxEVT_TEXT,lambda 此时 GetValue() 已读到
         * saved_addr 没问题;但某些 wx 版本会在控件初始化完成前 post 一次
         * 携带空值的 wxEVT_TEXT,这种"前置事件"会把 saved_addr 覆盖为空。
         * 所以再叠加一道"和上次保存值一致就不写"的护栏,无论哪种顺序都不会丢。
         */
        {
                /*
                 * 顺序不能反:必须先把品牌改造前写在旧根键 ("Software\usbip-win2\USBip")
                 * 下的设置搬进当前路径,再读 —— 否则老用户升级后第一次启动会读空
                 * ServerPort/ClientId,表现为"设置丢了"。只在新路径缺值时才迁,
                 * 不覆盖用户已设置的值;旧键保留不删,留作回滚保险。
                 */
                mgmt::migrate_legacy_config();

                /*
                 * 与 mgmt.cpp 里的 cfg_read_int 同等逻辑:容忍 REG_SZ/REG_DWORD 混用,
                 * 失败回退到 0(再走下方"未保存过端口"分支用 get_tcp_port() 默认值)。
                 * 这里不复用 mgmt.cpp 是因为它没导出,直接在 wusbip.cpp 内联。
                 */
                wxString saved_addr;
                long saved_port = 0;
                if (auto cfg = wxConfig::Get()) {
                        wxLogNull no_log; // 脏数据不要弹错
                        saved_addr = cfg->Read(L"ServerAddr", wxString());

                        /*
                         * 根键必须与 wxConfig 完全一致 —— 用 win::config_registry_root()
                         * (取自 exe 版本资源的 CompanyName\ProductName),而不是硬编码。
                         *
                         * 历史 bug:这里曾写死旧品牌 "Software\usbip-win2\USBip",而写入
                         * 走的是 wxConfig 的新路径 "Software\USB-SHARE\USB-SHARE"(品牌改名
                         * 后 wxConfig 根路径跟着版本资源变了)。结果端口写进新路径却从旧
                         * 路径读,新电脑上旧键根本不存在 —— 用户看到"服务器地址保存了,
                         * 端口没保存"(ServerAddr 走 cfg->Read,读写同路径所以正常)。
                         */
                        wxRegKey key(wxRegKey::HKCU, win::config_registry_root());
                        if (key.Exists() && key.Open(wxRegKey::Read) && key.HasValue(L"ServerPort")) {
                                const auto t = key.GetValueType(L"ServerPort");
                                if (t == wxRegKey::Type_Dword) {
                                        key.QueryValue(L"ServerPort", &saved_port);
                                } else if (t == wxRegKey::Type_String) {
                                        wxString s;
                                        long v = 0;
                                        if (key.QueryValue(L"ServerPort", s) && s.ToLong(&v)) {
                                                saved_port = v;
                                        }
                                }
                        }

                        // 启动时主动清理已知的旧版遗留值 (WebPort=18080)。
                        // 单端口网关 (Phase B) 下管理接口跟数据端口走同一端口,
                        // 18080 这种独立管理端口是无效值 —— 必须主动删,否则
                        // mgmt_loop 会一直去连 18080 然后 fallback 到 data_port,
                        // 心跳成功概率极低。
                        if (key.Exists() && key.HasValue(L"WebPort") && key.Open(wxRegKey::Write)) {
                                const auto t = key.GetValueType(L"WebPort");
                                long v = 0;
                                if (t == wxRegKey::Type_Dword) {
                                        key.QueryValue(L"WebPort", &v);
                                } else if (t == wxRegKey::Type_String) {
                                        wxString s;
                                        key.QueryValue(L"WebPort", s);
                                        long tmp = 0;
                                        if (s.ToLong(&tmp)) v = tmp;
                                }
                                if (v == 18080) {
                                        key.DeleteValue(L"WebPort");
                                        wxLogVerbose(L"已清理遗留 WebPort=18080");
                                }
                        }
                }

                // 先 Bind 事件,确保任何 SetValue/AddString 都触发持久化逻辑。
                // 持久化逻辑里"和上次值一致就不写"的检查会过滤掉启动瞬间的
                // 空值回写,所以先 Bind 后 SetValue 是安全的。
                //
                // 关键:绝不用 cfg->Write 写空字符串 —— wxConfig::Write(key, "") 在
                // wxWidgets 上确实会保留一个空的 REG_SZ 值,然后 reg.exe 显示"键还在但
                // 值是空",下次启动 cfg->Read 读到空,ComboBox 失去历史地址。
                // 因此 lambda 里一律跳过空值,只接受 ComboBox 真的有内容才写。
                m_comboBoxServer->Bind(wxEVT_TEXT, [this](wxCommandEvent&) {
                        if (auto cfg = wxConfig::Get()) {
                                auto cur = m_comboBoxServer->GetValue();
                                if (!cur.empty()) {
                                        auto last = cfg->Read(L"ServerAddr", wxString());
                                        if (cur != last) {
                                                cfg->Write(L"ServerAddr", cur);
                                                cfg->Flush();
                                        }
                                }
                        }
                });
                m_spinCtrlPort->Bind(wxEVT_SPINCTRL, [this](wxSpinEvent&) {
                        if (auto cfg = wxConfig::Get()) {
                                auto cur = m_spinCtrlPort->GetValue();
                                if (cur >= 1025 && cur <= 65535) {
                                        long last = 0;
                                        cfg->Read(L"ServerPort", &last);
                                        if (cur != static_cast<int>(last)) {
                                                cfg->Write(L"ServerPort", cur);
                                                cfg->Flush();
                                        }
                                }
                        }
                });
                /*
                 * wxEVT_SPINCTRL 只在"点上下箭头 / 回车 / 失焦提交"时才触发。用户
                 * 手工键入端口号后,如果直接点"连接"或关窗口,事件可能来不及到达 ——
                 * 端口就不落盘(而服务器地址走的是 wxEVT_TEXT,所以"地址存住了、
                 * 端口没存住"这种不对称很容易出现)。
                 *
                 * 这里补一个 wxEVT_TEXT 兜住手工输入,只写合法值(1025..65535);
                 * 与上面的 wxEVT_SPINCTRL 共用"和上次保存值一致就不写"的护栏,
                 * 因此不会重复写盘。
                 */
                m_spinCtrlPort->Bind(wxEVT_TEXT, [this](wxCommandEvent&) {
                        wxLogNull no_log; // 旧版遗留的 REG_SZ 型 ServerPort 不该弹 wxLogError
                        if (auto cfg = wxConfig::Get()) {
                                auto cur = m_spinCtrlPort->GetValue();
                                if (cur >= 1025 && cur <= 65535) {
                                        long last = 0;
                                        cfg->Read(L"ServerPort", &last);
                                        if (cur != static_cast<int>(last)) {
                                                cfg->Write(L"ServerPort", cur);
                                                cfg->Flush();
                                        }
                                }
                        }
                });
                m_comboBoxServer->Bind(wxEVT_COMBOBOX, [this](wxCommandEvent&) {
                        if (auto cfg = wxConfig::Get()) {
                                auto cur = m_comboBoxServer->GetValue();
                                if (!cur.empty()) {
                                        auto last = cfg->Read(L"ServerAddr", wxString());
                                        if (cur != last) {
                                                cfg->Write(L"ServerAddr", cur);
                                                cfg->Flush();
                                        }
                                }
                        }
                });

                if (!saved_addr.empty()) {
                        m_comboBoxServer->SetValue(saved_addr);
                        /*
                         * 二次加固:延迟到事件循环稳定后再设一次。某些 wx 版本 + AUIManager
                         * 组合在 ctor 末尾的 SetValue 会被工具栏布局/主题切换等后续事件
                         * 复位(实测 ComboBox.GetValue() 在这里已是空);CallAfter 推迟到
                         * 主循环稳定后再次写入,确保最终显示非空。
                         */
                        CallAfter([this, addr = saved_addr] {
                                m_comboBoxServer->SetValue(addr);
                        });
                }
                if (saved_port >= 1025 && saved_port <= 65535) {
                        m_spinCtrlPort->SetValue(static_cast<int>(saved_port));
                } else {
                        auto port = get_tcp_port();
                        m_spinCtrlPort->SetValue(wxString::FromAscii(port)); // NI_MAXSERV
                }
        }

        init_tree_list();
        apply_theme();

        Bind(wxEVT_TIMER, &MainFrame::on_status_bar_timer, this, ID_STATUS_BAR_TIMER);
        Bind(wxEVT_TIMER, &MainFrame::on_window_fix_timer, this, ID_WINDOW_FIX_TIMER);
        Bind(wxEVT_TIMER, &MainFrame::on_taskbar_watchdog, this, ID_TASKBAR_WATCHDOG);
        Bind(EVT_DEVICE_STATE, &MainFrame::on_device_state, this);
        Bind(wxEVT_ICONIZE, &MainFrame::on_iconize, this);
        Bind(wxEVT_MENU, &MainFrame::toggle_auto_connect, this, ID_AUTO_CONNECT);
        Bind(wxEVT_MENU, &MainFrame::mgmt_on_enable, this, ID_MGMT_ENABLE);
        Bind(wxEVT_MENU, &MainFrame::on_auto_start, this, ID_AUTO_START);
        Bind(wxEVT_MENU, &MainFrame::on_local_name, this, ID_LOCAL_NAME);
        Bind(wxEVT_THREAD, &MainFrame::mgmt_apply_event, this, wxID_ANY);

        if (auto itm = m_menu_file->FindItem(ID_MGMT_ENABLE)) {
                itm->Check(m_mgmt_enabled);
        }

        sync_auto_start_menu();
        update_summary();

        // 托盘图标常驻:程序一启动就在系统托盘显示(不只在隐藏时才出现)
        ensure_taskbar_icon();
        // Explorer 重启、任务栏崩溃重建、或 Windows 重新加载通知区域时,
        // 旧的 wxTaskBarIcon 句柄可能失效;定期自愈,不需要用户重启客户端。
        // 周期 5 秒:只用于图标自愈(枚举注册表有一定开销,不必更频繁)。
        m_taskbar_watchdog_timer.Start(5000);

        // 清理驱动残留的自动重连"连接尝试":旧版本曾用持续连接(attach 非 once)
        // 给驱动留下过自动重连会话——即使客户端退出、界面未勾 Auto,驱动仍会周期
        // 自动 attach(表现为"关了还在连/反复断连重连")。此调用只停止未完成的连接
        // 尝试,不影响已经连接成功的设备。
        if (auto &vhci = get_vhci(); vhci::stop_attach_attempts(vhci.get(), nullptr) < 0) {
                // 无可用 vhci/打开失败:忽略,不打扰用户
        }
}

void MainFrame::apply_theme()
{
        const auto p = make_ui_palette();

        SetBackgroundColour(p.window);
        SetForegroundColour(p.text);

        if (m_statusBar) {
                m_statusBar->SetBackgroundColour(p.surface_alt);
                m_statusBar->SetForegroundColour(p.text);
        }

        for (auto toolbar: {m_auiToolBar, m_auiToolBarAdd}) {
                if (toolbar) {
                        toolbar->SetBackgroundColour(p.surface);
                        toolbar->SetForegroundColour(p.text);
                }
        }

        for (auto label: {m_staticTextServer, m_staticTextPort}) {
                if (label) {
                        label->SetForegroundColour(p.muted_text);
                }
        }

        if (m_comboBoxServer) {
                m_comboBoxServer->SetBackgroundColour(p.surface);
                m_comboBoxServer->SetForegroundColour(p.text);
        }
        if (m_spinCtrlPort) {
                m_spinCtrlPort->SetBackgroundColour(p.surface);
                m_spinCtrlPort->SetForegroundColour(p.text);
        }
        if (m_button_add) {
                m_button_add->SetBackgroundColour(p.accent);
                m_button_add->SetForegroundColour(p.accent_text);
        }

        if (m_treeListCtrl) {
                m_treeListCtrl->SetBackgroundColour(p.table);
                m_treeListCtrl->SetForegroundColour(p.text);

                if (auto view = m_treeListCtrl->GetDataView()) {
                        view->SetBackgroundColour(p.table);
                        view->SetForegroundColour(p.text);
                        // Windows 原生 DataView 可能忽略 alternate row colour; generic
                        // backend 会使用它,因此无论平台都安全调用。
                        view->SetAlternateRowColour(p.alternate_row);
                }
        }

        if (auto hdr = m_treeListCtrl ? m_treeListCtrl->GetDataView()->GenericGetHeader() : nullptr) {
                // 禁用旧的绿色表头,改用与正文一致的中性文字色。
                hdr->SetBackgroundColour(p.surface_alt);
                hdr->SetForegroundColour(p.header_text);
        }

        if (auto art = m_mgr.GetArtProvider()) {
                art->SetColour(wxAUI_DOCKART_BACKGROUND_COLOUR, p.surface_alt);
                art->SetColour(wxAUI_DOCKART_SASH_COLOUR, p.border);
                art->SetColour(wxAUI_DOCKART_BORDER_COLOUR, p.border);
                art->SetColour(wxAUI_DOCKART_ACTIVE_CAPTION_COLOUR, p.surface_alt);
                art->SetColour(wxAUI_DOCKART_ACTIVE_CAPTION_GRADIENT_COLOUR, p.surface_alt);
                art->SetColour(wxAUI_DOCKART_INACTIVE_CAPTION_COLOUR, p.surface);
                art->SetColour(wxAUI_DOCKART_INACTIVE_CAPTION_GRADIENT_COLOUR, p.surface);
                art->SetColour(wxAUI_DOCKART_ACTIVE_CAPTION_TEXT_COLOUR, p.text);
                art->SetColour(wxAUI_DOCKART_INACTIVE_CAPTION_TEXT_COLOUR, p.muted_text);
                m_mgr.Update();
        }
}

void MainFrame::init_tree_list()
{
        auto &tree = *m_treeListCtrl;
        auto &dv = *tree.GetDataView();

        tree.SetImages(get_tree_images());
        tree.SetItemComparator(&m_tree_cmp);

        tree.GetView()->Bind(wxEVT_MOUSEWHEEL, wxMouseEventHandler(MainFrame::on_tree_mouse_wheel), this);

        if (auto hdr = dv.GenericGetHeader()) {
                hdr->Bind(wxEVT_MOUSEWHEEL, wxMouseEventHandler(MainFrame::on_tree_mouse_wheel), this);
        }
}

wxWithImages::Images MainFrame::get_tree_images()
{
        auto dark = wxSystemSettings::GetAppearance().IsDark();

        auto stock_bmp = wxArtProvider::GetBitmap(wxASCII_STR(wxART_GO_HOME), wxASCII_STR(wxART_LIST));
        auto size = stock_bmp.GetSize();

        return wxWithImages::Images { 
                wxBitmapBundle::FromSVGResource(wxASCII_STR(dark ? "computer_svg_dark" : "computer_svg"), size),
                wxBitmapBundle::FromSVGResource(wxASCII_STR(dark ? "usb_svg_dark" : "usb_svg"), size)
        };

        static_assert(IMG_SERVER == 0);
        static_assert(IMG_DEVICE == 1);
}

void MainFrame::configure_workspace_ui()
{
        // One stable row: server + port on the left, device actions on the right.
        // Low-frequency operations (停止/全部断开/加载/保存) stay in the menus and the
        // device context menu instead of claiming a second toolbar row.
        m_mgr.DetachPane(m_auiToolBar);
        m_auiToolBar->Hide(); // Keep generated item pointers alive for Frame's destructor.

        // Stretch spacer pushes 连接/断开/刷新 to the far right of this same row, so the
        // empty space that used to sit to their left is taken up by the fields above.
        m_auiToolBarAdd->AddStretchSpacer();
        for (auto item : {m_tool_attach, m_tool_detach, m_tool_reload}) {
                m_auiToolBarAdd->AddTool(item->GetId(), item->GetLabel(), item->GetBitmap(), item->GetShortHelp());
        }
        const auto palette = make_ui_palette();
        m_auiToolBarAdd->SetBackgroundColour(palette.surface);
        m_auiToolBarAdd->SetForegroundColour(palette.text);
        m_auiToolBarAdd->SetArtProvider(new FlatToolBarArt());
        m_auiToolBarAdd->SetWindowStyleFlag((m_auiToolBarAdd->GetWindowStyleFlag() & ~wxAUI_TB_GRIPPER)
                                            | wxAUI_TB_HORZ_LAYOUT | wxAUI_TB_TEXT);
        m_auiToolBarAdd->SetMargins(FromDIP(12), FromDIP(8));
        m_auiToolBarAdd->SetToolPacking(FromDIP(8));
        m_auiToolBarAdd->SetToolBorderPadding(FromDIP(5));

        // 工具栏文字的显隐开关(视图菜单)现在应当作用于这条可见工具栏。
        m_primary_bar = m_auiToolBarAdd;

        m_staticTextServer->SetLabel(L"服务端");
        m_comboBoxServer->SetMinSize(FromDIP(wxSize(230, -1)));
        m_spinCtrlPort->SetMinSize(FromDIP(wxSize(82, -1)));
        m_button_add->SetLabel(L"读取设备");
        m_button_add->SetToolTip(L"读取此服务器的共享列表，不会自动连接设备");
        m_button_add->SetBitmap(wxNullBitmap);
        m_comboBoxServer->SetHint(L"IP 地址或域名");
        m_auiToolBarAdd->Realize();
        m_mgr.GetPane(m_auiToolBarAdd).Layer(10).Row(0).Position(0).Gripper(false).PaneBorder(false)
                .BestSize(m_auiToolBarAdd->GetBestSize());
        m_mgr.GetPane(m_treeListCtrl).CaptionVisible(false);
        m_mgr.Update();

        // Move preference toggles into their own menu, preserving existing bindings.
        //
        // 关键:这里不能用 lambda 把事件 ProcessEvent 到 m_menu_file —— wxMenu
        // 不是 MainFrame 事件链的一部分,ProcessEvent 跑到 m_menu_file 上找不到
        // Bind(TryBefore 静态路径走完),GetNextHandler() 默认是 nullptr,事件
        // 就这么丢了 —— 用户点 "文件 → 偏好设置 → 本机名称…" 完全没反应。
        //
        // 直接调用 MainFrame 上对应的处理函数即可,可读性也比 ProcessEvent 高。
        // checkable items 的勾选切换由 wxMenu::TryBefore 在事件分派前自动处理,
        // 不需要我们插手。
        auto settings = new wxMenu();
        const auto items = m_menu_file->GetMenuItems();
        std::vector<wxMenuItem*> preferences;
        for (auto item : items) {
                if (item->IsCheckable() || item->GetId() == ID_LOCAL_NAME) preferences.push_back(item);
        }
        for (auto item : preferences) {
                auto id = item->GetId();
                settings->Append(m_menu_file->Remove(item));
                settings->Bind(wxEVT_MENU, [this, id](wxCommandEvent& event) {
                        switch (id) {
                        case ID_AUTO_CONNECT:   toggle_auto_connect(event);   break;
                        case ID_MGMT_ENABLE:    mgmt_on_enable(event);        break;
                        case ID_AUTO_START:     on_auto_start(event);         break;
                        case ID_LOCAL_NAME:     on_local_name(event);         break;
                        /*
                         * 这两个项早先是用 wxID_ANY 建的,搬到「偏好设置」子菜单后
                         * 没有稳定 ID 可分发,只能靠菜单链传播到 m_menu_file 上的
                         * 旧绑定 —— 那条链路在子菜单下不可靠(表现为勾了没反应)。
                         * 现在给了固定 ID,这里显式分发。
                         */
                        case ID_START_IN_TRAY:  on_start_in_tray(event);      break;
                        case ID_CLOSE_TO_TRAY:  on_close_to_tray(event);      break;
                        }
                }, id);
        }
        m_menu_file->AppendSubMenu(settings, L"偏好设置");

        auto fit = m_menu_view->AppendCheckItem(wxID_ANY, L"自动分配列宽", L"按窗口宽度分配设备信息，手动拖动列宽时暂停");
        m_auto_fit_columns = wxConfig::Get()->ReadBool(L"AutoFitColumns", true);
        fit->Check(m_auto_fit_columns);
        m_menu_view->Bind(wxEVT_MENU, [this](wxCommandEvent& e) {
                m_auto_fit_columns = e.IsChecked();
                wxConfig::Get()->Write(L"AutoFitColumns", m_auto_fit_columns);
                fit_visible_columns();
        }, fit->GetId());
        Bind(wxEVT_UPDATE_UI, [this](wxUpdateUIEvent& e) { e.Check(m_auto_fit_columns); }, fit->GetId());
        auto& view = *m_treeListCtrl->GetDataView();
        auto table_font = view.GetFont();
        table_font.SetFaceName(L"Microsoft YaHei UI");
        m_treeListCtrl->SetFont(table_font);
        view.SetFont(table_font);
        view.SetRowHeight(FromDIP(32));
        const auto p = make_ui_palette();
        view.SetHeaderAttr(wxItemAttr(p.header_text, p.surface_alt, GetFont()));
        view.Bind(wxEVT_SIZE, [this](wxSizeEvent& e) {
                e.Skip();
                if (!m_column_fit_pending) {
                        m_column_fit_pending = true;
                        CallAfter([this] { m_column_fit_pending = false; fit_visible_columns(); });
                }
        });
        if (auto header = view.GenericGetHeader()) {
                header->Bind(wxEVT_HEADER_END_RESIZE, [this](wxHeaderCtrlEvent& e) {
                        if (!m_fitting_columns) {
                                m_auto_fit_columns = false;
                                wxConfig::Get()->Write(L"AutoFitColumns", false);
                        }
                        e.Skip();
                });
        }
        m_statusBar->SetStatusText(L"选择设备后连接 · 高级信息可在“视图 → 列”中展开", 0);
        const int widths[] = {-1, FromDIP(245)};
        m_statusBar->SetStatusWidths(2, widths);
}

void MainFrame::fit_visible_columns()
{
        if (!m_auto_fit_columns || m_fitting_columns || !m_treeListCtrl) return;
        auto& view = *m_treeListCtrl->GetDataView();
        const int available = view.GetClientSize().x - FromDIP(10);
        if (available < FromDIP(200)) return;
        struct column_width { wxDataViewColumn* col; int minimum; int weight; };
        std::vector<column_width> visible;
        int total_min = 0, total_weight = 0;
        for (unsigned i = 0; i < view.GetColumnCount(); ++i) {
                auto col = view.GetColumn(i);
                if (col->IsHidden()) continue;
                const auto title = col->GetTitle();
                int minimum = 90, weight = 1;
                if (title == L"服务器 / 总线号") { minimum = 160; weight = 3; }
                else if (title == L"产品") { minimum = 140; weight = 4; }
                /*
                 * 备注没有和产品绑在一起:它是"偶尔写一句"的字段,绝大多数行
                 * 是空的,给和产品一样的权重会白占一截横向空间。
                 */
                else if (title == L"备注") { minimum = 90; weight = 1; }
                else if (title == L"状态") { minimum = 88; weight = 1; }
                else if (title == L"自动") { minimum = 62; weight = 1; }
                minimum = FromDIP(minimum);
                visible.push_back({col, minimum, weight});
                total_min += minimum; total_weight += weight;
        }
        if (!total_weight) return;
        const int extra = std::max(0, available - total_min);
        m_fitting_columns = true;
        for (auto item : visible) {
                const int width = item.minimum + extra * item.weight / total_weight;
                if (item.col->WXGetSpecifiedWidth() != width) item.col->SetWidth(width);
        }
        m_fitting_columns = false;
}

void MainFrame::apply_compact_columns()
{
        constexpr auto kLayoutVersion = 4L;
        constexpr wchar_t kLayoutKey[] = L"CompactColumnsVersion";

        auto cfg = wxConfig::Get();
        if (!cfg || cfg->ReadLong(kLayoutKey, 0L) >= kLayoutVersion) {
                return; // 已迁移过,以后尊重用户在"视图 -> 列"里的手动选择
        }

        struct default_column {
                const wchar_t *title;
                int width;
                bool hide;
        };

        /*
         * 默认只保留日常连接真正需要的信息:
         *   服务器/总线号 + 产品 + 状态 + 自动 + 备注
         *
         * 设备列表现在不是把 14 列平均摊开,而是给常用信息更充足的宽度。
         * 低频列仍然可以从"视图 -> 列"随时打开,只是首次使用时不占屏幕:
         *   端口/速率/厂商       - 硬件拓扑与 USB 描述信息,大多数时候不需要
         *   序列号/接收模式      - 高级诊断/设备区分信息
         *   源ID/设备ID          - 内部诊断字段(本来就默认隐藏)
         *   服务器备注/占用方    - 管理接口高级信息,空白时尤其浪费宽度
         *
         * 宽度是物理像素,按 4K/200% 屏幕实际效果调过:服务器和产品名不再被
         * 截断,备注也留有足够空间。按标题查找而不是按枚举下标,因为用户可能
         * 已经拖动过列顺序。
         */
        constexpr default_column columns[] = {
                {L"服务器 / 总线号", 420, false},
                {L"端口",             100, true},
                {L"速率",             100, true},
                {L"厂商",             220, true},
                {L"产品",             220, false},
                {L"状态",             130, false},
                {L"自动",             100, false},
                {L"备注",             180, false},
                {L"设备ID",           150, true},
                {L"序列号",           220, true},
                {L"接收模式",         160, true},
                {L"源ID",             120, true},
                {L"服务器备注",       260, true},
                {L"占用方",           200, true},
        };

        int changed{};
        for (const auto &item: columns) {
                if (auto col = find_column(item.title)) {
                        if (item.hide && !col->IsHidden()) {
                                col->SetHidden(true);
                                ++changed;
                        }
                        col->SetWidth(item.width);
                }
        }

        /*
         * wxPersistentDataViewCtrl 通常在窗口退出时才保存 Hidden/Width 值,但布局
         * 版本标记现在就要写入。若用户此时直接杀进程,下次启动看到版本 2 却没有
         * 迁移值,布局会恢复成旧的拥挤状态。因此把这次迁移的属性立即写入同一
         * 个持久化路径;之后 wxPersistentDataViewCtrl 仍会继续保存用户手动调整。
         * 对默认可见列不强写 Hidden,避免覆盖用户在升级前已经主动隐藏的列。
         */
        const auto old_path = cfg->GetPath();
        constexpr wchar_t kColumnsPath[] = L"/Persistent_Options/DataView/wxTreeListCtrl/Columns";
        for (const auto &item: columns) {
                /*
                 * 每轮都从完整路径开始。列标题"服务器 / 总线号"本身含有 '/',
                 * wxConfig 会把它编码成持久化路径的两级节点;不能用 SetPath("..")
                 * 依赖相对路径回退,否则下一列会被误写到"服务器"子键下面。
                 */
                cfg->SetPath(kColumnsPath);
                cfg->SetPath(item.title);
                cfg->Write(L"Width", item.width);
                if (item.hide) {
                        cfg->Write(L"Hidden", true);
                }
        }
        cfg->SetPath(old_path);
        cfg->Write(kLayoutKey, kLayoutVersion);
        cfg->Flush();
        wxLogVerbose(L"已应用紧凑设备列表布局,隐藏 %d 个低频列并放宽常用列;可从“视图 -> 列”恢复", changed);
}

/*
 * 「备注」列宽一次性收敛。
 *
 * 备注是"偶尔写一句"的字段,绝大多数行是空的,之前却和产品同宽,白占一截
 * 横向空间。这里把它压窄。
 *
 * 刻意**不**去改 apply_compact_columns() 的 kLayoutVersion: 提高那个版本号
 * 会让整套默认布局(含各列隐藏/显示)重新灌一遍,把用户多年在"视图 -> 列"
 * 里的手动选择全部冲掉。这里只动这一列的宽度,并用独立的版本键记录,
 * 之后用户手动拖宽/拖窄依然会被正常持久化。
 */
void MainFrame::apply_note_column_width()
{
        constexpr auto kNoteLayoutVersion = 1L;
        constexpr wchar_t kNoteLayoutKey[] = L"NoteColumnWidthVersion";
        constexpr int kNoteWidth = 180;

        auto cfg = wxConfig::Get();
        if (!cfg || cfg->ReadLong(kNoteLayoutKey, 0L) >= kNoteLayoutVersion) {
                return; // 已收敛过,之后尊重用户的手动宽度
        }

        constexpr wchar_t kColumnsPath[] =
                L"Persistent_Options/DataView/wxTreeListCtrl/Columns";

        if (auto col = find_column(L"备注")) {
                col->SetWidth(kNoteWidth);
        }

        // 立即写入,避免这次调整在用户直接杀进程后丢失(见 apply_compact_columns 的同类说明)
        const auto old_path = cfg->GetPath();
        cfg->SetPath(kColumnsPath);
        cfg->SetPath(L"备注");
        cfg->Write(L"Width", kNoteWidth);
        cfg->SetPath(old_path);

        cfg->Write(kNoteLayoutKey, kNoteLayoutVersion);
        cfg->Flush();

        wxLogVerbose(L"已把“备注”列宽收敛为 %d", kNoteWidth);
}

void MainFrame::restore_state()
{
        wxPersistentRegisterAndRestore(this, L"MainFrame"); // @see persist.h
        wxPersistentRegisterAndRestore(m_treeListCtrl->GetDataView(), m_treeListCtrl->GetName());
        apply_compact_columns();
        apply_note_column_width();
        apply_default_tray_autostart();
}

/*
 * 修一个由"恢复上次窗口状态"引入的坑:
 *
 * 上次退出时窗口是最小化的 -> wx 会把 Iconized 一并持久化并在下次启动时恢复,
 * 进而触发 on_iconize() -> iconize_to_tray() 把窗口 Hide() 掉。桌面上什么都
 * 没有、只剩托盘图标,用户会以为"程序根本没启动"。注意这与"启动时最小化到
 * 托盘"选项无关: StartInTray 明明是 0,窗口照样被藏了。
 *
 * 顺序很关键: 必须先把窗口从最小化恢复出来,再判断几何。
 * 最小化状态下 GetWindowRect 返回的是 -32000 之类的占位矩形(尺寸是最小化后
 * 的 ~314x50),拿它去做"夹回工作区"会把窗口尺寸改坏。
 */
void MainFrame::show_initial_window()
{
        if (IsIconized()) Iconize(false);
        Show();
        ensure_visible_window();
        fit_visible_columns();
}

/*
 * 单实例:处理第二个实例广播过来的"把你自己显示出来"消息。
 *
 * 这是顶层窗口的消息处理入口。wx 会把所有未识别的 HWND 消息送到这里,
 * 我们先比对是不是我们注册的那个消息,是就自己消费掉、不要再交给
 * wxFrame::MSWWindowProc 让它继续往下传(否则可能被当作未知消息丢掉)。
 */
WXLRESULT MainFrame::MSWWindowProc(_In_ WXUINT msg, _In_ WXWPARAM wParam, _In_ WXLPARAM lParam)
{
        if (msg && msg == App::instance_message()) {
                on_activate_from_second_instance();
                return 0;
        }

        return wxFrame::MSWWindowProc(msg, wParam, lParam);
}

/*
 * 用户双击了桌面/开始菜单图标,但已经有一个实例在跑 —— 把已有窗口
 * 从"托盘里藏着"或"最小化"状态收拾出来,放到前台。
 *
 * 直接复用 restore_from_tray():它本来就是"托盘/最小化还原的唯一入口",
 * 内部已经处理好 Iconize(false) -> Show() -> ensure_visible_window()
 * -> fit_visible_columns() -> Raise() 的完整顺序。自己再写一遍只会
 * 和那条既有逻辑分叉。
 *
 * 注意不能用 show_initial_window():那条路径在 m_start_in_tray 时会被
 * ensure_visible_window() 直接 return 掉(那是给"开机自启"用的规则,
 * 目的是别打扰用户)。而这里用户是**明确点了图标**要看到窗口。
 */
void MainFrame::on_activate_from_second_instance()
{
        restore_from_tray();

        auto hwnd = static_cast<HWND>(GetHandle());
        if (!hwnd) {
                set_status_text(L"程序已在运行,已切换到主窗口");
                return;
        }

        /*
         * 抢前台。
         *
         * 背景:第二个实例在广播消息之前已经调用过 AllowSetForegroundWindow(本进程PID),
         * 把"这次用户触发带来的前台权限"转交给了我们,所以正常路径下
         * SetForegroundWindow 是能成功的。
         *
         * 但仍然有两道系统的额外限制可能让它失败:
         *  1) 前台锁定超时(默认 200000ms)内,连有权限的进程也不一定能立刻抢到;
         *  2) 本进程当前线程若不在前台输入队列里,SetForegroundWindow 会被静默忽略。
         *
         * 因此这里用两步:
         *  先 AttachThreadInput 挂到当前前台线程的输入队列上(消除限制 2),
         *  再 SetForegroundWindow;仍失败则用"临时置顶再取消"兜底 ——
         *  HWND_TOPMOST 的切换不需要前台权限,能把窗口强行提到最前。
         */
        auto mine = ::GetCurrentThreadId();

        auto try_foreground = [&]() -> bool {
                if (auto fg = ::GetForegroundWindow(); fg && fg != hwnd) {
                        auto cur = ::GetWindowThreadProcessId(fg, nullptr);
                        if (cur && cur != mine) {
                                ::AttachThreadInput(cur, mine, TRUE);
                                ::SetForegroundWindow(hwnd);
                                ::SetFocus(hwnd);
                                ::AttachThreadInput(cur, mine, FALSE);
                                return ::GetForegroundWindow() == hwnd;
                        }
                }
                ::SetForegroundWindow(hwnd);
                ::SetFocus(hwnd);
                return ::GetForegroundWindow() == hwnd;
        };

        if (!try_foreground()) {
                /*
                 * 兜底:临时把窗口设成 TOPMOST 再取消。
                 *
                 * SetWindowPos(HWND_TOPMOST) 不受前台权限限制,一定能把窗口
                 * 提到所有普通窗口之上;之后立刻取消 TOPMOST,窗口就"留在"
                 * 最前面且不打扰其他窗口的层级关系。这是各输入法/托盘程序
                 * 通用的"强行露脸"手法。
                 */
                ::SetWindowPos(hwnd, HWND_TOPMOST, 0, 0, 0, 0,
                               SWP_NOMOVE | SWP_NOSIZE | SWP_SHOWWINDOW);
                ::SetWindowPos(hwnd, HWND_NOTOPMOST, 0, 0, 0, 0,
                               SWP_NOMOVE | SWP_NOSIZE | SWP_SHOWWINDOW);
                try_foreground(); // 再试一次,失败也无妨
        }

        set_status_text(L"程序已在运行,已切换到主窗口");
}

void MainFrame::ensure_visible_window()
{
        if (m_start_in_tray) {
                return; // 用户要的就是托盘常驻,别去打扰
        }

        // 1) 先清掉"上次退出时是最小化"的状态
        if (IsIconized()) {
                Iconize(false);
        }
        if (!IsShown()) {
                Show(true);
                Raise();
        }

        // 2) 还没恢复过来就别碰几何,等下一次(定时器兜底)再看
        if (IsIconized()) {
                return;
        }

        /*
         * 3) 窗口正常显示后再检查它是否落在当前显示器的工作区内。
         *
         * 这里用 Win32 原始坐标(物理像素),不走 wx 的几何 API: 实测
         * wxDisplay::GetClientArea() 给的是物理尺寸(3840x2064),而
         * wxWindow::GetRect() 给的是逻辑坐标(200% 缩放下只有一半),
         * 两者单位不一致会让"是否在工作区内"的判断完全失真。
         */
        auto hwnd = static_cast<HWND>(GetHandle());

        RECT r{};
        if (!::GetWindowRect(hwnd, &r)) {
                wxLogDebug(L"ensure_visible_window: GetWindowRect 失败 %lu", GetLastError());
                return;
        }

        auto mon = ::MonitorFromWindow(hwnd, MONITOR_DEFAULTTONEAREST);

        MONITORINFO mi{};
        mi.cbSize = sizeof(mi);
        if (!::GetMonitorInfo(mon, &mi)) {
                wxLogDebug(L"ensure_visible_window: GetMonitorInfo 失败 %lu", GetLastError());
                return;
        }

        auto &wa = mi.rcWork; // 工作区,已排除任务栏

        auto w = r.right - r.left;
        auto h = r.bottom - r.top;

        auto nw = std::min<LONG>(w, wa.right - wa.left);
        auto nh = std::min<LONG>(h, wa.bottom - wa.top);
        auto nx = std::clamp<LONG>(r.left, wa.left, wa.right  - nw);
        auto ny = std::clamp<LONG>(r.top,  wa.top,  wa.bottom - nh);

        if (nx != r.left || ny != r.top || nw != w || nh != h) {
                wxLogVerbose(L"窗口 %ld,%ld %ldx%ld 超出工作区 %ld,%ld %ldx%ld,夹回为 %ld,%ld %ldx%ld",
                        r.left, r.top, w, h,
                        wa.left, wa.top, wa.right - wa.left, wa.bottom - wa.top,
                        nx, ny, nw, nh);
                ::SetWindowPos(hwnd, nullptr, nx, ny, nw, nh, SWP_NOZORDER | SWP_NOACTIVATE);
        }
}

void MainFrame::sync_auto_connect_menu()
{
        if (auto itm = m_menu_file->FindItem(ID_AUTO_CONNECT)) {
                itm->Check(m_auto_connect);
        }
}

void MainFrame::toggle_auto_connect(_In_ wxCommandEvent&)
{
        if (auto itm = m_menu_file->FindItem(ID_AUTO_CONNECT)) {
                m_auto_connect = itm->IsChecked();
        }

        if (auto cfg = wxConfig::Get()) {
                cfg->Write(L"AutoConnect", m_auto_connect);
        }
}

namespace { // HKCU ...\Run autostart helpers

constexpr wchar_t kRunKeyPath[] = L"Software\\Microsoft\\Windows\\CurrentVersion\\Run";
constexpr wchar_t kRunValueName[] = L"USB-SHARE";
constexpr wchar_t kLegacyRunValueName[] = L"USB-IP 中文客户端";

bool read_auto_start()
{
        HKEY key{};
        if (RegOpenKeyExW(HKEY_CURRENT_USER, kRunKeyPath, 0, KEY_QUERY_VALUE, &key) != ERROR_SUCCESS) {
                return false;
        }
        auto rc = RegQueryValueExW(key, kRunValueName, nullptr, nullptr, nullptr, nullptr);
        if (rc != ERROR_SUCCESS) {
                // 升级旧版时兼容旧的启动项名称,避免用户的开机自启动选项突然失效。
                rc = RegQueryValueExW(key, kLegacyRunValueName, nullptr, nullptr, nullptr, nullptr);
        }
        RegCloseKey(key);
        return rc == ERROR_SUCCESS; // 值存在即视为已开启
}

bool write_auto_start(bool enable)
{
        if (!enable) {
                HKEY key{};
                if (RegOpenKeyExW(HKEY_CURRENT_USER, kRunKeyPath, 0, KEY_SET_VALUE, &key) != ERROR_SUCCESS) {
                        return true; // 打开失败视为目标已不存在
                }
                auto rc = RegDeleteValueW(key, kRunValueName);
                auto legacy_rc = RegDeleteValueW(key, kLegacyRunValueName);
                RegCloseKey(key);
                return (rc == ERROR_SUCCESS || rc == ERROR_FILE_NOT_FOUND) &&
                       (legacy_rc == ERROR_SUCCESS || legacy_rc == ERROR_FILE_NOT_FOUND);
        }

        wchar_t exe[MAX_PATH]{};
        if (!GetModuleFileNameW(nullptr, exe, MAX_PATH)) {
                return false;
        }

        auto cmd = wxString::Format(L"\"%s\"", exe);
        HKEY key{};
        if (RegOpenKeyExW(HKEY_CURRENT_USER, kRunKeyPath, 0, KEY_SET_VALUE, &key) != ERROR_SUCCESS) {
                return false;
        }
        auto rc = RegSetValueExW(key, kRunValueName, 0, REG_SZ,
                        reinterpret_cast<const BYTE*>(cmd.wc_str()), static_cast<DWORD>((cmd.Len()+1)*sizeof(wchar_t)));
        if (rc == ERROR_SUCCESS) {
                // 新名称写入成功后清掉旧名称,避免升级后两个客户端同时自启动。
                RegDeleteValueW(key, kLegacyRunValueName);
        }
        RegCloseKey(key);
        return rc == ERROR_SUCCESS;
}

} // namespace

void MainFrame::sync_auto_start_menu()
{
        if (auto itm = m_menu_file->FindItem(ID_AUTO_START)) {
                itm->Check(read_auto_start());
        }
}

void MainFrame::on_auto_start(_In_ wxCommandEvent&)
{
        auto enable = false;
        if (auto itm = m_menu_file->FindItem(ID_AUTO_START)) {
                enable = itm->IsChecked();
        }

        if (write_auto_start(enable)) {
                wxLogStatus(enable ? L"已开启开机自启动" : L"已关闭开机自启动");
        } else {
                wxLogError(enable ? L"无法开启开机自启动" : L"无法关闭开机自启动");
        }

        sync_auto_start_menu(); // 勾选状态与注册表实际保持一致
}

/*
 * 一次性把「启动时最小化到托盘」和「开机自启动」打开。
 *
 * 这是常驻型工具,期望的默认状态是:开机自动在后台起来、设备自动连上、
 * 桌面上不弹窗口。新装的实例由 m_start_in_tray 的成员默认值保证;
 * 这里负责让**已经装过**的实例也切到这个默认(否则老用户永远看不到变化)。
 *
 * 用独立版本键,只跑一次:跑完之后用户手动取消勾选依然会被正常持久化。
 *
 * 放在这里而不是 restore_state() 旁边,是因为 read_auto_start()/
 * write_auto_start() 定义在上方那个匿名 namespace 里,得晚于它们。
 */
void MainFrame::apply_default_tray_autostart()
{
        constexpr auto kVersion = 1L;
        constexpr wchar_t kKey[] = L"DefaultTrayAutostartVersion";

        auto cfg = wxConfig::Get();
        if (!cfg || cfg->ReadLong(kKey, 0L) >= kVersion) {
                return; // 已切过默认,之后尊重用户的选择
        }

        m_start_in_tray = true;

        /*
         * 必须立刻落到持久层,不能只改内存里的成员。
         *
         * 这个程序默认是"关闭 = 收进托盘后台"而不是退出,wxPersistent 的
         * Save() 只在真正退出时才写;用户可能几个月都不退出一次。若只改成员,
         * 下次启动 do_restore() 又会把注册表里那个旧的 StartInTray=0 读回来,
         * 迁移等于白做(实测过:迁移后窗口确实藏了,但重启又弹出来)。
         */
        {
                constexpr wchar_t kMainFramePath[] = L"Persistent_Options/Window/MainFrame";
                const auto old_path = cfg->GetPath();
                cfg->SetPath(kMainFramePath);
                cfg->Write(L"StartInTray", 1L);
                cfg->SetPath(old_path);
        }

        /*
         * 开机自启动:只在 Run 键不存在时才写。
         * 已经存在(用户以前自己开过)就不动,避免覆盖用户手改的命令行。
         */
        if (!read_auto_start()) {
                if (write_auto_start(true)) {
                        wxLogVerbose(L"已默认开启开机自启动");
                } else {
                        wxLogError(L"无法开启开机自启动");
                }
        }

        cfg->Write(kKey, kVersion);
        cfg->Flush();

        // 迁移可能刚写入 Run 键,菜单勾选要按注册表实际状态重画一次
        sync_auto_start_menu();
}

void MainFrame::update_summary()
{
        if (!m_statusBar || !m_treeListCtrl) {
                return;
        }

        auto &tree = *m_treeListCtrl;
        if (!tree.GetColumnCount()) {
                return;
        }

        int servers{}, devices{}, connected{};

        for (auto s = tree.GetFirstItem(); s.IsOk(); s = tree.GetNextSibling(s)) {
                if (!is_server(tree, s)) {
                        continue;
                }
                ++servers;
                for (auto d = tree.GetFirstChild(s); d.IsOk(); d = tree.GetNextSibling(d)) {
                        ++devices;
                        auto &st = tree.GetItemText(d, COL_STATE);
                        if (st == to_string(state::plugged)) {
                                ++connected;
                        }
                }
        }

        m_statusBar->SetStatusText(wxString::Format(L"服务器 %d · 设备 %d · 已连接 %d", servers, devices, connected), 1);
}

void MainFrame::schedule_auto_connect()
{
        if (!m_auto_connect) {
                return;
        }

        auto host = m_comboBoxServer->GetValue();
        if (host.empty()) {
                /*
                 * ComboBox 文本为空:可能是 ctor 末尾的 SetValue 在某些 wx 版本上
                 * 被 AUIManager / 主题切换复位。我们的持久化逻辑已在 wxConfig 里
                 * 保留了上次成功的 ServerAddr,但 schedule_auto_connect 是 ctor
                 * 末尾的同步调用,可能跑在 SetValue 生效之前。
                 *
                 * 这里主动把 wxConfig 里的 saved_addr 写回 ComboBox,并把 SpinCtrl
                 * 端口也同步,然后再发起 add_exported_devices —— 绕过"ComboBox
                 * 显示状态"的脆弱依赖,直接用 wxConfig 的真值驱动连接。
                 */
                if (auto cfg = wxConfig::Get()) {
                        auto addr = cfg->Read(L"ServerAddr", wxString());
                        if (!addr.empty()) {
                                m_comboBoxServer->SetValue(addr);
                                host = addr;

                                /*
                                 * 端口不在这里恢复:init() 已经按当前根键
                                 * (win::config_registry_root())读过 ServerPort 并写进
                                 * SpinCtrl(见 init() 里的 m_spinCtrlPort->SetValue),
                                 * 而 schedule_auto_connect() 是在 init() 之后才跑的。
                                 *
                                 * 这里曾再从旧品牌根键 "Software\usbip-win2\USBip" 读一次
                                 * 端口 —— 旧键里那个陈旧端口会把 init() 刚恢复的正确值覆盖
                                 * 掉,于是自动连接连到错端口。删掉这段冗余读取(它唯一的效果
                                 * 就是覆盖)。
                                 */
                                wxLogVerbose(L"[persist] schedule_auto_connect 从 wxConfig 恢复 ComboBox='%s' 端口=%d",
                                             host, m_spinCtrlPort->GetValue());
                        }
                }
                if (host.empty()) {
                        return;
                }
        }

        CallAfter([this]
        {
                m_auto_connecting = true;
                wxCommandEvent evt;
                add_exported_devices(evt);
                m_auto_connecting = false;

                /*
                 * 枚举完成后,把 Auto 列已勾选的设备真正挂载起来。
                 *
                 * add_exported_devices() 只做 OPCODE_LIST(拉列表),不会 attach;
                 * 驱动层的 PersistentDevices 也只在驱动重新加载时才自动挂载。
                 * 两者之间原先没有衔接,所以"勾了自动连接"的设备重启客户端后
                 * 一直停在"未连接"。这里补上这一步。
                 */
                attach_persistent_devices(m_comboBoxServer->GetValue());

                /*
                 * 唤醒心跳线程:让 "服务端管理页面" 在自动连接完成后的下一个循环里
                 * 立刻发心跳,而不是等满 10s 间隔。前提是 add_exported_devices
                 * 至少成功 OPCODE_LIST 过了一次(否则 m_mgmt_attached 仍然空,
                 * 踢一次也只是把空包早一点送出去 —— 没有副作用)。
                 *
                 * 不需要互斥锁:std::atomic_bool 的 store/exchange 是原子的,
                 * mgmt_loop 端 wait_or_stop 用 exchange(false) 抢一次就算消费,
                 * 多次 store(true) 与一次等价。
                 */
                m_mgmt_kick.store(true);
        });
}

void MainFrame::attach_persistent_devices(_In_ const wxString &url)
{
        if (url.empty()) {
                return;
        }

        auto persistent = get_persistent();
        if (persistent.empty()) {
                return;
        }

        /*
         * 先查驱动里**当前已经挂载**的设备,这些一律跳过。
         *
         * 为什么必须跳过:驱动层的 PersistentDevices 只有在驱动重新加载
         * (重装/重启)时才自动挂载,但**客户端重启**时驱动里的会话依然活着
         * (实测:杀掉客户端后 usbip port 仍显示 Port 01 占用)。此时如果再对
         * 同一台设备调一次 vhci::attach(),驱动会把这次重复挂载当作一次新的
         * 连接尝试,发出 connecting -> connected -> **disconnected** 一串事件。
         *
         * 后果非常隐蔽:设备在内核层其实一直是好的(usbip port 显示在用),
         * 但最后一条事件是 disconnected,于是
         *   - UI 那一行被刷成"未连接";
         *   - mgmt_note_state() 按"未连接"上报心跳,服务端管理页也看不到占用方。
         * 也就是用户看到的"实际连上了,但客户端和服务器都说没连"。
         *
         * 用 vhci::open() 而不是 get_vhci(): attach() 可能正在另一个线程用
         * 共享句柄,同句柄的查询会被阻塞(见 on_device_state() 上方的注释)。
         */
        std::set<std::string> attached_keys;

        if (auto imported = vhci::get_imported_devices(vhci::open().get())) {
                for (auto &d: *imported) {
                        attached_keys.insert(device_key(d.location.hostname,
                                                        d.location.service,
                                                        d.location.busid));
                }
        }

        /*
         * 只处理属于当前服务器的记录。
         *
         * make_server_url() 的规范化结果与 persistent_device.location 里
         * 存的字符串可能不完全一致(用户手动输入的 ServerAddr 大小写、尾部
         * 空格等),这里统一走 make_server_url 归一后再比,避免因为字符串
         * 细节不同而漏掉本该挂载的设备。
         */
        auto target = url;
        if (!target.Contains(L':')) {
                target += wxString::Format(L":%d", m_spinCtrlPort->GetValue());
        }
        auto target_url = wxString(target).Lower();

        int attached = 0;
        int skipped = 0;

        for (auto &pd: persistent) {
                auto host = wxString::FromUTF8(pd.location.hostname);
                auto service = wxString::FromUTF8(pd.location.service);
                if (host.empty() || service.empty()) {
                        continue;
                }

                auto dev_url = wxString::Format(L"%s:%s", host, service).Lower();
                if (dev_url != target_url) {
                        continue;
                }

                auto busid = wxString::FromUTF8(pd.location.busid);
                if (busid.empty()) {
                        continue;
                }

                /*
                 * 驱动里已经挂着的设备:跳过,千万不要再 attach 一次。
                 * 重复 attach 会让驱动发出 disconnected,把 UI 和服务端
                 * 都刷成"未连接"(原因见本函数开头的大段注释)。
                 */
                if (auto key = device_key(pd.location.hostname, pd.location.service,
                                          pd.location.busid);
                    attached_keys.contains(key)) {
                        ++skipped;
                        continue;
                }

                /*
                 * Auto 设备 = 持续连接(once=false),允许驱动在断线后自愈。
                 * 与用户双击/菜单「连接」的语义一致(见 attach(bool) 里
                 * dev_once = once || !is_persistent(dev))。
                 */
                if (auto err = attach(wxString::Format(L"%s:%s", host, service), busid,
                                      wxString::FromUTF8(pd.serial),
                                      to_string(pd.recv_mode), false)) {
                        if (err != ERROR_OPERATION_ABORTED && err != ERROR_CANCELLED) {
                                wxLogVerbose(L"自动连接(Auto)设备 %s/%s 挂载失败\n错误 %lu\n%s",
                                             dev_url, busid, err, GetLastErrorMsg(err));
                        }
                        continue;
                }

                ++attached;
        }

        if (attached > 0) {
                set_status_text(wxString::Format(L"已自动连接 %d 个设备", attached));
        } else if (skipped > 0) {
                // 设备本来就在挂载中(驱动持久化会话仍活着):这属于正常情况,
                // 不再重复挂载,也不弹任何提示,只是记一条日志备查。
                wxLogVerbose(L"%d 个自动连接设备已在挂载状态,跳过重复挂载", skipped);
        }
}

void MainFrame::post_refresh()
{
        wxCommandEvent evt(wxEVT_COMMAND_MENU_SELECTED, wxID_REFRESH);
        wxASSERT(m_menu_devices->FindItem(wxID_REFRESH)); // command belongs to this menu
        wxPostEvent(m_menu_devices, evt);
}

void MainFrame::post_exit()
{
        wxCommandEvent evt(wxEVT_COMMAND_MENU_SELECTED, wxID_EXIT);
        wxPostEvent(m_menu_file, evt);
}

/*
 * 见 wusbip.h 的声明:退出路径上的兜底写盘。
 *
 * 为什么不依赖控件事件就算完:控件事件(wxEVT_TEXT / wxEVT_SPINCTRL)在
 * "键入了但没回车、没失焦" 时可能不触发;而勾了「最小化到托盘 + 开机自启」
 * 的用户,常见操作就是改完端口直接重启电脑。这里在真正退出的路径上再写一次,
 * 无条件覆盖 —— 与事件驱动写盘互为补充,不冲突(值相同,写盘结果一致)。
 *
 * 覆盖范围(别指望它兜住所有情况):
 *   - 覆盖:菜单「退出」(on_exit → Close(true));以及重启/注销时
 *     wxEVT_END_SESSION → App::on_end_session() → Close(true)。
 *     这两种情况 CanVeto() 为假,会走到本函数。
 *   - 不覆盖:未勾选「关闭时直接退出」时点 × —— on_close() 在 Veto() +
 *     收进托盘之后就 return 了,走不到这里。该路径上的持久化完全依赖
 *     wxEVT_TEXT / wxEVT_SPINCTRL 两个控件事件。
 *   - 不覆盖:任务管理器结束进程、断电等非正常退出。
 */
void MainFrame::save_server_endpoint()
{
        auto cfg = wxConfig::Get();
        if (!cfg || !m_comboBoxServer || !m_spinCtrlPort) {
                return;
        }

        wxLogNull no_log; // 写配置失败不要弹日志框

        auto addr = m_comboBoxServer->GetValue();
        addr.Trim(true).Trim(false);
        if (!addr.empty()) {
                cfg->Write(L"ServerAddr", addr);
        }

        const int port = m_spinCtrlPort->GetValue();
        if (port >= 1025 && port <= 65535) {
                cfg->Write(L"ServerPort", port);
        }

        cfg->Flush();
}

void MainFrame::on_close(wxCloseEvent &event)
{
        wxLogVerbose(wxString::FromAscii(__func__));
        wxASSERT(event.GetEventType() == wxEVT_CLOSE_WINDOW);

        // 未勾选「关闭时直接退出」:点 × 收进系统托盘后台常驻(角标保留)
        if (!m_close_to_tray && event.CanVeto()) {
                event.Veto();
                iconize_to_tray();
                return;
        }

        m_window_fix_timer.Stop();
        m_taskbar_watchdog_timer.Stop();

        /*
         * 退出兜底:把当前服务器地址/端口无条件写一次配置。
         *
         * 能走到这里就说明是"真的要退出"(菜单退出,或重启/注销时的
         * on_end_session → Close(true))。未勾选「关闭时直接退出」时点 × 会在
         * 上面的 Veto() + iconize_to_tray() 处直接 return,不会执行到这里 ——
         * 那条路径的持久化靠 wxEVT_TEXT / wxEVT_SPINCTRL 兜住。
         */
        save_server_endpoint();

        /*
         * 主动告别:正常退出路径下,在断 read_loop 之前先告诉服务端"我走了"。
         * 否则服务端只能等 watchdog(KICK_IDLE_SECONDS=60s)发现心跳停,才会
         * unbind 设备 —— 这 60s 内管理页面会看到"设备被占用"但其实客户端已经退了,
         * 其他客户端也无法绑定到该设备。
         *
         * mgmt_send_goodbye() 同步阻塞,最多等 2s(http_exchange 内部 connect 超时),
         * 超时直接放弃 —— watchdog 会兜底。
         *
         * 必须在 break_read_loop() 之前:此时 m_mgmt_attached 还反映"本机实际
         * 还在用的设备",busid 清单是准确的。break_read_loop 之后 vhci 驱动
         * 会清 attach 会话,read_loop 退出,busid 就丢了。
         */
        mgmt_send_goodbye();

        break_read_loop();
        m_read_thread.join();
        mgmt_stop();

        event.Skip();
}

void MainFrame::on_iconize(wxIconizeEvent &event)
{
        wxLogVerbose(wxString::FromAscii(__func__));

        if (event.IsIconized()) {
                // 最小化 = 收进系统托盘(任务栏不再保留窗口)
                iconize_to_tray();
        }
}

void MainFrame::on_exit(wxCommandEvent&)
{
        Close(true);
}

/*
 * 收进托盘:隐藏主窗口与日志窗口,只保留通知区的常驻图标。
 *
 * 这里有一个必须处理、又很容易踩坑的点 —— 最小化标志 WS_MINIMIZE。
 *
 * 用户点标题栏"最小化"时,窗口先被 Windows 置为最小化状态,我们才收到
 * wxEVT_ICONIZE。此时若只调 wxWindow::Hide(),窗口虽然看不见了,但
 * WS_MINIMIZE 仍然留着:Shell 依旧认为它"是任务栏上的一个窗口",会在
 * 资源管理器刷新、激活、快捷键切换等时机把它还原回桌面 —— 这正是长期
 * 困扰用户的"收进托盘后又自己蹦出来"。
 *
 * 几条常走的路都不能要:
 *  - SetWindowPos + SWP_HIDEWINDOW:等价于 SW_HIDE,而 SW_HIDE 按文档只清
 *    WS_VISIBLE,不碰 WS_MINIMIZE(已实测:ws_minimize 仍为真)。
 *  - 直接 SetWindowLongW(GWL_STYLE, ... & ~WS_MINIMIZE):能清位,但绕过了
 *    wx 的状态机,下一次 Show() 会与内部状态不一致。
 *  - wxWindow::Iconize(false) 单独使用:底层走 ShowWindow(SW_RESTORE),
 *    会把窗口先显示一帧。若此时再有别的代码拦截窗口消息,就会残留成
 *    "闪一下"甚至"蹦出来"。
 *
 * 这里改为直接组合两个 Win32 调用:
 *   ShowWindow(SW_RESTORE)  -> 清掉 WS_MINIMIZE 和任务栏按钮
 *   ShowWindow(SW_HIDE)     -> 立刻隐藏
 * 两者在同一个消息处理内连续执行、中间不返回消息循环,Windows 不会插入
 * 重绘,因此不会看到闪烁;而 wx 的状态由随后的 Hide() 完成同步。
 *
 * 之前还叠加过"摘任务栏条目(WS_EX_TOOLWINDOW)+ 拦截 WM_SHOWWINDOW +
 * 看门狗定时强制收回"等做法,那些都会和 wx 内部的状态机打架,已全部移除。
 */
void MainFrame::iconize_to_tray()
{
        auto &log = *m_log->GetFrame();

        ensure_taskbar_icon(); // 常驻图标:已存在则复用,已安装则跳过

        if (auto hwnd = static_cast<HWND>(GetHandle())) {
                if (::IsIconic(hwnd)) {
                        ::ShowWindow(hwnd, SW_RESTORE); // 清 WS_MINIMIZE(任务栏按钮随之消失)
                }
                ::ShowWindow(hwnd, SW_HIDE);
        }

        log.Hide();
        Hide();
}

/*
 * 托盘还原的唯一入口:先从最小化恢复,再显示并置前。
 */
void MainFrame::restore_from_tray()
{
        if (IsIconized()) {
                Iconize(false);
        }

        Show();
        ensure_visible_window();
        fit_visible_columns();
        Raise();
}

void MainFrame::ensure_taskbar_icon()
{
        if (!m_taskbar_icon) {
                m_taskbar_icon = std::make_unique<TaskBarIcon>();
        }

        if (m_taskbar_icon->IsIconInstalled()) {
                /*
                 * 图标已经在通知区。什么都不做 —— 用户的"图标位置选择"
                 * (主栏/折叠区)由 Shell 自己维护,我们既不该 promote,也不该
                 * RemoveIcon+SetIcon 重装;任何改动都会让 Shell 重新按当前
                 * IsPromoted 摆放,从而覆盖用户拖动操作。
                 *
                 * Explorer 崩溃重启后 wxTaskBarIcon 的 IsIconInstalled() 会
                 * 自然变 false,会走到下面的 NIM_ADD 分支,届时 Shell 会按
                 * 当前 IsPromoted (即用户上次拖到的位置) 放置图标。
                 */
                return;
        }

        // 不直接复用窗口当前的 wxIcon:某些 Windows 主题/缩放组合下,窗口图标
        // 只有大尺寸资源,Shell 通知区会拒绝空的小图标。明确从资源中的 16/32
        // 位图组重新加载,失败才回退到窗口图标。
        auto load_icon = [this]() -> bool {
                wxIcon tray_icon(L"USBip");
                if (!tray_icon.IsOk()) {
                        tray_icon = GetIcon();
                }
                if (!tray_icon.IsOk()) {
                        wxLogError(L"无法加载托盘图标资源 USBip");
                        return false;
                }
                if (!m_taskbar_icon->SetIcon(tray_icon, GetTitle())) {
                        wxLogError(L"无法设置托盘图标");
                        return false;
                }
                return true;
        };

        if (!load_icon()) {
                return;
        }

        /*
         * 首次 NIM_ADD 之后,Shell 会读 IsPromoted 决定初始位置。
         * 此时若注册表里 IsPromoted 还不存在(典型情况:首次安装首次启动),
         * 帮它写成 1,让图标出现在主通知栏。如果已经是 0 (用户拖到隐藏区后
         * Explorer 重启了),则尊重现状,绝不动注册表。
         *
         * 注意:不要做 RemoveIcon + SetIcon 重加操作 —— 那会改变图标的安装
         * 时间戳并触发 Shell 重新按 IsPromoted 摆放,把用户拖到隐藏区的图标
         * 弹回主栏。
         */
        promote_taskbar_icon();
}

void MainFrame::on_taskbar_watchdog(wxTimerEvent&)
{
        /*
         * wxTaskBarIcon 的 IsIconInstalled() 会在 Explorer 重启后变为 false,
         * 重新 SetIcon 即可恢复通知区域图标,不影响窗口或设备连接。
         *
         * 注意这里只做图标自愈,不再顺带"强制收回窗口":窗口的显示/隐藏完全
         * 交给 wx 的 Show()/Hide() 处理,额外插手反而会打架。
         */
        ensure_taskbar_icon();
}

void MainFrame::on_local_name(wxCommandEvent&)
{
        wxString cur;
        if (auto c = wxConfig::Get()) {
                cur = c->Read(L"ClientName", wxString());
        }
        if (cur.empty()) {
                cur = wxGetHostName();
        }

        auto name = wxGetTextFromUser(
                L"该名称将作为“这台电脑”的名字,显示在服务器网页的当前连接方/占用方中。\n"
                L"(设备本身的名称与备注请在服务器网页的设备列表里修改。)",
                L"本机名称(这台电脑的备注)", cur, this);

        if (name.empty()) {
                return;
        }

        name.Trim(true).Trim(false);
        if (name.empty()) {
                return;
        }

        if (name.length() > 64) {
                name = name.Left(64);
        }

        if (auto c = wxConfig::Get()) {
                c->Write(L"ClientName", name);
                c->Flush();
        }

        mgmt_capture_settings(); // 立即以新名称登记心跳

        wxLogStatus(L"本机名称已设置为:%s", name);
}

void MainFrame::read_loop()
{
        auto on_exit = [] (auto frame)
        {
                std::lock_guard<std::mutex> lock(frame->m_read_close_mtx);
                frame->m_read.close();
        };

        std::unique_ptr<MainFrame, decltype(on_exit)> ptr(this, on_exit);

        while (auto state = vhci::read_device_state(m_read.get())) {
                auto evt = new DeviceStateEvent(std::move(*state));
                QueueEvent(evt); // see on_device_state()
        }

        if (auto err = GetLastError(); err != ERROR_OPERATION_ABORTED) { // see CancelSynchronousIo
                wxLogError(L"读取设备状态失败 %lu\n%s", err, GetLastErrorMsg(err));
        }
}

void MainFrame::break_read_loop()
{
        auto cancel_read = [this] // CancelSynchronousIo hangs if thread was terminated
        {
                std::lock_guard<std::mutex> lock(m_read_close_mtx);
                return !m_read || CancelSynchronousIo(m_read_thread.native_handle());
        };

        for (int i = 0; i < 300 && !cancel_read(); ++i, std::this_thread::sleep_for(std::chrono::milliseconds(100))) {
                if (auto err = GetLastError(); err != ERROR_NOT_FOUND) { // could not find a request to cancel
                        wxLogError(L"取消失败 %lu\n%s", err, wxSysErrorMsg(err));
                        break; // wxLogSysError does not compile if wxNO_IMPLICIT_WXSTRING_ENCODING is set
                }
        }
}

/*
 * GUI thread!
 * vhci::open() is used instead of get_vhci() becase of GUI thread locking:
 * 1. vhci:attach() is started in the separate thread.
 * 2. state::connecting is issued by the driver and on_device_state() is called in GUI thread.
 * 3. vhci::get_persistent() is called with the same HANDLE as vhci:attach().
 * 4. get_persistent() will be blocked as well as GUI thread until attach() is completed.
 * 5.The blocking will not happen if use different handles.
 */
void MainFrame::on_device_state(_In_ DeviceStateEvent &event)
{
        auto &tree = *m_treeListCtrl;

        auto &st = event.get();
        log(st);

        mgmt_note_state(st);

        if (m_taskbar_icon && m_taskbar_icon->IsIconInstalled()) {
                auto s = to_string(st.state) + L' ' + make_device_url(st.device.location);
                m_taskbar_icon->show_balloon(s);
        }

        auto [dev, added] = find_or_add_device(st.device.location);

        if (added) {
                auto server = tree.GetItemParent(dev);
                auto &url = tree.GetItemText(server);
                auto &busid = tree.GetItemText(dev);
                wxLogVerbose(L"已添加 %s/%s", url, busid);
        }

        if (st.state == state::disconnected && is_empty(tree, dev)) { // connection has failed/closed
                wxLogVerbose(L"临时设备已移除");
                remove_device(dev);
                return;
        }

        if (auto &cur_id = tree.GetItemText(dev, COL_SOURCE_ID); cur_id.empty() || st.state == state::plugged) {
                // update state and source_id
        } else if (auto id = to_wxstring(st.source_id); id != cur_id) {
                wxLogVerbose(L"忽略来自 %s 的事件(%s != %s)", id, cur_id);
                return;
        }

        if (st.state == state::disconnected || st.state == state::unplugged) { // could be the last event for the source
                st.source_id = 0; // clear COL_SOURCE_ID
        }

        auto [dc, flags] = make_device_columns(st);
        wxASSERT(flags);

        // 状态列做 union:本机驱动层状态 + 服务端 meta 的远端语义叠加。
        // 见 utils.h 的 to_view_state 注释。本机视角的真相层 plugged/connected
        // 不会被远端覆盖(避免自我回环),其余 unplugged/unplugging/connecting
        // 都按 meta 二次细分,消除"未连接"的歧义。
        {
                std::lock_guard<std::mutex> lock(m_mgmt_mtx);
                std::string self_id = m_mgmt_cfg.client_id;

                const mgmt::meta_device *meta_ptr = nullptr;
                mgmt::meta_device meta_snapshot; // 锁外读,m_mgmt_meta 后续可能被改不要紧
                if (auto it = m_mgmt_meta.find(st.device.location.busid); it != m_mgmt_meta.end()) {
                        meta_snapshot = it->second;
                        meta_ptr = &meta_snapshot;
                }

                /*
                 * 用"本机状态模型"而不是事件本身来判定这一行是否已连接:
                 * mgmt_note_state() 刚刚按驱动列表兜底过 —— 一次**失败**的 attach
                 * 尝试也会以 state::disconnected 收尾,照抄事件就会把明明还挂着的
                 * 设备画成"空闲/未连接"(见 mgmt_sync_attached() 的注释)。
                 *
                 * 其余瞬时状态(connecting/unplugging)仍按事件原样显示,否则
                 * "挂载中"会被抹成"空闲"。
                 */
                auto local = st.state;
                if (local == state::disconnected) {
                        if (auto it = m_mgmt_attached.find(st.device.location.busid);
                            it != m_mgmt_attached.end() && it->second) {
                                local = state::plugged;
                        }
                }

                dc[COL_STATE] = to_view_state(local, meta_ptr, self_id, meta_ptr ? meta_ptr->attached_count : 0);
        }

        if (auto &port = dc[COL_PORT]; !port.empty() && is_port_residual(st.state)) {
                port.clear();
                flags |= mkflag(COL_PORT);
        }

        if (added) { // COL_SERIAL, COL_RECEIVE_MODE are actual
                auto saved = as_set(get_saved());
                auto persistent = get_persistent(vhci::open());
                constexpr auto cols = columns::saved_read_only | columns::saved_notes | columns::pers_persistent;
                flags = update_from_registry(dc, flags, cols, saved, persistent);
        }

        if (!added) {
                log(tree, dev, L"之前");
        }

        update_device(dev, dc, flags);
        log(tree, dev, L"之后");
}

void MainFrame::on_has_devices_update_ui(wxUpdateUIEvent &event)
{
        auto v = get_devices(*m_treeListCtrl);
        event.Enable(!v.empty());
}

void MainFrame::on_has_selected_devices_update_ui(wxUpdateUIEvent &event)
{
        auto v = get_selected_devices(*m_treeListCtrl);
        event.Enable(!v.empty());
}

void MainFrame::on_has_any_selected_devices_update_ui(wxUpdateUIEvent &event)
{
        auto v = get_selected_devices(*m_treeListCtrl, is_server); // OK if is_empty()
        event.Enable(!v.empty());
}

void MainFrame::on_copy_rows(wxCommandEvent&)
{
        wxString rows;

        for (auto &tree = *m_treeListCtrl;
             auto &dev: get_selected_devices(tree, is_server)) {
                rows += to_string(tree, dev) + L'\n';
        }

        wxLogVerbose(rows);

        if (wxClipboardLocker lck; !lck) {
                wxLogError(L"无法锁定剪贴板");
        } else if (auto data = std::make_unique<wxTextDataObject>(rows); wxTheClipboard->SetData(data.get())) {
                data.release();
        } else {
                wxLogError(L"无法将数据写入剪贴板");
        }
}

void MainFrame::on_select_all(wxCommandEvent&)
{
        m_treeListCtrl->SelectAll();
}

wxTreeListItem MainFrame::get_edit_device()
{
        auto &tree = *m_treeListCtrl;
        wxTreeListItem item;

        if (auto v = get_selected_devices(tree); v.size() == 1) {
                item = v.front();
        }

        return item;
}

void MainFrame::on_edit_device_update_ui(wxUpdateUIEvent &event)
{
        auto item = get_edit_device();
        event.Enable(item.IsOk());
}

void MainFrame::edit_column_dlg(
        _In_ const wxString &title, _In_ column_pos_t col, _In_ int maxlen,
        _In_ const std::function<wxString (const wxString&)> &validator)
{
        auto dev = get_edit_device();
        if (!dev.IsOk()) {
                return;
        }

        auto &tree = *m_treeListCtrl;
        auto server = tree.GetItemParent(dev);

        auto &url = tree.GetItemText(server);
        auto &busid = tree.GetItemText(dev);
        auto caption = wxString::Format(L"%s - %s/%s", title, url, busid);

        auto &vendor = tree.GetItemText(dev, COL_VENDOR);
        auto &product = tree.GetItemText(dev, COL_PRODUCT);
        auto message = wxString::Format(L"%s\n%s", vendor, product);

        for (auto value = tree.GetItemText(dev, col); ;) {

                wxTextEntryDialog dlg(this, message, caption, value, wxTextEntryDialogStyle);
                dlg.SetMaxLength(maxlen);

                if (dlg.ShowModal() != wxID_OK) {
                        break;
                }

                value = dlg.GetValue();

                if (!validator) {
                        //
                } else if (auto err = validator(value); !err.empty()) {
                        wxMessageDialog(this, err, title, wxICON_ERROR).ShowModal();
                        continue;
                }

                tree.SetItemText(dev, col, value);
                break;
        }
}

void MainFrame::on_edit_notes(wxCommandEvent&)
{
        enum { MAXLEN = 256 };
        edit_column_dlg(L"备注", COL_NOTES, MAXLEN, nullptr);
}

void MainFrame::on_edit_serial(wxCommandEvent&)
{
        auto validator = [] (const wxString &serial)
        {
                wxString err;
                if (auto s = serial.utf8_string(); !validate_device_serial(s)) {
                        err = GetLastErrorMsg();
                }
                return err;
        };

        auto maxlen = get_device_serial_maxlen();
        edit_column_dlg(L"序列号", COL_SERIAL, maxlen, validator);
}

void MainFrame::on_edit_gen_serial(wxCommandEvent&)
{
        if (auto dev = get_edit_device(); dev.IsOk()) {
                auto s = generate_device_serial();
                auto ws = wxString::FromAscii(s.data(), s.size());
                m_treeListCtrl->SetItemText(dev, COL_SERIAL, ws);
        }
}

bool MainFrame::is_checked(_In_ wxTreeListItem device, _In_ column_pos_t col)
{
        auto &tree = *m_treeListCtrl;
   
        wxASSERT(tree.GetItemParent(device).IsOk()); // server
        wxASSERT(!tree.GetFirstChild(device).IsOk());

        auto &s = tree.GetItemText(device, col);
        return !s.empty();
}

void MainFrame::set_checked(_In_ wxTreeListItem device, _In_ column_pos_t col, _In_ bool checked)
{
        auto &tree = *m_treeListCtrl;

        wxASSERT(tree.GetItemParent(device).IsOk()); // server
        wxASSERT(!tree.GetFirstChild(device).IsOk());

        wxString val;
        if (checked) {
                val = g_persistent_mark; // CHECK MARK, 2714 HEAVY CHECK MARK
        }

        tree.SetItemText(device, col, val);
}

void MainFrame::on_log_show_update_ui(wxUpdateUIEvent &event)
{
        auto f = m_log->GetFrame();
        event.Check(f->IsShown());
}

void MainFrame::on_log_show(wxCommandEvent &event)
{
        bool checked = event.GetInt();
        m_log->Show(checked);
}

void MainFrame::on_view_zebra_update_ui(wxUpdateUIEvent &event)
{
        auto &dv = *m_treeListCtrl->GetDataView();
        auto check = dv.HasFlag(wxDV_ROW_LINES);
        event.Check(check);
}

void MainFrame::on_view_zebra(wxCommandEvent&)
{
        auto &dv = *m_treeListCtrl->GetDataView();
        dv.ToggleWindowStyle(wxDV_ROW_LINES);
        dv.Refresh(false);
}

void MainFrame::on_log_verbose_update_ui(wxUpdateUIEvent &event)
{
        auto verbose = m_log->GetLogLevel() == VERBOSE_LOGLEVEL;
        event.Check(verbose);
}

void MainFrame::on_log_verbose(wxCommandEvent &event)
{
        bool checked = event.GetInt();
        m_log->SetLogLevel(checked ? VERBOSE_LOGLEVEL : DEFAULT_LOGLEVEL);
}

void MainFrame::on_log_library_update_ui(wxUpdateUIEvent &event)
{
        auto verbose = m_log->GetLogLevel() == VERBOSE_LOGLEVEL;

        if (!verbose && is_library_log_enabled()) {
                enable_library_log(false);
        }

        event.Check(is_library_log_enabled());
        event.Enable(verbose);
}

void MainFrame::on_log_library(wxCommandEvent &event)
{
        bool checked = event.GetInt();
        enable_library_log(checked);
}

void MainFrame::on_start_in_tray_update_ui(wxUpdateUIEvent &event)
{
        event.Check(m_start_in_tray);
}

void MainFrame::on_start_in_tray(wxCommandEvent &event)
{
        bool checked = event.GetInt();
        m_start_in_tray = checked;
}

void MainFrame::on_close_to_tray_update_ui(wxUpdateUIEvent &event)
{
        event.Check(m_close_to_tray);
}

void MainFrame::on_close_to_tray(wxCommandEvent &event)
{
        bool checked = event.GetInt();
        m_close_to_tray = checked;
}

DWORD MainFrame::attach(
        _In_ const wxString &url, _In_ const wxString &busid,
        _In_ const wxString &serial, _In_ const wxString &recv_mode, _In_ bool once)
{
        /*
         * 连接服务器前先过「共享访问密码」闸门。
         *
         * 放在这个函数里(而不是各个调用点)是因为它是所有 attach 的唯一出口:
         * 用户点「连接」、Auto 设备自动挂载、排队唤醒后的自动接手,全都走这里。
         * 服务端没启用访问控制时 mgmt_ensure_access 直接返回 true,这条路径与
         * 改造前逐字等价。
         *
         * 返回 ERROR_CANCELLED 而不是 ERROR_OPERATION_ABORTED:语义上这是"用户
         * 放弃提供访问密码",不是"用户中断了正在进行的连接"。各调用点把它和
         * ERROR_OPERATION_ABORTED 一样当作"用户主动中止",不弹错误框。
         */
        if (!mgmt_ensure_access(url)) {
                set_status_text(wxString::Format(L"未连接 %s:未通过服务器访问密码验证", url));
                return ERROR_CANCELLED;
        }

        wxString hostname;
        wxString service;

        if (!split_server_url(url, hostname, service)) {
                return ERROR_INVALID_PARAMETER;
        }

        /*
         * 驱动层已经挂着这台设备:绝不能再发一次 attach。
         *
         * 重复 attach 会被服务端回 ST_DEV_BUSY("设备忙"),而驱动为这次失败的
         * 尝试发出的最后一条事件是 state::disconnected —— 它会把本机状态刷成
         * "未连接",尽管原来那条会话毫发无损。用户看到的就是"明明连着,界面
         * 说没连,再点连接还报设备忙",而且不会自愈。详见 mgmt_sync_attached()。
         *
         * 这里直接按"已连接"处理:用驱动列表校正本机状态、踢一次心跳让界面与
         * 服务端立刻一致,不发 IOCTL、也不弹错误框 —— 用户点「连接」得到的正是
         * "它已经连上了"。
         */
        if (mgmt_sync_attached(hostname.utf8_string(), busid.utf8_string())) {
                m_mgmt_kick.store(true);
                set_status_text(wxString::Format(L"%s/%s 已在本机连接", url, busid));
                return ERROR_SUCCESS;
        }

        vhci::attach_args args {
                .location {
                        .hostname = hostname.utf8_string(),
                        .service = service.utf8_string(),
                        .busid = busid.utf8_string(),
                },
                .serial = serial.utf8_string(),
                .recv_mode = to_receive_mode(recv_mode),
                .once = once
        };

        DWORD err{};
        auto f = [&err, args = std::move(args), vhci = get_vhci().get()]
        { 
                auto port = vhci::attach(vhci, args); 
                err = port > 0 ? ERROR_SUCCESS : GetLastError();
        };

        auto msg = wxString::Format(L"%s/%s", url, busid);
        run_cancellable(this, msg, L"正在连接…", std::move(f));

        return err;
}

void MainFrame::on_attach_once(wxCommandEvent&)
{
        attach(true);
}

bool MainFrame::is_persistent(_In_ wxTreeListItem device)
{
        auto &tree = *m_treeListCtrl;

        wxASSERT(tree.GetItemParent(device).IsOk()); // server
        wxASSERT(!tree.GetFirstChild(device).IsOk());

        auto &s = tree.GetItemText(device, COL_PERSISTENT);
        return !s.empty();
}

void MainFrame::attach(_In_ bool once)
{
        wxLogVerbose(L"%s, once %d", wxString::FromAscii(__func__), once);

        for (auto &tree = *m_treeListCtrl; auto &dev: get_selected_devices(tree, is_server)) {

                auto server = tree.GetItemParent(dev);
                auto &url = tree.GetItemText(server);
                auto &busid = tree.GetItemText(dev);
                auto &serial = tree.GetItemText(dev, COL_SERIAL);
                auto &recv_mode = tree.GetItemText(dev, COL_RECEIVE_MODE);

                // 连接语义与 Auto 列一致:
                //   - Auto(勾选/持久)设备 → 持续连接(断线自动自愈)
                //   - 普通设备 → 连接一次(双击/「连接」=连一次,断开后停在未连接,
                //     不做后台偷偷重连;需要自愈就在该行勾 Auto)
                auto dev_once = once || !is_persistent(dev);

                /*
                 * 设备正被别的客户端占用:不再弹一个必然失败的"设备忙"错误,
                 * 而是登记到服务端的排队队列。当前占用方一释放,服务端会通过
                 * 心跳回发 attach 通知,由 mgmt_handle_queue_notices() 自动接手。
                 *
                 * 只有在管理接口真的可用时才有排队可言(mgmt_queue_device 返回
                 * false 说明没启用/没配服务器),此时回退到老的直连行为。
                 */
                auto busid_u8 = busid.utf8_string();
                if (mgmt_device_busy(busid_u8) && mgmt_queue_device(busid_u8, true)) {
                        set_status_text(wxString::Format(
                                L"%s 正被其他客户端使用，已加入排队，等对方释放后自动连接", busid));
                        continue;
                }

                if (auto err = attach(url, busid, serial, recv_mode, dev_once)) {
                        if (err != ERROR_OPERATION_ABORTED && err != ERROR_CANCELLED) {
                                wxLogError(L"无法连接 %s/%s\n错误 %lu\n%s", 
                                              url, busid, err, GetLastErrorMsg(err));
                        }
                        break;
                }

                // 连接成功,排队位已经没有意义,主动退出队列(服务端也会凭
                // 心跳里的 busids 自动摘掉,这里只是让本机状态立刻一致)。
                mgmt_queue_device(busid_u8, false);
        }
}

void MainFrame::on_attach_stop(wxCommandEvent&)
{
        auto &vhci = get_vhci();
        int total = 0;

        for (auto &tree = *m_treeListCtrl; auto &dev: get_selected_devices(tree, is_server)) {

                auto server = tree.GetItemParent(dev);
                auto pd = make_persistent_device(tree, server, dev);

                if (auto cnt = vhci::stop_attach_attempts(vhci.get(), &pd.location); cnt >= 0) {
                        total += cnt;
                        continue;
                }

                auto err = GetLastError();

                auto &url = tree.GetItemText(server);
                auto &busid = tree.GetItemText(dev);

                wxLogError(L"无法停止对 %s/%s 的连接尝试\n错误 %lu\n%s",
                              url, busid, err, GetLastErrorMsg(err));

                break;
        }

        set_status_text(wxString::Format(L"已停止 %d 个连接请求", total));
}

void MainFrame::on_attach_stop_all(wxCommandEvent&)
{
        auto &vhci = get_vhci();

        if (auto cnt = vhci::stop_attach_attempts(vhci.get(), nullptr); cnt < 0) {
                auto err = GetLastError();
                wxLogError(L"无法停止连接尝试\n错误 %lu\n%s", err, GetLastErrorMsg(err));
        } else {
                set_status_text(wxString::Format(L"已停止 %d 个连接请求", cnt));
        }
}

DWORD MainFrame::detach(_In_ int port)
{
        wxLogVerbose(L"断开端口 %d", port);
        auto err = ERROR_SUCCESS;

        auto f = [&err, port]
        {
                if (auto &vhci = get_vhci(); !vhci::detach(vhci.get(), port)) {
                        err = GetLastError();
                }
        };

        auto msg = wxString::Format(L"端口 %d", port);
        run_cancellable(this, msg, L"正在断开…", std::move(f));

        return err;
}

void MainFrame::on_detach(wxCommandEvent&)
{
        auto &vhci = get_vhci();

        for (auto &tree = *m_treeListCtrl; auto &dev: get_selected_devices(tree)) {

                // 先停止该设备的自动重连"连接尝试",再断开端口,
                // 保证「断开」= 真正断开,不会又被驱动自动拉起。
                auto server = tree.GetItemParent(dev);
                auto pd = make_persistent_device(tree, server, dev);
                (void)vhci::stop_attach_attempts(vhci.get(), &pd.location);

                // 用户主动断开 = 不再想用这台设备,顺带退出排队。否则一旦本机
                // 排在队首,设备空出来的那一刻又会被自动连回来,和用户意图相反。
                mgmt_queue_device(pd.location.busid, false);

                if (auto port = get_port(dev); !port) {
                        // 未连接(可能仍在自动重试,上面已停止其尝试)
                } else if (auto err = detach(port)) {
                        if (err != ERROR_OPERATION_ABORTED) {
                                wxLogError(L"无法断开端口 %d\n错误 %lu\n%s",
                                              port, err, GetLastErrorMsg(err));
                        }
                        break;
                }
        }
}

void MainFrame::on_detach_all(wxCommandEvent&) 
{
        auto &vhci = get_vhci();

        // 先停止全部自动重连"连接尝试",再断开全部设备
        if (auto cnt = vhci::stop_attach_attempts(vhci.get(), nullptr); cnt < 0) {
                auto err = GetLastError();
                wxLogError(L"无法停止连接尝试\n错误 %lu\n%s", err, GetLastErrorMsg(err));
        }

        // 「全部断开」表示不再自动接手任何设备,清空本机的全部排队意愿。
        mgmt_clear_all_queues();

        if (auto err = detach(vhci::port_all); err && err != ERROR_OPERATION_ABORTED) {
                wxLogError(L"无法断开全部设备\n错误 %lu\n%s",
                              err, GetLastErrorMsg(err));
        }
}

/*
 * 从 wxConfig 的 /devices 保存组里删掉属于某个服务器的子键。
 *
 * 必须删:否则下次「加载」或重启时 on_load() 会按 /devices 把刚移除的
 * 服务器节点原样重建回来 —— 用户会觉得"删了又回来了"。
 *
 * 这里分两步(先收集组名,再删)而不是边遍历边删,避免在枚举过程中改动
 * 注册表布局。删完留下的编号空洞无害:get_saved() 按组名读取不依赖连续性,
 * 下次 save() 会重新编号。
 */
static void remove_saved_server(_In_ const wxString &url)
{
        auto &cfg = *wxConfig::Get();
        auto path = cfg.GetPath();

        cfg.SetPath(g_key_devices);

        std::vector<wxString> drop;
        wxString name;
        long idx;

        for (auto ok = cfg.GetFirstGroup(name, idx); ok; ok = cfg.GetNextGroup(name, idx)) {
                if (!cfg.HasGroup(name)) {
                        continue;
                }
                cfg.SetPath(name);

                wxString saved_url;
                wxLogNull no_log;
                cfg.Read(g_key_url, &saved_url);
                cfg.SetPath(L"..");

                if (saved_url == url) {
                        drop.emplace_back(name);
                }
        }

        for (auto &group: drop) {
                cfg.DeleteGroup(group);
        }

        cfg.Flush();
        cfg.SetPath(path);
}

/*
 * 移除选中的服务器节点(连同其下全部设备行)。
 *
 * 只影响本机客户端的列表,不改动服务端:
 *   - 服务器下已连接的设备先 detach,并停掉自动重连尝试,不留幽灵行;
 *   - 删掉 /devices 里属于该服务器的保存项,否则「加载」/重启会把它恢复;
 *   - 从输入框历史里删除该地址;若它正是当前地址,一并清空 ComboBox 与
 *     ServerAddr —— 否则下次「刷新」会按当前地址把它重新枚举回来;
 *   - 同步驱动的自动连接(Auto)列表,去掉属于该服务器的条目。
 */
void MainFrame::on_remove_server(wxCommandEvent&)
{
        auto &tree = *m_treeListCtrl;

        // 选中服务器节点本身,或以设备行选中 —— 都归到它所属的服务器节点
        wxTreeListItems selection;
        tree.GetSelections(selection);

        std::vector<wxTreeListItem> servers;

        for (auto &item: selection) {
                auto server = is_server(tree, item) ? item : tree.GetItemParent(item);

                if (!server.IsOk() || !is_server(tree, server)) {
                        continue;
                }
                if (std::find(servers.begin(), servers.end(), server) == servers.end()) {
                        servers.emplace_back(server);
                }
        }

        if (servers.empty()) {
                return;
        }

        wxString list;
        for (auto &server: servers) {
                if (!list.empty()) {
                        list += L'\n';
                }
                list += wxString::Format(L"    %s", tree.GetItemText(server));
        }

        wxMessageDialog dlg(this,
                wxString::Format(L"确定要从列表中移除以下服务器吗?\n\n%s\n\n"
                                 L"该服务器下的设备行会一并移除,已连接的设备会被断开。\n"
                                 L"服务器本身不受影响,之后仍可用「读取设备」重新添加。", list),
                L"移除服务器", wxOK | wxCANCEL | wxCANCEL_DEFAULT | wxICON_QUESTION | wxCENTRE);

        if (dlg.ShowModal() != wxID_OK) {
                return;
        }

        auto &vhci = get_vhci();
        auto persistent = get_persistent();
        bool persistent_changed = false;
        bool cleared_current = false; // 删掉的正是输入框当前地址

        for (auto &server: servers) {
                auto &url = tree.GetItemText(server);
                wxLogVerbose(L"%s: '%s'", wxString::FromAscii(__func__), url);

                // 1) 停掉该服务器下所有设备的自动重连尝试,并断开已连接的端口
                for (auto dev = tree.GetFirstChild(server); dev.IsOk(); dev = tree.GetNextSibling(dev)) {
                        auto pd = make_persistent_device(tree, server, dev);
                        (void)vhci::stop_attach_attempts(vhci.get(), &pd.location);

                        if (auto port = get_port(dev)) {
                                if (auto err = detach(port); err && err != ERROR_OPERATION_ABORTED) {
                                        wxLogError(L"无法断开 %s/%s 的端口 %d\n错误 %lu\n%s",
                                                      url, tree.GetItemText(dev), port, err, GetLastErrorMsg(err));
                                }
                        }

                        if (persistent.erase(pd)) {
                                persistent_changed = true;
                        }
                }

                // 2) 清 /devices 里属于该服务器的保存项
                remove_saved_server(url);

                // 3) 从输入框历史里删除;若是当前地址则留待下面处理,防止刷新时复活
                if (auto pos = m_comboBoxServer->FindString(url); pos != wxNOT_FOUND) {
                        m_comboBoxServer->Delete(pos);
                }

                if (m_comboBoxServer->GetValue() == url) {
                        m_comboBoxServer->SetValue(wxEmptyString);
                        cleared_current = true;
                }

                // 4) 删除树节点(连同子项)
                tree.DeleteItem(server);
        }

        /*
         * 删掉的若正是输入框当前地址,别留空:自动切到列表里仍在的另一个服务器
         * (通常会得到用户想要的"只剩一个"),并把 ServerAddr 同步写回。
         * 一个都不剩才清空地址 —— 同样是为了让「刷新」不会把刚删的服务器拉回来。
         */
        if (cleared_current) {
                if (auto first = tree.GetFirstItem(); first.IsOk() && is_server(tree, first)) {
                        auto address = tree.GetItemText(first);

                        m_comboBoxServer->SetValue(address);

                        if (auto cfg = wxConfig::Get()) {
                                cfg->Write(L"ServerAddr", address);
                                cfg->Flush();
                        }
                } else if (auto cfg = wxConfig::Get()) {
                        wxString addr;
                        if (cfg->Read(L"ServerAddr", &addr) && !addr.empty()) {
                                // 只清地址,保留端口供下次使用
                                cfg->DeleteEntry(L"ServerAddr", false);
                                cfg->Flush();
                        }
                }
        }

        // 5) 同步驱动的自动连接(Auto)列表
        if (persistent_changed) {
                auto v = std::vector<persistent_device>(persistent.begin(), persistent.end());

                if (!vhci::set_persistent(vhci.get(), v)) {
                        auto err = GetLastError();
                        wxLogError(L"无法更新自动连接(Auto)信息\n错误 %lu\n%s", err, GetLastErrorMsg(err));
                }
        }

        update_summary();
        set_status_text(wxString::Format(L"已移除 %zu 个服务器", servers.size()));
}

wxTreeListItem MainFrame::find_or_add_server(_In_ const wxString &url)
{
        auto &tree = *m_treeListCtrl;
        wxTreeListItem server;

        for (auto item = tree.GetFirstItem(); item.IsOk(); item = tree.GetNextSibling(item)) {
                if (tree.GetItemText(item) == url) {
                        wxASSERT(is_server(tree, item));
                        return server = item;
                }
        }

        return server = tree.AppendItem(tree.GetRootItem(), url, IMG_SERVER, IMG_SERVER);
}

std::pair<wxTreeListItem, bool> MainFrame::find_or_add_device(_In_ const wxString &url, _In_ const wxString &busid)
{
        std::pair<wxTreeListItem, bool> res;

        auto &tree = *m_treeListCtrl;
        auto server = find_or_add_server(url);

        for (auto item = tree.GetFirstChild(server); item.IsOk(); item = tree.GetNextSibling(item)) {
                if (tree.GetItemText(item) == busid) {
                        return res = std::make_pair(item, false);
                }
        }

        auto device = tree.AppendItem(server, busid, IMG_DEVICE, IMG_DEVICE);

        if (!tree.IsExpanded(server)) {
                tree.Expand(server);
        }

        return res = std::make_pair(device, true);
}

std::pair<wxTreeListItem, bool> MainFrame::find_or_add_device(_In_ const device_location &loc)
{
        auto url = make_server_url(loc);
        auto busid = wxString::FromUTF8(loc.busid);

        return find_or_add_device(url, busid);
}

std::pair<wxTreeListItem, bool> MainFrame::find_or_add_device(_In_ const device_columns &dc)
{
        auto &url = get_url(dc);
        return find_or_add_device(url, dc[COL_BUSID]);
}

void MainFrame::remove_device(_In_ wxTreeListItem device)
{
        wxASSERT(device.IsOk());
        auto &tree = *m_treeListCtrl;

        auto server = tree.GetItemParent(device);
        tree.DeleteItem(device);

        if (auto child = tree.GetFirstChild(server); !child.IsOk()) { // has no children
                tree.DeleteItem(server);
        }

        update_summary();
}

void MainFrame::update_device(_In_ wxTreeListItem device, _In_ const device_columns &dc, _In_ unsigned int flags)
{
        auto &tree = *m_treeListCtrl;

        wxASSERT(device.IsOk());
        wxASSERT(tree.GetItemText(device) == dc[COL_BUSID]);
        wxASSERT(tree.GetItemText(tree.GetItemParent(device)) == get_url(dc));

        if (!flags) {
                return;
        }
        
        for (int i = UPD_COL_FIRST; i <= UPD_COL_LAST; ++i) {
                auto col = static_cast<column_pos_t>(i);

                if (auto &new_val = dc[col]; (flags & mkflag(col)) && new_val != tree.GetItemText(device, col)) {
                        tree.SetItemText(device, col, new_val);
                }
        }

        update_summary();
}

void MainFrame::on_help_about(wxCommandEvent&)
{
        auto &v = win::get_file_version();
 
        wxAboutDialogInfo d;

        d.SetVersion(wx_string(v.GetProductVersion()));
        d.SetDescription(wx_string(v.GetFileDescription()));
        d.SetCopyright(wx_string(v.GetLegalCopyright()));

        d.AddDeveloper(L"Vadym Hrynchyshyn\t<vadimgrn@gmail.com>");
        d.SetWebSite(L"https://github.com/vadimgrn/usbip-win2", L"GitHub 项目主页");

        d.SetLicence(load_license());
        //d.SetIcon();

        wxAboutBox(d, this);
}

auto MainFrame::connect(
        _In_ const wxString &hostname, _In_ const wxString &service, 
        _In_ const std::string &hostname_u8, _In_ const std::string &service_u8)
{
        Socket sock;
        DWORD err{};

        auto f = [&sock, &err, host = hostname_u8.c_str(), svc = service_u8.c_str()]
        {
                sock = usbip::connect(host, svc, CANCEL_BY_APC);
                err = sock ? ERROR_SUCCESS : GetLastError();
        };

        auto msg = wxString::Format(L"%s:%s", hostname, service);
        run_cancellable(this, msg, L"正在连接…", std::move(f), cancel_connect);

        switch (err) {
        case ERROR_SUCCESS:
        case WSA_E_CANCELLED:
        case ERROR_CANCELLED:
                break;
        default:
                if (m_auto_connecting) {
                        wxLogStatus(L"无法连接到 %s:%s", hostname, service);
                } else {
                        wxLogError(L"无法连接到 %s:%s\n错误 %lu\n%s", hostname, service, err, GetLastErrorMsg(err));
                }
        }

        return sock;
}

void MainFrame::add_exported_devices(wxCommandEvent&)
{
        auto &cb = *m_comboBoxServer;

        auto host = cb.GetValue();
        if (host.empty()) {
                cb.SetFocus();
                return;
        }

        auto port = wxString::Format(L"%d", m_spinCtrlPort->GetValue());
        wxLogVerbose(L"%s, host='%s', port='%s'", wxString::FromAscii(__func__), host, port);

        mgmt_capture_settings();

        /*
         * 拉取设备列表前先过「共享访问密码」闸门:服务端启用了访问控制而本机
         * 还没授权时,先弹框要密码(或用保存的密码静默授权)。
         *
         * 必须放在 connect() 之前 —— 否则会先花一次 TCP 连接去枚举,失败后
         * 再让用户输密码,体验和错误提示都不对。
         * 服务器没启用访问控制时这里直接放行,行为与改造前完全一致。
         */
        if (!mgmt_ensure_access(wxString::Format(L"%s:%s", host, port))) {
                set_status_text(L"已取消:需要服务器访问密码");
                return;
        }

        auto u8_host = host.utf8_string();
        auto u8_port = port.utf8_string();

        auto sock = connect(host, port, u8_host, u8_port);
        if (!sock) {
                return;
        }

        auto persistent = get_persistent();
        auto saved = as_set(get_saved());

        auto dev = [this, host = std::move(u8_host), port = std::move(u8_port), &persistent, &saved] (auto, auto &device)
        {
                device_state state {
                        .device = make_imported_device(std::move(host), std::move(port), device),
                };
                auto [dc, flags] = make_device_columns(state);

                wxASSERT(state.device.serial.empty());
                wxASSERT(!(flags & mkflag(COL_SERIAL)));

                wxASSERT(state.device.recv_mode == receive_mode::zero_copy);
                wxASSERT(flags & mkflag(COL_RECEIVE_MODE));
                wxASSERT(dc[COL_RECEIVE_MODE] == to_string(receive_mode::zero_copy));

                auto [item, added] = find_or_add_device(dc); 

                if (added) {
                        constexpr auto cols = columns::saved_read_write | columns::pers_all;
                        flags = update_from_registry(dc, flags, cols, saved, persistent);
                } else {
                        flags &= ~mkflags({COL_STATE, COL_RECEIVE_MODE}); // clear
                }

                update_device(item, dc, flags);
        };

        auto intf = [] <typename... Args> (Args&&...) {};

        if (!enum_exportable_devices(sock.get(), dev, intf)) {
                auto err = GetLastError();
                if (m_auto_connecting) {
                        wxLogStatus(L"无法枚举 %s:%s 上的远程设备", host, port);
                } else {
                        wxLogError(L"枚举远程设备失败 %lu\n%s", err, GetLastErrorMsg(err));
                }
        } else if (cb.FindString(host) != wxNOT_FOUND) {
                // already exists
        } else if (auto pos = cb.Append(host); cb.GetCount() > 32) {
                cb.Delete(pos > 0 ? --pos : ++pos);
        }

        /*
         * 枚举完再补一次"管理接口才知道的设备"。
         *
         * USB/IP 的 DEVLIST 只反映服务端当前可导出的设备;一台正被别的客户端
         * 占用的设备在某些服务端实现里会从列表里消失,于是用户根本看不到它,
         * 也就没有入口去排队。管理接口的 /api/devices 始终会列出它(并带上
         * 占用方和等待名单),以它为补充来源就能保证"别人在用的设备也可见"。
         */
        add_mgmt_only_devices(host.utf8_string(), port.utf8_string());

        /*
         * 保底持久化:即便 wxEVT_TEXT/wxEVT_SPINCTRL 没触发(例如从外部命令行
         * 启动并直接进 add_exported_devices),这里也确保 host/port 被记住。
         * host 为空时不要覆盖已有值 —— 否则后续某次空 host 调用会把"上次成功
         * 连接的服务器地址"擦掉。
         */
        if (auto cfg = wxConfig::Get()) {
                if (!host.empty()) {
                        cfg->Write(L"ServerAddr", host);
                }
                cfg->Write(L"ServerPort", m_spinCtrlPort->GetValue());
                cfg->Flush();
        }
}

void MainFrame::add_mgmt_only_devices(_In_ const std::string &host, _In_ const std::string &port)
{
        mgmt::meta_map meta;
        {
                std::lock_guard<std::mutex> lock(m_mgmt_mtx);
                // 元数据必须属于本机正在连接的那台服务器,否则会把别的服务器的
                // 设备名挂到这一棵树下。
                if (!m_mgmt_cfg.enabled || m_mgmt_cfg.host != host) {
                        return;
                }
                meta = m_mgmt_meta;
        }

        if (meta.empty()) {
                return; // 管理接口还没拉到任何数据(或不可用):保持原有行为
        }

        usbip::device_location loc {
                .hostname = host,
                .service = port,
                .busid = {},
        };
        auto url = make_server_url(loc);
        auto url_u8 = url.utf8_string();

        /*
         * 只有"已共享"的设备才允许出现在客户端。
         *
         * 服务端 /api/devices 是本机 USB 设备的全集(管理页要靠它显示未共享设备
         * 的「开始共享」按钮),而 USB/IP 的 DEVLIST 只返回已 bind 的设备。若把
         * 全集照搬进客户端,就会多出一批点不通的设备 —— 必须按 shared 过滤,
         * 与 DEVLIST 的语义保持一致。
         */
        std::set<std::string> wanted; // "url|busid"
        for (const auto &[busid, m] : meta) {
                if (m.shared) {
                        wanted.insert(url_u8 + "|" + busid);
                }
        }

        auto &tree = *m_treeListCtrl;

        // 先回收:上一轮由本函数补进来、现在已不再共享的行。这些行不在 DEVLIST
        // 里,没有任何其它代码会删它们,不收就会一直显示一台已经收回的设备。
        for (auto it = m_mgmt_only_rows.begin(); it != m_mgmt_only_rows.end(); ) {
                if (wanted.count(*it)) {
                        ++it;
                        continue;
                }

                if (auto sep = it->rfind('|'); sep != std::string::npos) {
                        auto row_url = wxString::FromUTF8(it->substr(0, sep));
                        auto row_busid = wxString::FromUTF8(it->substr(sep + 1));

                        for (auto server = tree.GetFirstItem(); server.IsOk(); server = tree.GetNextSibling(server)) {
                                if (tree.GetItemText(server) != row_url) {
                                        continue;
                                }
                                for (auto dev = tree.GetFirstChild(server); dev.IsOk(); dev = tree.GetNextSibling(dev)) {
                                        if (tree.GetItemText(dev) == row_busid) {
                                                wxLogVerbose(L"%s: 设备 %s 已不再共享,移除客户端列表中的这一行",
                                                             wxString::FromAscii(__func__), row_busid);
                                                remove_device(dev);
                                                break;
                                        }
                                }
                                break;
                        }
                }

                it = m_mgmt_only_rows.erase(it);
        }

        int added_cnt = 0;

        // 缓存本机 clientId:meta 二次细分里的"他人占用"判定需要它。本函数
        // 上方已经取过 meta 的本地副本,这里再独立取一次 self_id,锁粒度更细。
        std::string self_id;
        {
                std::lock_guard<std::mutex> lock(m_mgmt_mtx);
                self_id = m_mgmt_cfg.client_id;
        }

        for (const auto &[busid, m] : meta) {
                if (!m.shared) {
                        continue; // 未共享:客户端不该看到它
                }

                auto server = find_or_add_server(url);

                bool exists = false;
                for (auto item = tree.GetFirstChild(server); item.IsOk(); item = tree.GetNextSibling(item)) {
                        if (tree.GetItemText(item).utf8_string() == busid) {
                                exists = true;
                                break;
                        }
                }
                if (exists) {
                        continue; // 枚举已经建好了这一行,交给 mgmt_apply_event 补充列
                }

                usbip::device_columns dc;
                usbip::get_url(dc) = url;
                dc[usbip::COL_BUSID] = wxString::FromUTF8(busid);
                dc[usbip::COL_PRODUCT] = m.display_name; // 枚举不到描述信息,先用服务器上的显示名
                // 初始状态走 union:刚补进来的设备在驱动层一定是 unplugged,但服务端
                // meta 已知,直接显示"他人占用/排队中/空闲"比先写"未连接"再被
                // mgmt_apply_event 覆盖更省一次闪烁。
                dc[usbip::COL_STATE] = usbip::to_view_state(usbip::state::unplugged, &m, self_id, m.attached_count);
                dc[usbip::COL_RECEIVE_MODE] = usbip::to_string(usbip::receive_mode::zero_copy);

                auto [item, added] = find_or_add_device(dc);
                if (added) {
                        auto flags = usbip::mkflags({usbip::COL_PRODUCT, usbip::COL_STATE, usbip::COL_RECEIVE_MODE});
                        update_device(item, dc, flags);
                        m_mgmt_only_rows.insert(url_u8 + "|" + busid);
                        ++added_cnt;
                }
        }

        if (added_cnt) {
                wxLogVerbose(L"%s: 补入 %d 台仅管理接口可见的设备", wxString::FromAscii(__func__), added_cnt);
                update_summary();
        }
}

void MainFrame::set_menu_columns_labels()
{
        /*
         * 菜单项与树控件列按位置一一对应(顺序见 frame.cpp 的 AppendColumn)。
         * 这里必须用 MGMT_COL_COUNT 而不是 COL_LAST_VISIBLE + 1:
         * 后者只覆盖到"接收模式",会让 mgmt 追加的 源ID/服务器备注/占用方 三列
         * 有列无开关——窄屏下用户无法把"服务器备注/占用方"收起来。
         */
        constexpr auto cnt = MGMT_COL_COUNT;

        auto &menu = *m_menu_columns;
        wxASSERT(menu.GetMenuItemCount() == cnt);

        auto &view = *m_treeListCtrl->GetDataView();
        wxASSERT(cnt <= view.GetColumnCount());

        for (auto pos = 0; pos < cnt; ++pos) {
                auto col = view.GetColumn(pos);
                auto title = col->GetTitle();

                if (auto item = menu.FindItemByPosition(pos); item->GetItemLabel() != title) {
                        item->SetItemLabel(title);
                }
        }
}

wxDataViewColumn* MainFrame::find_column(_In_ const wxString &title) const noexcept
{
        auto &view = *m_treeListCtrl->GetDataView();

        for (auto n = view.GetColumnCount(), pos = 0U; pos < n; ++pos) {
                if (auto col = view.GetColumn(pos); col->GetTitle() == title) {
                        return col;
                }
        }

        wxLogDebug(L"%s: 未找到列 '%s'", wxString::FromAscii(__func__), title);
        return nullptr;
}

wxDataViewColumn* MainFrame::find_column(_In_ int item_id) const noexcept
{
        if (auto item = m_menu_columns->FindItem(item_id)) {
                auto title = item->GetItemLabel(); 
                return find_column(title);
        }

        wxLogDebug(L"%s: 未找到项目 %d", wxString::FromAscii(__func__), item_id);
        return nullptr;
}

void MainFrame::on_view_column_update_ui(wxUpdateUIEvent &event)
{
        if (auto id = event.GetId(); auto col = find_column(id)) {
                event.Check(col->IsShown());
        }
}

void MainFrame::on_view_column(wxCommandEvent &event)
{
        if (auto id = event.GetId(); auto col = find_column(id)) {
                bool checked = event.GetInt();
                col->SetHidden(!checked);
                fit_visible_columns();
                wxPersistenceManager::Get().Save(m_treeListCtrl->GetDataView());
        }
}

void MainFrame::on_item_activated(wxTreeListEvent &event)
{
        auto &tree = *m_treeListCtrl;
        
        if (auto item = event.GetItem(); is_server(tree, item)) {
                //
        } else if (auto &state = tree.GetItemText(item, COL_STATE); state == to_string(state::unplugged)) {
                on_attach(event); 
        } else if (state == to_string(state::plugged)) {
                on_detach(event);
        }
}

void MainFrame::on_view_labels_update_ui(wxUpdateUIEvent &event)
{
        auto shown = m_auiToolBar->HasFlag(wxAUI_TB_TEXT);
        event.Check(shown);
}

void MainFrame::on_view_labels(wxCommandEvent &)
{
        auto &tb = *m_auiToolBar;
        tb.ToggleWindowStyle(wxAUI_TB_TEXT);
        tb.Refresh(false);
        if (m_primary_bar) {
                m_primary_bar->ToggleWindowStyle(wxAUI_TB_TEXT);
                m_primary_bar->Realize();
                m_mgr.Update();
        }
}

int MainFrame::get_port(_In_ wxTreeListItem dev) const
{
        auto &tree = *m_treeListCtrl;

        wxASSERT(!tree.GetFirstChild(dev).IsOk());
        auto &str = tree.GetItemText(dev, COL_PORT);

        int port;
        return str.ToInt(&port) ? port : 0;
}

void MainFrame::on_flip_auto(wxCommandEvent&)
{
        auto toggled = std::vector<wxTreeListItem>{};

        for (auto &dev: get_selected_devices(*m_treeListCtrl)) {
                auto ok = is_checked(dev, COL_PERSISTENT);
                set_checked(dev, COL_PERSISTENT, !ok);
                toggled.push_back(dev);
        }

        /*
         * 立即落盘驱动层 + 同步注册表里所有列。
         *
         * 之前只翻 UI 不调 save(),导致用户勾 Auto → 退出 → 重启后驱动层
         * 没记录 → UI 读不到 persistent → ✓ 自动消失。要让「勾选就生效」
         * 不依赖用户手动点「保存」,这里强制把当前所有设备持久化一遍。
         *
         * save() 内部本身已经会写 wxConfig(产品/序列号/接收模式/备注 等)
         * + vhci::set_persistent(自动连接列),一次调用就把两条链路都覆盖了。
         * 性能上不会成为问题:get_devices() 走的是 wxTreeListCtrl 的内存遍历,
         * 树里通常几十条,设备树再大也只是 O(N)。
         */
        if (!toggled.empty()) {
                save(get_devices(*m_treeListCtrl));
        }
}

void MainFrame::on_flip_receive_mode(wxCommandEvent&)
{
        for (auto &tree = *m_treeListCtrl; auto &dev: get_selected_devices(tree)) {
                auto &mode = tree.GetItemText(dev, COL_RECEIVE_MODE);

                auto r = to_receive_mode(mode) == receive_mode::zero_copy ?
                                receive_mode::low_latency : receive_mode::zero_copy;

                auto &val = to_string(r);
                tree.SetItemText(dev, COL_RECEIVE_MODE, val);
        }
}

void MainFrame::save(_In_ const wxTreeListItems &devices)
{
        auto &cfg = *wxConfig::Get();
        auto path = cfg.GetPath();

        cfg.DeleteGroup(g_key_devices);
        cfg.SetPath(g_key_devices);

        std::vector<persistent_device> persistent;
        auto &tree = *m_treeListCtrl;

        for (int cnt = 0; auto &dev: devices) {

                cfg.SetPath(wxString::Format(L"%d", ++cnt));

                auto server = tree.GetItemParent(dev);
                auto &url = tree.GetItemText(server);

                cfg.Write(g_key_url, url);

                for (auto [key, col] : get_saved_keys()) {
                        auto &value = tree.GetItemText(dev, col);
                        cfg.Write(key, value);
                }

                if (is_checked(dev, COL_PERSISTENT)) {
                        persistent.emplace_back(make_persistent_device(tree, server, dev));
                }

                cfg.SetPath(L"..");
        }

        cfg.Flush();
        cfg.SetPath(path);

        set_status_text(wxString::Format(L"已保存 %zu 个设备", devices.size()), std::chrono::seconds(30));

        if (auto &vhci = get_vhci(); !vhci::set_persistent(vhci.get(), persistent)) {
                auto err = GetLastError();
                wxLogError(L"无法保存自动连接信息\n错误 %lu\n%s", err, GetLastErrorMsg(err));
        }
}

void MainFrame::on_save(wxCommandEvent&)
{
        auto devices = get_devices(*m_treeListCtrl);
        save(devices);
}

void MainFrame::on_save_selected(wxCommandEvent&)
{
        if (auto devices = get_selected_devices(*m_treeListCtrl); devices.empty()) {
                wxMessageBox(L"未选中任何设备", L"保存所选", wxICON_WARNING);
        } else {
                save(devices);
        }
}

void MainFrame::on_load(wxCommandEvent&)
{
        int cnt = 0;
        auto saved = get_saved();

        if (static bool once{}; !once) {
                once = true;
                for (auto &i: get_servers(saved)) {
                        m_comboBoxServer->Append(i);
                }
        }

        for (auto persistent = get_persistent(); auto &dc: saved) {

                auto flags = get_saved_flags();
                auto [dev, added] = find_or_add_device(dc);

                if (!(added || is_empty(*m_treeListCtrl, dev))) {
                        wxLogVerbose(L"跳过加载已存在的设备 %s/%s", get_url(dc), dc[COL_BUSID]);
                        continue;
                }

                constexpr auto state_flag = mkflag(COL_STATE);
                static_assert(!(get_saved_flags() & state_flag));

                // on_load 加载的是 wxConfig 里存的设备行,启动时 meta 还没拉到,
                // 传 0 让 to_view_state 走"未连接"分支,与旧行为一致;后续
                // mgmt_apply_event 拿到 meta 会再 union 一次覆盖。
                dc[COL_STATE] = to_view_state(state::unplugged, nullptr, {}, 0);
                flags = update_from_registry(dc, flags | state_flag, columns::pers_all, persistent);

                update_device(dev, dc, flags);
                ++cnt;
        }

        set_status_text(wxString::Format(L"已加载 %d 个设备", cnt), std::chrono::seconds(30));
}

void MainFrame::on_reload(wxCommandEvent &event)
{
        wxLogVerbose(wxString::FromAscii(__func__));

        auto &tree = *m_treeListCtrl;
        tree.DeleteAllItems();
        update_summary();

        auto &vhci = get_vhci();

        auto devices = vhci::get_imported_devices(vhci.get());
        if (!devices) {
                auto err = GetLastError();
                wxLogError(L"无法读取已连接的设备\n错误 %lu\n%s", err, GetLastErrorMsg(err));
                return;
        }

        auto persistent = get_persistent(vhci);
        auto saved = as_set(get_saved());

        for (auto &dev: *devices) {
                auto [item, added] = find_or_add_device(dev.location);
                wxASSERT(added);

                device_state st {
                        .device = std::move(dev),
                        .state = state::plugged
                };

                auto [dc, flags] = make_device_columns(st); // COL_SERIAL, COL_RECEIVE_MODE are actual

                constexpr auto cols = columns::saved_notes | columns::pers_persistent;
                flags = update_from_registry(dc, flags, cols, saved, persistent);

                // 本机驱动层就是 plugged,union 走"已连接"分支,与 make_device_columns
                // 默认写的 to_string(state::plugged) 一致。仍然调一次以保持逻辑
                // 统一:以后改 union 行为时,这条路径会自动跟上。
                {
                        std::lock_guard<std::mutex> lock(m_mgmt_mtx);
                        std::string self_id = m_mgmt_cfg.client_id;
                        const mgmt::meta_device *meta_ptr = nullptr;
                        mgmt::meta_device meta_snapshot;
                        if (auto it = m_mgmt_meta.find(st.device.location.busid); it != m_mgmt_meta.end()) {
                                meta_snapshot = it->second;
                                meta_ptr = &meta_snapshot;
                        }
                        dc[COL_STATE] = to_view_state(st.state, meta_ptr, self_id,
                                                       meta_ptr ? meta_ptr->attached_count : 0);
                }

                update_device(item, dc, flags);

                /*
                 * 必须同步给管理接口:这批设备是「刷新」时从驱动里读回来的
                 * 已连接设备,它们的状态**没有发生过变化**,因此不会经过
                 * read_loop()/vhci::read_device_state() 产生 DeviceStateEvent,
                 * on_device_state() 也就不会被调用 —— mgmt_note_state() 收不到。
                 *
                 * 而「自动连接」(驱动持久化 PersistentDevices)的设备,在客户端
                 * 重启后走的正是这条路径:驱动里会话还在,状态不变,只靠这里还原。
                 * 少了这一句,心跳包 busids 永远为空,服务端管理页只能显示
                 * 「未登记连接方」,即使设备实际上已经连上(表现为手动连接可见、
                 * 自动连接不可见)。
                 */
                mgmt_note_state(st);
        }

        // 状态刚补全,立刻踢一次心跳,让管理页不必再等一个 10s 轮询间隔
        m_mgmt_kick.store(true);

        if (static bool once; !once) {
                once = true;
                on_load(event);
        }

        // 刷新后重新枚举当前服务器导出的设备,把此前列表中的远端设备
        // (含未连接的 unplugged 行)恢复回来;本机已连接的设备上面已重建。
        if (auto &cb = *m_comboBoxServer; !cb.GetValue().empty()) {
                CallAfter([this] {
                        wxCommandEvent evt;
                        add_exported_devices(evt);
                });
        }
}

std::unique_ptr<wxMenu> MainFrame::create_menu(_In_ const menu_item_descr *items, _In_ int cnt)
{
        auto menu = std::make_unique<wxMenu>();

        for (int i = 0; i < cnt; ++i) {
                if (auto [id, src, handler] = items[i]; clone_menu_item(*menu, id, *src)) {
                        if (handler) {
                                menu->Bind(wxEVT_COMMAND_MENU_SELECTED, handler, this, id);
                        }
                }
        }

        return menu;
}

std::unique_ptr<wxMenu> MainFrame::create_tree_popup_menu()
{
        const menu_item_descr separator {wxID_SEPARATOR, nullptr, nullptr};

        const auto items = std::to_array<menu_item_descr>({
                { wxID_SELECTALL, m_menu_edit, &MainFrame::on_select_all },
                { wxID_COPY, m_menu_edit, &MainFrame::on_copy_rows },
                { wxID_SAVEAS, m_menu_file, &MainFrame::on_save_selected },
                separator,
                { ID_EDIT_NOTES, m_menu_edit, &MainFrame::on_edit_notes },
                { ID_FLIP_AUTO, m_menu_edit, &MainFrame::on_flip_auto },
                { ID_FLIP_RECEIVE_MODE, m_menu_edit, &MainFrame::on_flip_receive_mode },
                separator,
                { ID_EDIT_SERIAL, m_menu_edit, &MainFrame::on_edit_serial },
                { ID_EDIT_GEN_SERIAL, m_menu_edit, &MainFrame::on_edit_gen_serial },
                separator,
                { ID_REMOVE_SERVER, m_menu_devices, &MainFrame::on_remove_server },
        });

        return create_menu(items.data(), items.size());
}

void MainFrame::on_item_context_menu(wxTreeListEvent &event)
{
        /*
         * 右键的那一行不会自动变成选中项;若直接弹菜单,随后点"备注""移除服务器"
         * 之类命令会作用在之前选中的其它行上(或什么都不作用)。这里先把右键行选中。
         */
        if (auto item = event.GetItem(); item.IsOk() && !m_treeListCtrl->IsSelected(item)) {
                m_treeListCtrl->UnselectAll();
                m_treeListCtrl->Select(item);
        }

        if (!m_tree_popup_menu) {
                m_tree_popup_menu = create_tree_popup_menu();
        }

        /*
         * 弹出菜单不参与菜单栏的 wxEVT_UPDATE_UI,克隆时带过来的是源菜单项的
         * 旧状态。按当前选择手动同步一次,避免出现"明明选了行却点不动"。
         */
        wxTreeListItems selection;
        m_treeListCtrl->GetSelections(selection);
        m_tree_popup_menu->Enable(ID_REMOVE_SERVER, !selection.empty());

        PopupMenu(m_tree_popup_menu.get());
}

/*
 * @see wxConfig::Get()->DeleteAll()
 */
void MainFrame::on_view_reset(wxCommandEvent&)
{
        wxMessageDialog dlg(this, L"确定要重置全部设置并重启程序吗?", L"重置设置",
                wxOK | wxCANCEL | wxCANCEL_DEFAULT | wxICON_WARNING | wxCENTRE);

        if (dlg.ShowModal() == wxID_CANCEL) {
                return;
        }

        wxPersistenceManager::Get().DisableSaving();	
        wxConfig::Get()->DeleteGroup(L"Persistent_Options"); // FIXME: private key, defined in src\msw\regconf.cpp

        wchar_t** argv = wxGetApp().argv;

        switch (wxExecute(argv)) {
        case 0: // the command could not be executed
        case -1: // can happen when using DDE under Windows for command execution
                wxLogError(L"无法自动重启程序,请手动重新启动");
                break;
        default:
                post_exit();
        }
}

void MainFrame::on_help_about_lib(wxCommandEvent&)
{
        wxInfoMessageBox(this);
}

void MainFrame::on_frame_mouse_wheel(wxMouseEvent &event)
{
        on_tree_mouse_wheel(event);
}

void MainFrame::on_tree_mouse_wheel(_In_ wxMouseEvent &event)
{
        if (event.GetModifiers() == wxMOD_CONTROL) { // only Ctrl is depressed
                auto dir = event.GetWheelRotation();
                change_font_size(m_treeListCtrl, dir);
        }
}

void MainFrame::on_view_font_increase(wxCommandEvent&)
{
        change_font_size(m_treeListCtrl, 1);
}

void MainFrame::on_view_font_decrease(wxCommandEvent&)
{
        change_font_size(m_treeListCtrl, -1);
}

void MainFrame::on_view_font_default(wxCommandEvent&)
{
        change_font_size(m_treeListCtrl, 0);
}

void MainFrame::on_view_appearance(wxCommandEvent &event)
{
        auto val = get_appearance_value(event.GetId());
        wxGetApp().write_appearance(val);

        wxLogStatus(L"重启程序后外观更改才会生效");
}

void MainFrame::set_status_text(
        _In_ const wxString &text, _In_ std::chrono::seconds duration, _In_ bool verbose_log)
{
        if (verbose_log) {
                wxLogVerbose(text);
        }

        if (auto ms = duration_cast<std::chrono::milliseconds>(duration).count(); m_status_bar_timer.StartOnce(ms)) {
                m_statusBar->PushStatusText(text);
                ++m_status_text_pushes;
        } else {
                wxLogDebug(L"无法启动状态栏计时器");
        }
}

void MainFrame::on_status_bar_timer(wxTimerEvent&)
{
        for (int i = 0; i < m_status_text_pushes; ++i) {
                m_statusBar->PopStatusText();
        }

        m_status_text_pushes = 0;
}

void MainFrame::on_window_fix_timer(wxTimerEvent&)
{
        ensure_visible_window();
}
