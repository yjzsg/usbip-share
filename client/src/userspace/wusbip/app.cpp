/*
 * Copyright (c) 2023-2025 Vadym Hrynchyshyn <vadimgrn@gmail.com>
 */

#include "app.h"
#include "utils.h"
#include "wusbip.h"

#include <libusbip/src/file_ver.h>
#include <wx/config.h>

#include <windows.h>

namespace
{

using namespace usbip;

/*
 * 单实例互斥体的名字。
 *
 * 用 "Local\" 前缀而不是 "Global\":客户端是每用户会话运行的程序,
 * 让不同登录会话(如远程桌面)各自能开一个才是正确行为;
 * "Global\" 会连别的用户/会话都一起挡掉。
 *
 * 名字里带 "USB-SHARE" 而不是沿用 exe 名,是为了避免以后改 exe 名时
 * 忘记同步这里,导致新旧版本的互斥体名字不同而互相放行。
 */
constexpr auto kInstanceMutexName = L"Local\\USB-SHARE-Client-SingleInstance";

/*
 * 第二个实例通过这个自定义消息通知已运行实例:"用户又点了一次程序,
 * 请把你的窗口显示出来"。
 *
 * 用 RegisterWindowMessageW 而不是 WM_USER+xxx:WM_USER 段的消息只在
 * 单个窗口类内有意义,广播时可能被别的程序的窗口误解释;
 * RegisterWindowMessageW 返回的是全系统唯一值。
 *
 * 实现在 App::instance_message()(类是公开的,MainFrame 要用它比对消息),
 * 这里只留一个内部使用的消息名常量。
 */
constexpr auto kInstanceMessageName = L"USB-SHARE-Client-Activate-Existing-Instance";

/*
 * 主窗口的标题(必须与 MainFrame::init() 里的 SetTitle 一致)。
 * 用来在"已经有一个实例在跑"时把它的窗口找出来。
 */
constexpr auto kMainWindowTitle = L"USB-SHARE 客户端";

/*
 * 找已运行实例的主窗口。
 *
 * 用标题精确匹配 + 要求窗口可见/非最小化之外的窗口:这里只要求标题相等,
 * 是因为"收进托盘"的窗口仍然存在(只是隐藏),标题不变,依然能命中原窗口。
 * 同进程内我们自己的窗口还没建,所以不存在误命中自己。
 */
HWND find_existing_window()
{
        struct Ctx {
                HWND found;
        } ctx{};

        ::EnumWindows([](HWND hwnd, LPARAM lp) -> BOOL {
                auto &c = *reinterpret_cast<Ctx *>(lp);

                wchar_t title[256]{};
                if (!::GetWindowTextW(hwnd, title, _countof(title))) {
                        return TRUE;
                }

                if (::wcscmp(title, kMainWindowTitle) != 0) {
                        return TRUE;
                }

                // 标题对了,再确认它确实是本程序的进程(避免别的程序起了同名窗口)
                DWORD pid = 0;
                ::GetWindowThreadProcessId(hwnd, &pid);

                wchar_t exe[MAX_PATH]{};
                if (!::GetModuleFileNameW(nullptr, exe, _countof(exe))) {
                        return TRUE;
                }

                auto h = ::OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pid);
                if (!h) {
                        return TRUE;
                }

                wchar_t other[MAX_PATH]{};
                DWORD n = _countof(other);
                auto ok = ::QueryFullProcessImageNameW(h, 0, other, &n);
                ::CloseHandle(h);

                if (ok && _wcsicmp(exe, other) == 0) {
                        c.found = hwnd;
                        return FALSE; // 找到了,停止枚举
                }

                return TRUE;
        }, reinterpret_cast<LPARAM>(&ctx));

        return ctx.found;
}

auto read_appearance(_In_ const wchar_t *key)
{
        using enum wxApp::Appearance;
        enum { Sys = static_cast<long>(System), LightMode = static_cast<long>(Light) };

        auto &cfg = *wxConfig::Get();
        // 新安装默认用浅色蓝灰主题;用户仍可在"视图 -> 外观"中选择跟随系统/深色。
        auto val = cfg.ReadLong(key, LightMode);

        if (!(val >= Sys && val <= static_cast<long>(Dark))) {
                wxLogDebug(L"无效的值 %s=%d", key, val);
                val = Sys;
        }

        return static_cast<wxApp::Appearance>(val);
}

auto init_mainframe(_In_ wxApp::Appearance app)
{
        wxString err;

        if (auto read = usbip::init(err) ? vhci::open() : Handle()) {
                if (auto &frame = *new MainFrame(std::move(read), static_cast<int>(app)); frame.start_in_tray()) {
                        frame.iconize_to_tray();
                } else {
                        frame.show_initial_window();
                }
                return true;
        }

        if (err.empty()) {
                err = GetLastErrorMsg();
        }

        err += L"\n\n如果尚未安装 USB-SHARE 内核驱动,请先运行安装包安装驱动"
               L"(驱动 usbip2_ude / usbip2_filter),然后再启动本程序。";

        wxSafeShowMessage(L"致命错误", err);
        return false;
}

} // namespace


App::App()
{
        Bind(wxEVT_END_SESSION, &App::on_end_session, this);
}

unsigned int App::instance_message()
{
        /*
         * register 只做一次;RegisterWindowMessageW 对同一个字符串
         * 永远返回同一个值,重复调用也无害,static 只是省掉重复的调用。
         */
        static unsigned int msg = ::RegisterWindowMessageW(kInstanceMessageName);
        return msg;
}

bool App::OnInit()
{
        if (!wxApp::OnInit()) {
                return false;
        }

        set_names();

        /*
         * 单实例检查必须在任何"有副作用"的动作之前:
         * 特别是 init_mainframe() 里的 vhci::open() 会打开内核设备,
         * 两个实例同时挂载设备会互相打架(重复 attach / 设备状态错乱)。
         */
        if (!acquire_single_instance()) {
                /*
                 * 已经有实例在跑:通知它把窗口显示出来,然后本进程立刻退出。
                 *
                 * 这里不弹提示框 —— 用户只是双击了图标,弹窗反而打扰;
                 * 把已有窗口拉到前台才是用户期望的结果。
                 */
                activate_existing_instance();
                return false; // 返回 false 让 wxApp 不进入消息循环,进程正常退出
        }

        auto app = set_appearance();
        return init_mainframe(app);
}

bool App::acquire_single_instance()
{
        auto mutex = ::CreateMutexW(nullptr, TRUE, kInstanceMutexName);

        if (!mutex) {
                /*
                 * 连互斥体都创建不出来(极罕见):不能因此拒绝启动,
                 * 否则程序就彻底打不开了。放行并在日志里留痕。
                 */
                wxLogError(L"创建单实例互斥体失败,错误 %lu,本次不限制多开", ::GetLastError());
                return true;
        }

        if (::GetLastError() == ERROR_ALREADY_EXISTS) {
                // 已经有实例持有这个互斥体 —— 我们是第二个实例
                ::CloseHandle(mutex);
                return false;
        }

        // 第一个实例:持有到进程结束(由 OS 在进程退出时自动释放)
        m_instance_mutex = mutex;
        return true;
}

void App::activate_existing_instance()
{
        auto msg = instance_message();
        if (!msg) {
                wxLogError(L"注册单实例激活消息失败,错误 %lu", ::GetLastError());
                return;
        }

        /*
         * 先找到已运行实例的主窗口。找窗口而不是纯靠广播,是因为下面要
         * 把这个进程的"前台激活权限"转交给它 —— 那一步需要知道对方 PID。
         *
         * 匹配条件用"窗口标题 + 可见窗口"而不是类名:类名 wxWindowNR 是
         * wx 通用窗口类,别的 wx 程序也可能用,单靠类名会误命中。
         */
        auto hwnd = find_existing_window();

        if (hwnd) {
                DWORD pid = 0;
                ::GetWindowThreadProcessId(hwnd, &pid);

                /*
                 * 关键一步:允许对方抢前台。
                 *
                 * Windows 只让"当前拥有前台权限的进程"把焦点交给别人。
                 * 本进程是刚被 CreateProcess 拉起来的后台进程,自己没有前台
                 * 权限,所以对方收到消息后调 SetForegroundWindow 会被系统
                 * 静默拒绝(实测:窗口显示了,但焦点仍留在 Chrome)。
                 *
                 * AllowSetForegroundWindow(对方PID) 把"我这次触发所带来的
                 * 前台权限"转交给对方,它随后就能合法地把窗口置前。
                 * ASFW_ANY 更宽松但没有必要,精确到 PID 更安全。
                 */
                if (!::AllowSetForegroundWindow(pid)) {
                        wxLogDebug(L"AllowSetForegroundWindow(%lu) 失败,错误 %lu",
                                   pid, ::GetLastError());
                }

                // 直接投递给具体窗口,比广播更可靠(不受 UIPI 之外的干扰)
                ::PostMessageW(hwnd, msg, 0, 0);
                return;
        }

        /*
         * 兜底:没找到明确的窗口(例如对方刚好正在重建窗口)时退回广播。
         * 广播到 HWND_BROADCAST 能被对方窗口过程收到,只是这种情况下
         * 抢前台的权限没法精确转交,置顶可能失败 —— 但窗口至少会显示出来。
         */
        ::PostMessageW(HWND_BROADCAST, msg, 0, 0);
}

void App::set_names()
{
        auto &v = win::get_file_version();

        /*
         * 注意: GetAppName()/GetVendorName() 决定 wxConfig 的注册表路径
         * (HKCU\Software\<Vendor>\<App>)。该路径同时也是 win::config_registry_root()
         * 的返回值(见 utils.h),配置读写两边共用同一个真相。
         * 这里刻意沿用 .rc 里的 ProductName/CompanyName, 不要改成中文名,
         * 否则老用户的 ClientId/服务器/列宽等设置会全部"丢失"(其实是被写到新路径)。
         *
         * 界面显示名单独用 SetAppDisplayName 设置: 它只影响标题栏、关于对话框等,
         * 不参与配置路径计算。
         */
        SetAppName(wx_string(v.GetProductName()));
        SetVendorName(wx_string(v.GetCompanyName()));
        SetAppDisplayName(L"USB-SHARE 客户端");
}

auto App::set_appearance() -> Appearance
{
        auto app = read_appearance(m_appearance);

        if (auto res = SetAppearance(app); res != AppearanceResult::Ok) {
                wxLogError(L"SetAppearance(%d) 错误 %d", app, res);
        }

        return app;
}

void App::write_appearance(_In_ int val)
{
        if (auto &cfg = *wxConfig::Get(); !cfg.Write(m_appearance, val)) {
                wxLogError(L"无法写入 %s=%d", m_appearance, val);
        }
}

/*
 * wxEVT_CLOSE_WINDOW will not be sent to MainFrame, but m_read_thread must be joined.
 * @see MainFrame::on_close
 */
void App::on_end_session(_In_ wxCloseEvent&)
{
        if (auto wnd = GetMainTopWindow()) {
                wnd->Close(true);
        }
}

wxIMPLEMENT_APP(App);
