/*
 * Copyright (c) 2023-2026 Vadym Hrynchyshyn <vadimgrn@gmail.com>
 */

#pragma once

#include "frame.h"
#include "device_columns.h"
#include "tree_comparator.h"
#include "mgmt.h"

#include <libusbip/win_handle.h>

#include <thread>
#include <mutex>
#include <atomic>
#include <map>
#include <set>
#include <vector>
#include <chrono>

class LogWindow;
class TaskBarIcon;
class wxDataViewColumn;
class wxIconizeEvent;

class DeviceStateEvent;
wxDECLARE_EVENT(EVT_DEVICE_STATE, DeviceStateEvent);


class MainFrame : public Frame
{
public:
	MainFrame(_In_ usbip::Handle read, _In_ int appearance);
	~MainFrame();

	auto start_in_tray() const noexcept { return m_start_in_tray; }
	void iconize_to_tray();
        void show_initial_window();
        void fit_visible_columns();

        /*
         * 单实例:第二个实例广播的自定义消息在这里被收到。
         * 见 app.cpp 的 App::activate_existing_instance()。
         */
        WXLRESULT MSWWindowProc(WXUINT msg, WXWPARAM wParam, WXLPARAM lParam) override;
        void on_activate_from_second_instance();

	void on_exit(wxCommandEvent &event) override;
	void on_detach_all(wxCommandEvent &event) override;

        void set_status_text(_In_ const wxString &text,
                _In_ std::chrono::seconds duration = std::chrono::seconds(10), _In_ bool verbose_log = true);

private:
        friend class TaskBarIcon;
        friend class wxPersistentMainFrame;

        enum { IMG_SERVER, IMG_DEVICE, IMG_CNT };

        /*
         * 定时器必须各自有明确的 ID: Bind(wxEVT_TIMER, ..., this) 不带 ID 时会
         * 把该窗口的所有定时器事件都投给同一个处理函数,两个定时器会互相串扰。
         */
        enum {
                ID_STATUS_BAR_TIMER = 6100, // 状态栏临时文本的自动弹出
                ID_WINDOW_FIX_TIMER,        // 启动后校正窗口几何/可见性,见 ensure_visible_window()
                ID_TASKBAR_WATCHDOG,        // Explorer 重启/托盘刷新后的图标自愈
        };

        /*
         * 默认「启动时最小化到托盘 = 开」。
         *
         * 这是常驻型工具:装好之后用户希望开机就在后台跑着、设备自动连上,
         * 而不是每次登录都弹一个窗口出来。首次安装(注册表里还没有 StartInTray)
         * 时生效;老用户看自己的持久化值,另有一次性迁移统一打开
         * (@see MainFrame::apply_default_tray_autostart)。
         */
	bool m_start_in_tray{true};
        // 关闭按钮 = 收进托盘后台(而非直接退出),同样是常驻工具的合理默认
	bool m_close_to_tray{};
        bool m_fitting_columns{};
        bool m_auto_fit_columns{true};
        bool m_column_fit_pending{};
        wxAuiToolBar* m_primary_bar{};
	bool m_auto_connect{true};
	bool m_auto_connecting{};

        // Management API (server HTTP port, optional) -- see mgmt.cpp
        bool m_mgmt_enabled{true};
        std::thread m_mgmt_thread;
        std::atomic_bool m_mgmt_stop{true};
        std::atomic_bool m_mgmt_kick{false}; // wake mgmt thread to refresh immediately
        std::atomic_int m_mgmt_ok_port{0};   // last reachable management port (0=unset)
        std::mutex m_mgmt_mtx;
        mgmt::settings m_mgmt_cfg;
        std::map<std::string, bool> m_mgmt_attached; // busid -> attached (current server)
        // 本机声明"想用但当前被别人占用"的设备(busid)。每拍心跳随包上报,
        // 由服务端维护 FIFO;设备空出后服务端回发 attach 通知,客户端自动接手。
        std::set<std::string> m_mgmt_queued;
        // 最近一次成功拉取到的服务器设备元数据(占用方/排队名单)。attach 路径
        // 靠它判断"这台设备是不是被别人占用",从而决定排队还是直连。
        mgmt::meta_map m_mgmt_meta;
        // 由 add_mgmt_only_devices() 补进列表的行,键为 "服务器URL|busid"。
        // 只用于回收:这些行不在 USB/IP DEVLIST 里,设备一旦不再共享就必须
        // 由我们自己删掉,否则会一直显示一台已经收回的设备。
        std::set<std::string> m_mgmt_only_rows;
        std::string m_mgmt_last_err;                 // 上次已提示的失败原因(去重用)
        bool m_wsa_started{};                        // WSAStartup 成功过,mgmt_stop 需配对 WSACleanup

        /*
         * 「共享访问密码」(服务端启用访问控制时,连接前必须提供的那个密码)的会话状态。
         *
         * m_access_authorized   host(归一后)→ 本机授权到期的时刻点。命中即代表
         *                       "本次会话已经通过 /api/access/authorize",不必再弹框。
         *                       服务端没启用访问控制时也会记一个短 TTL,纯粹是为了
         *                       避免一次批量 attach 里反复探测同一台服务器。
         * m_access_prompt_snooze 用户在**被动发现**触发的弹框里点了「取消」之后的一段
         *                       时间内不再主动弹框(只提示状态栏)。没有它,后台每 30 秒
         *                       一次的轮询会把同一个模态框不停糊到用户脸上。用户主动
         *                       连接/刷新不受此限制。
         * m_access_prompting   重入保护:弹框期间跑的是嵌套事件循环,期间派发的
         *                       wxThreadEvent 可能再次走到闸门;这个标志同时防止套娃
         *                       弹框和 usbip::run_cancellable() 的共享事件对象被嵌套使用。
         */
        std::map<std::string, std::chrono::steady_clock::time_point> m_access_authorized;
        std::chrono::steady_clock::time_point m_access_prompt_snooze{};
        bool m_access_prompting{};

        LogWindow *m_log{};
	TreeListItemComparator m_tree_cmp;
	std::unique_ptr<TaskBarIcon> m_taskbar_icon;
	std::unique_ptr<wxMenu> m_tree_popup_menu;

	usbip::Handle m_read;
	std::mutex m_read_close_mtx;

	std::thread m_read_thread{ &MainFrame::read_loop, this };

        int m_status_text_pushes{};
        wxTimer m_status_bar_timer{this, ID_STATUS_BAR_TIMER};
        wxTimer m_window_fix_timer{this, ID_WINDOW_FIX_TIMER};
        wxTimer m_taskbar_watchdog_timer{this, ID_TASKBAR_WATCHDOG};

	static wxWithImages::Images get_tree_images();

	void on_close(wxCloseEvent &event) override; 
        void on_iconize(wxIconizeEvent &event);
        void restore_from_tray(); // 托盘还原的唯一入口(先从最小化恢复,再显示并置前)

        void on_attach_stop_all(wxCommandEvent &event) override;
        void on_detach(wxCommandEvent &event) override;
        void on_reload(wxCommandEvent &event) override;
	void on_help_about(wxCommandEvent &event) override;
	void add_exported_devices(wxCommandEvent &event) override;

	// 把"服务端管理接口知道、但 USB/IP 设备枚举里没有"的设备补进列表。
	// 典型场景:设备正被别的客户端占用,USB/IP 的 DEVLIST 不一定再列出它,
	// 但用户必须能看到这台设备、看到谁在用、并据此排队。
	void add_mgmt_only_devices(_In_ const std::string &host, _In_ const std::string &port);
	void on_select_all(wxCommandEvent &event) override;
	void on_has_devices_update_ui(wxUpdateUIEvent &event) override;
	void on_has_selected_devices_update_ui(wxUpdateUIEvent &event) override;
        void on_has_any_selected_devices_update_ui(wxUpdateUIEvent &event) override;
        void on_flip_auto(wxCommandEvent &event) override;
        void on_flip_receive_mode(wxCommandEvent &event) override;
	void on_item_context_menu(wxTreeListEvent &event) override;
	void on_view_reset(wxCommandEvent &event) override;
	void on_help_about_lib(wxCommandEvent&) override;
	void on_copy_rows(wxCommandEvent &event) override;
	void on_remove_server(wxCommandEvent &event) override;
        void on_attach(wxCommandEvent&) override { attach(false); }
        void on_attach_once(wxCommandEvent&) override;
        void on_attach_stop(wxCommandEvent &event) override;

        void on_save(wxCommandEvent &event) override;
	void on_save_selected(wxCommandEvent &event) override;
	void on_load(wxCommandEvent &event) override;

	void on_view_zebra_update_ui(wxUpdateUIEvent &event) override;
	void on_view_zebra(wxCommandEvent &event) override;

	void on_log_show_update_ui(wxUpdateUIEvent &event) override;
	void on_log_show(wxCommandEvent &event) override;

	void on_log_library_update_ui(wxUpdateUIEvent &event) override;
	void on_log_library(wxCommandEvent &event) override;

	void on_log_verbose_update_ui(wxUpdateUIEvent &event) override;
	void on_log_verbose(wxCommandEvent &event) override;

	void on_view_column_update_ui(wxUpdateUIEvent &event) override;
	void on_view_column(wxCommandEvent &event) override;

	void on_start_in_tray_update_ui(wxUpdateUIEvent &event) override;
	void on_start_in_tray(wxCommandEvent &event) override;
	
	void on_close_to_tray_update_ui(wxUpdateUIEvent &event) override;
	void on_close_to_tray(wxCommandEvent &event) override;

	void on_device_state(_In_ DeviceStateEvent &event);
	void on_item_activated(wxTreeListEvent &event) override;

	void on_view_labels(wxCommandEvent &event) override;
	void on_view_labels_update_ui(wxUpdateUIEvent &event) override;

        void on_edit_device_update_ui(wxUpdateUIEvent &event) override;
        void on_edit_notes(wxCommandEvent &event) override;
        void on_edit_serial(wxCommandEvent &event) override;
        void on_edit_gen_serial(wxCommandEvent &event) override;

        void on_frame_mouse_wheel(wxMouseEvent &event) override;

	void on_view_font_increase(wxCommandEvent & event) override;
	void on_view_font_decrease(wxCommandEvent & event) override;
	void on_view_font_default(wxCommandEvent &event) override;

	void on_view_appearance(wxCommandEvent &event) override;
	void on_status_bar_timer(wxTimerEvent&);
	void on_window_fix_timer(wxTimerEvent&);
	void on_taskbar_watchdog(wxTimerEvent&);

        void edit_column_dlg(
                _In_ const wxString &title, _In_ usbip::column_pos_t col, _In_ int maxlen,
                _In_ const std::function<wxString (const wxString&)> &validator);

	void init();
	void check_view_appearance(_In_ int appearance);
	void init_tree_list();
	void apply_theme();
        void configure_workspace_ui();
	void apply_compact_columns();
        /*
         * 「备注」列宽一次性收敛:只改这一列的宽度,不影响用户对其余列的
         * 手动调整(所以用了独立的版本键,没有去抬 CompactColumnsVersion)。
         */
        void apply_note_column_width();
        /*
         * 一次性把「启动时最小化到托盘」+「开机自启动」切到默认开启。
         * 只跑一次(独立版本键),跑完用户手动改动照常持久化。
         */
        void apply_default_tray_autostart();
	void restore_state();

	/*
	 * 恢复上次保存的窗口几何之后,把主窗口拉回可视工作区并确保它是"显示"而非
	 * "最小化到托盘"。详见 wusbip.cpp 里的实现注释。
	 */
	void ensure_visible_window();

	void sync_auto_connect_menu();
	void toggle_auto_connect(wxCommandEvent &event);
	void schedule_auto_connect();

	/*
	 * 启动/刷新后,把"自动(Auto)列已勾选"的设备真正挂载起来。
	 *
	 * 背景:「文件 → 启动时自动连接」原先只做到"连上服务器 + 枚举设备列表",
	 * 勾选的 Auto 设备虽然带着 ✓,却从来没有被 attach —— 驱动的
	 * PersistentDevices 只在驱动重新加载时才自动挂载,普通重启客户端不触发。
	 * 于是"设置了自动连接却没连上"。
	 *
	 * @param url 服务器 URL(形如 host:port);只挂载属于这台服务器的记录,
	 *            避免把别的服务器的设备挂到当前这棵树下。
	 */
	void attach_persistent_devices(_In_ const wxString &url);

        // Auto-start on Windows logon (HKCU ...\Run) -- see wusbip.cpp
        void on_auto_start(wxCommandEvent &event);
        void sync_auto_start_menu();

        // Local (this-PC) display name shown to the server as the owner
        void on_local_name(wxCommandEvent &event);

        // Always-present tray icon (installed at startup, not only when hidden)
        void ensure_taskbar_icon();

        // Status bar summary: servers / devices / connected count
        void update_summary();

	void mgmt_start();
	void mgmt_stop();
	void mgmt_capture_settings();
	/*
	 * 把服务器地址与端口无条件写一次配置(不依赖控件事件)。
	 *
	 * 兜底场景:客户端设了「最小化到托盘 + 开机自动启动」后,用户改完端口直接
	 * 重启电脑/关窗口,控件的 wxEVT_TEXT / wxEVT_SPINCTRL 可能来不及触发 ——
	 * 配置就不落盘。退出路径上补这一笔,保证"改了就能存住"。
	 */
	void save_server_endpoint();
	void mgmt_note_state(const usbip::device_state &st);

	/*
	 * 用驱动层的权威列表校正本机的"已连接"状态(只补上、不清除)。
	 *
	 * 为什么需要:驱动的 DeviceStateEvent 流**不能**用来回答"我现在连着哪些
	 * 设备"—— 一次**失败**的 attach 也会以 state::disconnected 收尾,而这条
	 * 事件会把本机状态刷成"未连接",即使驱动里那条早已建立的会话毫发无损。
	 * 代码依据与实测复现见 mgmt.cpp 里本函数的实现注释。
	 *
	 * @param host  要匹配的服务器主机名(大小写不敏感)
	 * @param busid 非空:只查这一台设备(attach 前的重复连接检查用);
	 *              空:校正该服务器上的全部设备(mgmt_loop 每拍调用一次)
	 * @return 驱动层当前是否挂着 host/busid(仅 busid 非空时有意义)
	 */
	bool mgmt_sync_attached(_In_ const std::string &host, _In_ const std::string &busid);
	void mgmt_on_enable(wxCommandEvent &event);
	void mgmt_apply_event(wxThreadEvent &event);
	void mgmt_handle_queue_notices(_In_ const std::vector<mgmt::queue_notice> &notices);
	void mgmt_loop();

	/*
	 * 「共享访问密码」闸门 —— 在发起 attach / 拉取设备列表之前调用。
	 *
	 * 先 GET /api/access 判断这台服务器要不要访问密码;要的话依次尝试
	 * "本机已授权" → "用保存过的密码静默授权" → "弹框让用户输入"。
	 * 服务端没启用访问控制(含老版本服务端没有该端点)时**直接放行**,不弹任何框。
	 *
	 * @param url         "host:port" 形式的服务器地址
	 * @param interactive false 时只静默尝试已保存的密码,失败即返回 false
	 * @return true = 可以继续;false = 需要密码但没拿到(用户取消/失败),
	 *         调用方必须中止本次操作并给出明确提示
	 */
	bool mgmt_ensure_access(_In_ const wxString &url, _In_ bool interactive = true);

	// 弹一次访问密码输入框并调用 /api/access/authorize;成功后记住授权、
	// 把密码 DPAPI 加密落盘。失败时显示原因并允许重试或取消。
	// 只能在 mgmt_ensure_access() 的重入保护内调用。
	bool mgmt_prompt_access_password(_In_ const mgmt::settings &cfg);

	// 被动发现:任何 API 响应里出现 401 + accessRequired 时由 GUI 线程走这里
	// (心跳/设备列表被访问控制拦下),复用同一套弹框流程。
	void mgmt_on_access_required();

	// 加入/退出某台设备的排队。设备被别人占用时,点击「连接」不再直接失败,
	// 而是登记排队;当前占用方释放后由服务端唤醒本机自动接手。
	// @return false 表示管理接口未启用,调用方应回退到普通 attach
	bool mgmt_queue_device(_In_ const std::string &busid, _In_ bool join);

	// 该设备当前是否正被*别的*客户端占用(来自服务器心跳元数据)。
	// 管理接口不可用时一律返回 false,让调用方退回普通 attach 行为。
	bool mgmt_device_busy(_In_ const std::string &busid);

	// 清空本机全部排队意愿(「全部断开」时调用),避免设备一空出来又被自动连回。
	void mgmt_clear_all_queues();

	// 在客户端退出前向服务端发一次含 shutdown=true 的最后一个心跳,
	// 让服务端 0 延迟把设备共享态收回。否则要等 60s watchdog 才能 unbind,
	// 这段时间里其他客户端无法看到/绑定该设备,管理页面也会看到"设备被占
	// 用"但客户端其实已经退了。
	void mgmt_send_goodbye();

	void read_loop();
	void break_read_loop();

	wxTreeListItem find_or_add_server(_In_ const wxString &url);

	std::pair<wxTreeListItem, bool> find_or_add_device(_In_ const wxString &url, _In_ const wxString &busid);
	std::pair<wxTreeListItem, bool> find_or_add_device(_In_ const usbip::device_location &loc);
	std::pair<wxTreeListItem, bool> find_or_add_device(_In_ const usbip::device_columns &dc);

	void remove_device(_In_ wxTreeListItem dev);

        DWORD detach(_In_ int port);
        void attach(_In_ bool once);

        bool is_persistent(_In_ wxTreeListItem device);

        DWORD attach(_In_ const wxString &url, _In_ const wxString &busid,
                     _In_ const wxString &serial, _In_ const wxString &recv_mode, _In_ bool once);

	void post_refresh();
	void post_exit();

	bool is_checked(_In_ wxTreeListItem device, _In_ usbip::column_pos_t col);
	void set_checked(_In_ wxTreeListItem device, _In_ usbip::column_pos_t col, _In_ bool checked);

	void update_device(_In_ wxTreeListItem device, _In_ const usbip::device_columns &dc, _In_ unsigned int flags);
	
	auto connect(_In_ const wxString &hostname, _In_ const wxString &service, 
		              _In_ const std::string &hostname_u8, _In_ const std::string &service_u8);

	wxDataViewColumn* find_column(_In_ const wxString &title) const noexcept;
	wxDataViewColumn* find_column(_In_ int item_id) const noexcept;
	void set_menu_columns_labels();

	int get_port(_In_ wxTreeListItem dev) const;
	wxTreeListItem get_edit_device();

	void save(_In_ const wxTreeListItems &devices);
	void on_tree_mouse_wheel(_In_ wxMouseEvent &event);

	using menu_item_descr = std::tuple<int, wxMenu*, decltype(&MainFrame::on_attach)>;
	std::unique_ptr<wxMenu> create_menu(_In_ const menu_item_descr *items, _In_ int cnt);

	std::unique_ptr<wxMenu> create_tree_popup_menu();
};
