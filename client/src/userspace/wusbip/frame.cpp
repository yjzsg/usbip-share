///////////////////////////////////////////////////////////////////////////
// C++ code generated with wxFormBuilder (version 4.2.1-ddd6fee7)
// http://www.wxformbuilder.org/
//
// PLEASE DO *NOT* EDIT THIS FILE!
///////////////////////////////////////////////////////////////////////////

#include "frame.h"

///////////////////////////////////////////////////////////////////////////

Frame::Frame( wxWindow* parent, wxWindowID id, const wxString& title, const wxPoint& pos, const wxSize& size, long style ) : wxFrame( parent, id, title, pos, size, style )
{
	#if wxCHECK_VERSION(3, 1, 3)
	[[maybe_unused]] auto pick_resname = [dark = wxSystemSettings::GetAppearance().IsDark()] (const auto& name, const auto& name_dark) -> decltype(auto)
	{
		return dark ? name_dark : name;
	};
	#else
	[[maybe_unused]] auto pick_resname = [] (const auto& name, const auto&) -> decltype(auto) { return name; };
	#endif

	this->SetSizeHints( wxDefaultSize, wxDefaultSize );
	m_mgr.SetManagedWindow(this);
	m_mgr.SetFlags(wxAUI_MGR_DEFAULT);

	m_statusBar = this->CreateStatusBar( 2, wxSTB_SIZEGRIP, wxID_ANY );
	int __sb_widths[] = { -1, 400 };
	m_statusBar->SetStatusWidths( 2, __sb_widths );
	m_menubar = new wxMenuBar( 0 );
	m_menu_file = new wxMenu();
	wxMenuItem* m_file_load;
	m_file_load = new wxMenuItem( m_menu_file, wxID_ANY, wxString( L"加载" ) + wxT('\t') + wxT("CTRL+L"), L"加载已保存的设备", wxITEM_NORMAL );
	#ifdef __WXMSW__
	m_file_load->SetBitmaps( wxBitmapBundle::FromSVGResource( pick_resname(wxT("save_alt_svg"), wxT("save_alt_svg_dark")), {16, 16} ) );
	#elif (defined( __WXGTK__ ) || defined( __WXOSX__ ))
	m_file_load->SetBitmap( wxBitmapBundle::FromSVGResource( pick_resname(wxT("save_alt_svg"), wxT("save_alt_svg_dark")), {16, 16} ) );
	#endif
	m_menu_file->Append( m_file_load );

	wxMenuItem* m_file_save;
	m_file_save = new wxMenuItem( m_menu_file, wxID_SAVE, wxString( L"保存" ) + wxT('\t') + wxT("CTRL+S"), L"保存设备", wxITEM_NORMAL );
	#ifdef __WXMSW__
	m_file_save->SetBitmaps( wxBitmapBundle::FromSVGResource( pick_resname(wxT("save_svg"), wxT("save_svg_dark")), {16, 16} ) );
	#elif (defined( __WXGTK__ ) || defined( __WXOSX__ ))
	m_file_save->SetBitmap( wxBitmapBundle::FromSVGResource( pick_resname(wxT("save_svg"), wxT("save_svg_dark")), {16, 16} ) );
	#endif
	m_menu_file->Append( m_file_save );

	wxMenuItem* m_file_save_selected;
	m_file_save_selected = new wxMenuItem( m_menu_file, wxID_SAVEAS, wxString( L"保存所选" ) + wxT('\t') + wxT("ALT+S"), L"保存所选设备", wxITEM_NORMAL );
	#ifdef __WXMSW__
	m_file_save_selected->SetBitmaps( wxBitmapBundle::FromSVGResource( pick_resname(wxT("SAVE_MULTIPLE_SVG"), wxT("SAVE_MULTIPLE_SVG_DARK")), {16, 16} ) );
	#elif (defined( __WXGTK__ ) || defined( __WXOSX__ ))
	m_file_save_selected->SetBitmap( wxBitmapBundle::FromSVGResource( pick_resname(wxT("SAVE_MULTIPLE_SVG"), wxT("SAVE_MULTIPLE_SVG_DARK")), {16, 16} ) );
	#endif
	m_menu_file->Append( m_file_save_selected );

	m_menu_file->AppendSeparator();

	wxMenuItem* m_file_start_in_tray;
	m_file_start_in_tray = new wxMenuItem( m_menu_file, ID_START_IN_TRAY, wxString( L"启动时最小化到托盘" ) , L"程序启动后最小化到系统托盘", wxITEM_CHECK );
	m_menu_file->Append( m_file_start_in_tray );

	wxMenuItem* m_file_close_to_tray;
	m_file_close_to_tray = new wxMenuItem( m_menu_file, ID_CLOSE_TO_TRAY, wxString( L"关闭时直接退出" ) , L"勾选后,点击窗口关闭按钮将直接退出程序(默认保留在系统托盘后台运行)", wxITEM_CHECK );
	m_menu_file->Append( m_file_close_to_tray );

	wxMenuItem* m_file_auto_connect;
	m_file_auto_connect = new wxMenuItem( m_menu_file, ID_AUTO_CONNECT, wxString( L"启动时自动连接" ) , L"程序启动后自动连接上次使用的服务器", wxITEM_CHECK );
	m_menu_file->Append( m_file_auto_connect );

	wxMenuItem* m_file_mgmt;
	m_file_mgmt = new wxMenuItem( m_menu_file, ID_MGMT_ENABLE, wxString( L"服务器管理接口(设备名/备注/占用方)" ) , L"从服务器的 HTTP 管理接口读取设备名称、备注与占用方,并发送本机心跳", wxITEM_CHECK );
	m_menu_file->Append( m_file_mgmt );

	wxMenuItem* m_file_auto_start;
	m_file_auto_start = new wxMenuItem( m_menu_file, ID_AUTO_START, wxString( L"开机自启动" ) , L"登录 Windows 后自动启动本程序", wxITEM_CHECK );
	m_menu_file->Append( m_file_auto_start );

	wxMenuItem* m_file_local_name;
	m_file_local_name = new wxMenuItem( m_menu_file, ID_LOCAL_NAME, wxString( L"本机名称…" ) , L"设置这台电脑在服务器上显示的名称(占用方/当前连接方)", wxITEM_NORMAL );
	m_menu_file->Append( m_file_local_name );

	m_menu_file->AppendSeparator();

	wxMenuItem* m_file_exit;
	m_file_exit = new wxMenuItem( m_menu_file, wxID_EXIT, wxString( L"退出" ) , wxEmptyString, wxITEM_NORMAL );
	#ifdef __WXMSW__
	m_file_exit->SetBitmaps( wxBitmapBundle::FromSVGResource( pick_resname(wxT("power_svg"), wxT("power_svg_dark")), {16, 16} ) );
	#elif (defined( __WXGTK__ ) || defined( __WXOSX__ ))
	m_file_exit->SetBitmap( wxBitmapBundle::FromSVGResource( pick_resname(wxT("power_svg"), wxT("power_svg_dark")), {16, 16} ) );
	#endif
	m_menu_file->Append( m_file_exit );

	m_menubar->Append( m_menu_file, L"文件" );

	m_menu_edit = new wxMenu();
	wxMenuItem* m_select_all;
	m_select_all = new wxMenuItem( m_menu_edit, wxID_SELECTALL, wxString( L"全选" ) + wxT('\t') + wxT("CTRL+A"), L"选中全部设备", wxITEM_NORMAL );
	#ifdef __WXMSW__
	m_select_all->SetBitmaps( wxBitmapBundle::FromSVGResource( pick_resname(wxT("select_all_svg"), wxT("select_all_svg_dark")), {16, 16} ) );
	#elif (defined( __WXGTK__ ) || defined( __WXOSX__ ))
	m_select_all->SetBitmap( wxBitmapBundle::FromSVGResource( pick_resname(wxT("select_all_svg"), wxT("select_all_svg_dark")), {16, 16} ) );
	#endif
	m_menu_edit->Append( m_select_all );

	wxMenuItem* m_copy_rows;
	m_copy_rows = new wxMenuItem( m_menu_edit, wxID_COPY, wxString( L"复制行" ) + wxT('\t') + wxT("CTRL+C"), L"复制选中行到剪贴板", wxITEM_NORMAL );
	#ifdef __WXMSW__
	m_copy_rows->SetBitmaps( wxBitmapBundle::FromSVGResource( pick_resname(wxT("COPY_SVG"), wxT("COPY_SVG_DARK")), {16, 16} ) );
	#elif (defined( __WXGTK__ ) || defined( __WXOSX__ ))
	m_copy_rows->SetBitmap( wxBitmapBundle::FromSVGResource( pick_resname(wxT("COPY_SVG"), wxT("COPY_SVG_DARK")), {16, 16} ) );
	#endif
	m_menu_edit->Append( m_copy_rows );

	m_menu_edit->AppendSeparator();

	wxMenuItem* m_edit_notes;
	m_edit_notes = new wxMenuItem( m_menu_edit, ID_EDIT_NOTES, wxString( L"备注" ) + wxT('\t') + wxT("CTRL+N"), L"编辑设备备注", wxITEM_NORMAL );
	#ifdef __WXMSW__
	m_edit_notes->SetBitmaps( wxBitmapBundle::FromSVGResource( pick_resname(wxT("COMMENT_EDIT_SVG"), wxT("COMMENT_EDIT_SVG_DARK")), {16, 16} ) );
	#elif (defined( __WXGTK__ ) || defined( __WXOSX__ ))
	m_edit_notes->SetBitmap( wxBitmapBundle::FromSVGResource( pick_resname(wxT("COMMENT_EDIT_SVG"), wxT("COMMENT_EDIT_SVG_DARK")), {16, 16} ) );
	#endif
	m_menu_edit->Append( m_edit_notes );

	wxMenuItem* m_flip_auto;
	m_flip_auto = new wxMenuItem( m_menu_edit, ID_FLIP_AUTO, wxString( L"切换自动(Auto)" ) + wxT('\t') + wxT("CTRL+P"), L"为选中设备启用或停用自动连接(Auto)", wxITEM_NORMAL );
	#ifdef __WXMSW__
	m_flip_auto->SetBitmaps( wxNullBitmap );
	#elif (defined( __WXGTK__ ) || defined( __WXOSX__ ))
	m_flip_auto->SetBitmap( wxNullBitmap );
	#endif
	m_menu_edit->Append( m_flip_auto );

	wxMenuItem* m_flip_receive_mode;
	m_flip_receive_mode = new wxMenuItem( m_menu_edit, ID_FLIP_RECEIVE_MODE, wxString( L"切换接收模式" ), L"在默认(零拷贝)与低延迟接收模式之间切换", wxITEM_NORMAL );
	#ifdef __WXMSW__
	m_flip_receive_mode->SetBitmaps( wxNullBitmap );
	#elif (defined( __WXGTK__ ) || defined( __WXOSX__ ))
	m_flip_receive_mode->SetBitmap( wxNullBitmap );
	#endif
	m_menu_edit->Append( m_flip_receive_mode );

	m_menu_edit->AppendSeparator();

	wxMenuItem* m_edit_serial;
	m_edit_serial = new wxMenuItem( m_menu_edit, ID_EDIT_SERIAL, wxString( L"序列号" ) , L"编辑设备序列号", wxITEM_NORMAL );
	#ifdef __WXMSW__
	m_edit_serial->SetBitmaps( wxNullBitmap );
	#elif (defined( __WXGTK__ ) || defined( __WXOSX__ ))
	m_edit_serial->SetBitmap( wxNullBitmap );
	#endif
	m_menu_edit->Append( m_edit_serial );

	wxMenuItem* m_edit_gen_serial;
	m_edit_gen_serial = new wxMenuItem( m_menu_edit, ID_EDIT_GEN_SERIAL, wxString( L"生成序列号" ) , L"生成唯一序列号", wxITEM_NORMAL );
	#ifdef __WXMSW__
	m_edit_gen_serial->SetBitmaps( wxNullBitmap );
	#elif (defined( __WXGTK__ ) || defined( __WXOSX__ ))
	m_edit_gen_serial->SetBitmap( wxNullBitmap );
	#endif
	m_menu_edit->Append( m_edit_gen_serial );

	m_menubar->Append( m_menu_edit, L"编辑" );

	m_menu_view = new wxMenu();
	m_menu_columns = new wxMenu();
	wxMenuItem* m_menu_columnsItem = new wxMenuItem( m_menu_view, wxID_ANY, L"列", wxEmptyString, wxITEM_NORMAL, m_menu_columns );
	#if (defined( __WXMSW__ ) || defined( __WXGTK__ ) || defined( __WXOSX__ ))
	m_menu_columnsItem->SetBitmap( wxBitmapBundle::FromSVGResource( pick_resname(wxT("VIEW_COLUMNS_SVG"), wxT("VIEW_COLUMNS_SVG_DARK")), {16, 16} ) );
	#endif

	wxMenuItem* m_view_busid;
	m_view_busid = new wxMenuItem( m_menu_columns, wxID_ANY, wxString( L"?" ) , wxEmptyString, wxITEM_CHECK );
	m_menu_columns->Append( m_view_busid );
	m_view_busid->Check( true );

	wxMenuItem* m_view_port;
	m_view_port = new wxMenuItem( m_menu_columns, wxID_ANY, wxString( L"?" ) , wxEmptyString, wxITEM_CHECK );
	m_menu_columns->Append( m_view_port );
	m_view_port->Enable( false );
	m_view_port->Check( true );

	wxMenuItem* m_view_speed;
	m_view_speed = new wxMenuItem( m_menu_columns, wxID_ANY, wxString( L"?" ) , wxEmptyString, wxITEM_CHECK );
	m_menu_columns->Append( m_view_speed );
	m_view_speed->Check( true );

	wxMenuItem* m_view_vendor;
	m_view_vendor = new wxMenuItem( m_menu_columns, wxID_ANY, wxString( L"?" ) , wxEmptyString, wxITEM_CHECK );
	m_menu_columns->Append( m_view_vendor );
	m_view_vendor->Check( true );

	wxMenuItem* m_view_product;
	m_view_product = new wxMenuItem( m_menu_columns, wxID_ANY, wxString( L"?" ) , wxEmptyString, wxITEM_CHECK );
	m_menu_columns->Append( m_view_product );
	m_view_product->Check( true );

	wxMenuItem* m_view_state;
	m_view_state = new wxMenuItem( m_menu_columns, wxID_ANY, wxString( L"?" ) , wxEmptyString, wxITEM_CHECK );
	m_menu_columns->Append( m_view_state );
	m_view_state->Enable( false );
	m_view_state->Check( true );

	wxMenuItem* m_view_persistent;
	m_view_persistent = new wxMenuItem( m_menu_columns, wxID_ANY, wxString( L"?" ) , wxEmptyString, wxITEM_CHECK );
	m_menu_columns->Append( m_view_persistent );
	m_view_persistent->Check( true );

	wxMenuItem* m_view_notes;
	m_view_notes = new wxMenuItem( m_menu_columns, wxID_ANY, wxString( L"?" ) , wxEmptyString, wxITEM_CHECK );
	m_menu_columns->Append( m_view_notes );
	m_view_notes->Check( true );

	wxMenuItem* m_view_devid;
	m_view_devid = new wxMenuItem( m_menu_columns, wxID_ANY, wxString( L"?" ) , wxEmptyString, wxITEM_CHECK );
	m_menu_columns->Append( m_view_devid );

	wxMenuItem* m_view_serial;
	m_view_serial = new wxMenuItem( m_menu_columns, wxID_ANY, wxString( L"?" ) , wxEmptyString, wxITEM_CHECK );
	m_menu_columns->Append( m_view_serial );

	wxMenuItem* m_view_receive_mode;
	m_view_receive_mode = new wxMenuItem( m_menu_columns, wxID_ANY, wxString( _("?") ) , wxEmptyString, wxITEM_CHECK );
	m_menu_columns->Append( m_view_receive_mode );

	// 以下三列由服务器管理接口(mgmt)填充, 位置见 device_columns.h 的 mgmt_column_pos_t。
	// 菜单项必须紧跟其后追加, 顺序与 AppendColumn 一一对应(set_menu_columns_labels 按位置配对)。
	wxMenuItem* m_view_source_id;
	m_view_source_id = new wxMenuItem( m_menu_columns, wxID_ANY, wxString( L"?" ) , wxEmptyString, wxITEM_CHECK );
	m_menu_columns->Append( m_view_source_id );

	wxMenuItem* m_view_mgmt_remark;
	m_view_mgmt_remark = new wxMenuItem( m_menu_columns, wxID_ANY, wxString( L"?" ) , wxEmptyString, wxITEM_CHECK );
	m_menu_columns->Append( m_view_mgmt_remark );
	m_view_mgmt_remark->Check( true );

	wxMenuItem* m_view_mgmt_owner;
	m_view_mgmt_owner = new wxMenuItem( m_menu_columns, wxID_ANY, wxString( L"?" ) , wxEmptyString, wxITEM_CHECK );
	m_menu_columns->Append( m_view_mgmt_owner );
	m_view_mgmt_owner->Check( true );

	wxMenuItem* m_view_mgmt_queue;
	m_view_mgmt_queue = new wxMenuItem( m_menu_columns, wxID_ANY, wxString( L"?" ) , wxEmptyString, wxITEM_CHECK );
	m_menu_columns->Append( m_view_mgmt_queue );
	m_view_mgmt_queue->Check( true );

	m_menu_view->Append( m_menu_columnsItem );

	m_view_appearance = new wxMenu();
	wxMenuItem* m_view_appearanceItem = new wxMenuItem( m_menu_view, wxID_ANY, L"外观", wxEmptyString, wxITEM_NORMAL, m_view_appearance );
	#if (defined( __WXMSW__ ) || defined( __WXGTK__ ) || defined( __WXOSX__ ))
	m_view_appearanceItem->SetBitmap( wxBitmapBundle::FromSVGResource( pick_resname(wxT("APPEARANCE_SVG"), wxT("APPEARANCE_SVG_DARK")), {16, 16} ) );
	#endif

	wxMenuItem* m_appearance_system;
	m_appearance_system = new wxMenuItem( m_view_appearance, wxID_FILE1, wxString( L"跟随系统" ) , L"使用 Windows 默认外观", wxITEM_RADIO );
	m_view_appearance->Append( m_appearance_system );
	m_appearance_system->Check( true );

	wxMenuItem* m_appearance_light;
	m_appearance_light = new wxMenuItem( m_view_appearance, wxID_FILE2, wxString( L"浅色" ) , wxEmptyString, wxITEM_RADIO );
	#ifdef __WXMSW__
	m_appearance_light->SetBitmaps( wxBitmapBundle::FromSVGResource( pick_resname(wxT("APP_LIGHT_CHECKED_SVG"), wxT("APP_LIGHT_CHECKED_SVG_DARK")), {16, 16} ), wxBitmapBundle::FromSVGResource( pick_resname(wxT("APP_LIGHT_UNCHECKED_SVG"), wxT("APP_LIGHT_UNCHECKED_SVG_DARK")), {16, 16} ) );
	#elif (defined( __WXGTK__ ) || defined( __WXOSX__ ))
	m_appearance_light->SetBitmap( wxBitmapBundle::FromSVGResource( pick_resname(wxT("APP_LIGHT_CHECKED_SVG"), wxT("APP_LIGHT_CHECKED_SVG_DARK")), {16, 16} ) );
	#endif
	m_view_appearance->Append( m_appearance_light );

	wxMenuItem* m_appearance_dark;
	m_appearance_dark = new wxMenuItem( m_view_appearance, wxID_FILE3, wxString( L"深色" ) , wxEmptyString, wxITEM_RADIO );
	#ifdef __WXMSW__
	m_appearance_dark->SetBitmaps( wxBitmapBundle::FromSVGResource( pick_resname(wxT("APP_DARK_CHECKED_SVG"), wxT("APP_DARK_CHECKED_SVG_DARK")), {16, 16} ), wxBitmapBundle::FromSVGResource( pick_resname(wxT("APP_DARK_UNCHECKED_SVG"), wxT("APP_DARK_UNCHECKED_SVG_DARK")), {16, 16} ) );
	#elif (defined( __WXGTK__ ) || defined( __WXOSX__ ))
	m_appearance_dark->SetBitmap( wxBitmapBundle::FromSVGResource( pick_resname(wxT("APP_DARK_CHECKED_SVG"), wxT("APP_DARK_CHECKED_SVG_DARK")), {16, 16} ) );
	#endif
	m_view_appearance->Append( m_appearance_dark );

	m_menu_view->Append( m_view_appearanceItem );

	wxMenuItem* m_view_zebra;
	m_view_zebra = new wxMenuItem( m_menu_view, wxID_ANY, wxString( L"隔行变色" ) , L"设备列表隔行显示不同颜色", wxITEM_CHECK );
	m_menu_view->Append( m_view_zebra );

	wxMenuItem* m_view_labels;
	m_view_labels = new wxMenuItem( m_menu_view, wxID_ANY, wxString( L"显示工具栏文字" ) , wxEmptyString, wxITEM_CHECK );
	m_menu_view->Append( m_view_labels );
	m_view_labels->Check( true );

	m_menu_view->AppendSeparator();

	wxMenuItem* m_view_font_increase;
	m_view_font_increase = new wxMenuItem( m_menu_view, wxID_ZOOM_IN, wxString( L"增大字体" ) + wxT('\t') + wxT("CTRL++"), L"也可按住 Ctrl+滚轮", wxITEM_NORMAL );
	#ifdef __WXMSW__
	m_view_font_increase->SetBitmaps( wxBitmapBundle::FromSVGResource( pick_resname(wxT("TEXT_INCREASE_SVG"), wxT("TEXT_INCREASE_SVG_DARK")), {16, 16} ), wxNullBitmap );
	#elif (defined( __WXGTK__ ) || defined( __WXOSX__ ))
	m_view_font_increase->SetBitmap( wxBitmapBundle::FromSVGResource( pick_resname(wxT("TEXT_INCREASE_SVG"), wxT("TEXT_INCREASE_SVG_DARK")), {16, 16} ) );
	#endif
	m_menu_view->Append( m_view_font_increase );

	wxMenuItem* m_view_font_decrease;
	m_view_font_decrease = new wxMenuItem( m_menu_view, wxID_ZOOM_OUT, wxString( L"减小字体" ) + wxT('\t') + wxT("CTRL+-"), L"也可按住 Ctrl+滚轮", wxITEM_NORMAL );
	#ifdef __WXMSW__
	m_view_font_decrease->SetBitmaps( wxBitmapBundle::FromSVGResource( pick_resname(wxT("TEXT_DECREASE_SVG"), wxT("TEXT_DECREASE_SVG_DARK")), {16, 16} ) );
	#elif (defined( __WXGTK__ ) || defined( __WXOSX__ ))
	m_view_font_decrease->SetBitmap( wxBitmapBundle::FromSVGResource( pick_resname(wxT("TEXT_DECREASE_SVG"), wxT("TEXT_DECREASE_SVG_DARK")), {16, 16} ) );
	#endif
	m_menu_view->Append( m_view_font_decrease );

	wxMenuItem* m_view_font_default;
	m_view_font_default = new wxMenuItem( m_menu_view, wxID_ZOOM_100, wxString( L"默认字体" ) + wxT('\t') + wxT("CTRL+0"), L"恢复默认字体", wxITEM_NORMAL );
	#ifdef __WXMSW__
	m_view_font_default->SetBitmaps( wxNullBitmap, wxNullBitmap );
	#elif (defined( __WXGTK__ ) || defined( __WXOSX__ ))
	m_view_font_default->SetBitmap( wxNullBitmap );
	#endif
	m_menu_view->Append( m_view_font_default );

	m_menu_view->AppendSeparator();

	wxMenuItem* m_view_reset;
	m_view_reset = new wxMenuItem( m_menu_view, wxID_ANY, wxString( L"重置设置" ) , L"重置全部设置", wxITEM_NORMAL );
	#ifdef __WXMSW__
	m_view_reset->SetBitmaps( wxBitmapBundle::FromSVGResource( pick_resname(wxT("replay_svg"), wxT("replay_svg_dark")), {16, 16} ) );
	#elif (defined( __WXGTK__ ) || defined( __WXOSX__ ))
	m_view_reset->SetBitmap( wxBitmapBundle::FromSVGResource( pick_resname(wxT("replay_svg"), wxT("replay_svg_dark")), {16, 16} ) );
	#endif
	m_menu_view->Append( m_view_reset );

	m_menubar->Append( m_menu_view, L"视图" );

	m_menu_devices = new wxMenu();
	wxMenuItem* m_cmd_reload;
	m_cmd_reload = new wxMenuItem( m_menu_devices, wxID_REFRESH, wxString( L"刷新" ) + wxT('\t') + wxT("CTRL+R"), L"显示已连接的设备", wxITEM_NORMAL );
	#ifdef __WXMSW__
	m_cmd_reload->SetBitmaps( wxBitmapBundle::FromSVGResource( pick_resname(wxT("refresh_svg"), wxT("refresh_svg_dark")), {16, 16} ) );
	#elif (defined( __WXGTK__ ) || defined( __WXOSX__ ))
	m_cmd_reload->SetBitmap( wxBitmapBundle::FromSVGResource( pick_resname(wxT("refresh_svg"), wxT("refresh_svg_dark")), {16, 16} ) );
	#endif
	m_menu_devices->Append( m_cmd_reload );

	wxMenuItem* m_cmd_add;
	m_cmd_add = new wxMenuItem( m_menu_devices, wxID_ANY, wxString( L"添加" ) + wxT('\t') + wxT("CTRL+I"), L"添加远程设备", wxITEM_NORMAL );
	#ifdef __WXMSW__
	m_cmd_add->SetBitmaps( wxBitmapBundle::FromSVGResource( pick_resname(wxT("add_svg"), wxT("add_svg_dark")), {16, 16} ), wxNullBitmap );
	#elif (defined( __WXGTK__ ) || defined( __WXOSX__ ))
	m_cmd_add->SetBitmap( wxBitmapBundle::FromSVGResource( pick_resname(wxT("add_svg"), wxT("add_svg_dark")), {16, 16} ) );
	#endif
	m_menu_devices->Append( m_cmd_add );

	m_menu_devices->AppendSeparator();

	wxMenuItem* m_cmd_attach;
	m_cmd_attach = new wxMenuItem( m_menu_devices, ID_ATTACH, wxString( L"连接" ) + wxT('\t') + wxT("CTRL+T"), L"连接选中的设备", wxITEM_NORMAL );
	#ifdef __WXMSW__
	m_cmd_attach->SetBitmaps( wxBitmapBundle::FromSVGResource( pick_resname(wxT("check_svg"), wxT("check_svg_dark")), {16, 16} ) );
	#elif (defined( __WXGTK__ ) || defined( __WXOSX__ ))
	m_cmd_attach->SetBitmap( wxBitmapBundle::FromSVGResource( pick_resname(wxT("check_svg"), wxT("check_svg_dark")), {16, 16} ) );
	#endif
	m_menu_devices->Append( m_cmd_attach );

	wxMenuItem* m_cmd_attach_once;
	m_cmd_attach_once = new wxMenuItem( m_menu_devices, ID_ATTACH_ONCE, wxString( L"连接一次" ) , L"连接所选设备并停用自动连接", wxITEM_NORMAL );
	#ifdef __WXMSW__
	m_cmd_attach_once->SetBitmaps( wxNullBitmap );
	#elif (defined( __WXGTK__ ) || defined( __WXOSX__ ))
	m_cmd_attach_once->SetBitmap( wxNullBitmap );
	#endif
	m_menu_devices->Append( m_cmd_attach_once );

	m_menu_devices->AppendSeparator();

	wxMenuItem* m_cmd_attach_stop;
	m_cmd_attach_stop = new wxMenuItem( m_menu_devices, ID_ATTACH_STOP, wxString( L"停止" ) + wxT('\t') + wxT("ALT+T"), L"停止对选中设备的连接尝试", wxITEM_NORMAL );
	#ifdef __WXMSW__
	m_cmd_attach_stop->SetBitmaps( wxBitmapBundle::FromSVGResource( pick_resname(wxT("attach_stop_svg"), wxT("attach_stop_svg_dark")), {16, 16} ) );
	#elif (defined( __WXGTK__ ) || defined( __WXOSX__ ))
	m_cmd_attach_stop->SetBitmap( wxBitmapBundle::FromSVGResource( pick_resname(wxT("attach_stop_svg"), wxT("attach_stop_svg_dark")), {16, 16} ) );
	#endif
	m_menu_devices->Append( m_cmd_attach_stop );

	wxMenuItem* m_cmd_attach_stop_all;
	m_cmd_attach_stop_all = new wxMenuItem( m_menu_devices, ID_ATTACH_STOP_ALL, wxString( L"全部停止" ) , L"停止全部进行中的连接尝试", wxITEM_NORMAL );
	#ifdef __WXMSW__
	m_cmd_attach_stop_all->SetBitmaps( wxBitmapBundle::FromSVGResource( pick_resname(wxT("attach_stop_all_svg"), wxT("attach_stop_all_svg_dark")), {16, 16} ) );
	#elif (defined( __WXGTK__ ) || defined( __WXOSX__ ))
	m_cmd_attach_stop_all->SetBitmap( wxBitmapBundle::FromSVGResource( pick_resname(wxT("attach_stop_all_svg"), wxT("attach_stop_all_svg_dark")), {16, 16} ) );
	#endif
	m_menu_devices->Append( m_cmd_attach_stop_all );

	m_menu_devices->AppendSeparator();

	wxMenuItem* m_cmd_detach;
	m_cmd_detach = new wxMenuItem( m_menu_devices, wxID_CLOSE, wxString( L"断开" ) + wxT('\t') + wxT("CTRL+D"), L"断开选中的设备", wxITEM_NORMAL );
	#ifdef __WXMSW__
	m_cmd_detach->SetBitmaps( wxBitmapBundle::FromSVGResource( pick_resname(wxT("close_svg"), wxT("close_svg_dark")), {16, 16} ) );
	#elif (defined( __WXGTK__ ) || defined( __WXOSX__ ))
	m_cmd_detach->SetBitmap( wxBitmapBundle::FromSVGResource( pick_resname(wxT("close_svg"), wxT("close_svg_dark")), {16, 16} ) );
	#endif
	m_menu_devices->Append( m_cmd_detach );

	wxMenuItem* m_cmd_detach_all;
	m_cmd_detach_all = new wxMenuItem( m_menu_devices, wxID_CLOSE_ALL, wxString( L"全部断开" ) + wxT('\t') + wxT("CTRL+X"), L"断开全部设备", wxITEM_NORMAL );
	#ifdef __WXMSW__
	m_cmd_detach_all->SetBitmaps( wxBitmapBundle::FromSVGResource( wxT("dismiss_svg"), {16, 16} ) );
	#elif (defined( __WXGTK__ ) || defined( __WXOSX__ ))
	m_cmd_detach_all->SetBitmap( wxBitmapBundle::FromSVGResource( wxT("dismiss_svg"), {16, 16} ) );
	#endif
	m_menu_devices->Append( m_cmd_detach_all );

	m_menu_devices->AppendSeparator();

	/*
	 * 主列表里的服务器节点是"每次读取设备时累积"出来的(读取 A 再读取 B,
	 * 树里就同时留着 A 和 B 两个服务器)。以前没有任何入口能主动删掉其中一个,
	 * 只能等它下面的设备被全部移除后节点才自动消失 —— 用户换过一次地址
	 * (域名 vs 局域网 IP)就会永久留下两条重复服务器。这里补一个显式移除命令。
	 */
	wxMenuItem* m_cmd_remove_server;
	m_cmd_remove_server = new wxMenuItem( m_menu_devices, ID_REMOVE_SERVER, wxString( L"移除服务器" ) , L"从列表中移除选中的服务器及其设备(不影响服务器本身)", wxITEM_NORMAL );
	#ifdef __WXMSW__
	m_cmd_remove_server->SetBitmaps( wxBitmapBundle::FromSVGResource( wxT("dismiss_svg"), {16, 16} ) );
	#elif (defined( __WXGTK__ ) || defined( __WXOSX__ ))
	m_cmd_remove_server->SetBitmap( wxBitmapBundle::FromSVGResource( wxT("dismiss_svg"), {16, 16} ) );
	#endif
	m_menu_devices->Append( m_cmd_remove_server );

	m_menubar->Append( m_menu_devices, L"设备" );

	m_menu_log = new wxMenu();
	wxMenuItem* m_log_toggle;
	m_log_toggle = new wxMenuItem( m_menu_log, ID_TOGGLE_LOG_WINDOW, wxString( L"显示/隐藏窗口" ) + wxT('\t') + wxT("CTRL+W"), L"显示/隐藏日志窗口", wxITEM_CHECK );
	#ifdef __WXMSW__
	m_log_toggle->SetBitmaps( wxNullBitmap );
	#elif (defined( __WXGTK__ ) || defined( __WXOSX__ ))
	m_log_toggle->SetBitmap( wxNullBitmap );
	#endif
	m_menu_log->Append( m_log_toggle );

	wxMenuItem* m_log_verbose;
	m_log_verbose = new wxMenuItem( m_menu_log, wxID_ANY, wxString( L"详细日志" ) , L"在日志窗口显示调试信息", wxITEM_CHECK );
	#ifdef __WXMSW__
	m_log_verbose->SetBitmaps( wxNullBitmap );
	#elif (defined( __WXGTK__ ) || defined( __WXOSX__ ))
	m_log_verbose->SetBitmap( wxNullBitmap );
	#endif
	m_menu_log->Append( m_log_verbose );

	wxMenuItem* m_log_library;
	m_log_library = new wxMenuItem( m_menu_log, wxID_ANY, wxString( L"库日志" ) , L"显示 libusbip 库的调试信息", wxITEM_CHECK );
	#ifdef __WXMSW__
	m_log_library->SetBitmaps( wxNullBitmap );
	#elif (defined( __WXGTK__ ) || defined( __WXOSX__ ))
	m_log_library->SetBitmap( wxNullBitmap );
	#endif
	m_menu_log->Append( m_log_library );

	m_menubar->Append( m_menu_log, L"日志" );

	m_menu_help = new wxMenu();
	wxMenuItem* m_help_about;
	m_help_about = new wxMenuItem( m_menu_help, wxID_ABOUT, wxString( L"关于" ) , wxEmptyString, wxITEM_NORMAL );
	#ifdef __WXMSW__
	m_help_about->SetBitmaps( wxBitmapBundle::FromSVGResource( pick_resname(wxT("info_svg"), wxT("info_svg_dark")), {16, 16} ) );
	#elif (defined( __WXGTK__ ) || defined( __WXOSX__ ))
	m_help_about->SetBitmap( wxBitmapBundle::FromSVGResource( pick_resname(wxT("info_svg"), wxT("info_svg_dark")), {16, 16} ) );
	#endif
	m_menu_help->Append( m_help_about );

	wxMenuItem* m_help_about_lib;
	m_help_about_lib = new wxMenuItem( m_menu_help, wxID_ANY, wxString( L"wxWidgets 库" ) , L"显示所使用第三方库的信息", wxITEM_NORMAL );
	#ifdef __WXMSW__
	m_help_about_lib->SetBitmaps( wxNullBitmap );
	#elif (defined( __WXGTK__ ) || defined( __WXOSX__ ))
	m_help_about_lib->SetBitmap( wxNullBitmap );
	#endif
	m_menu_help->Append( m_help_about_lib );

	m_menubar->Append( m_menu_help, L"帮助" );

	this->SetMenuBar( m_menubar );

	m_auiToolBar = new wxAuiToolBar( this, wxID_ANY, wxDefaultPosition, wxDefaultSize, wxAUI_TB_DEFAULT_STYLE|wxAUI_TB_GRIPPER|wxAUI_TB_TEXT );
	m_auiToolBar->SetToolSeparation( 1 );
	m_tool_reload = m_auiToolBar->AddTool( wxID_REFRESH, L"刷新", wxBitmapBundle::FromSVGResource( pick_resname(wxT("refresh_svg"), wxT("refresh_svg_dark")), {24, 24} ), wxNullBitmap, wxITEM_NORMAL, L"显示已连接的设备", wxEmptyString, NULL );

	m_auiToolBar->AddSeparator();

	m_tool_attach = m_auiToolBar->AddTool( wxID_ANY, L"连接", wxBitmapBundle::FromSVGResource( pick_resname(wxT("check_svg"), wxT("check_svg_dark")), {24, 24} ), wxNullBitmap, wxITEM_NORMAL, L"连接选中的设备", wxEmptyString, NULL );

	m_tool_attach_stop = m_auiToolBar->AddTool( wxID_ANY, L"停止", wxBitmapBundle::FromSVGResource( pick_resname(wxT("attach_stop_svg"), wxT("attach_stop_svg_dark")), {24, 24} ), wxNullBitmap, wxITEM_NORMAL, L"停止对选中设备的连接尝试", wxEmptyString, NULL );

	m_tool_attach_stop_all = m_auiToolBar->AddTool( wxID_ANY, L"全部停止", wxBitmapBundle::FromSVGResource( pick_resname(wxT("attach_stop_all_svg"), wxT("attach_stop_all_svg_dark")), {24, 24} ), wxNullBitmap, wxITEM_NORMAL, L"停止全部进行中的连接尝试", wxEmptyString, NULL );

	m_auiToolBar->AddSeparator();

	m_tool_detach = m_auiToolBar->AddTool( wxID_ANY, L"断开", wxBitmapBundle::FromSVGResource( pick_resname(wxT("close_svg"), wxT("close_svg_dark")), {24, 24} ), wxNullBitmap, wxITEM_NORMAL, L"断开选中的设备", wxEmptyString, NULL );

	m_tool_detach_all = m_auiToolBar->AddTool( wxID_ANY, L"全部断开", wxBitmapBundle::FromSVGResource( wxT("dismiss_svg"), {24, 24} ), wxNullBitmap, wxITEM_NORMAL, L"断开全部设备", wxEmptyString, NULL );

	m_auiToolBar->AddSeparator();

	m_tool_load = m_auiToolBar->AddTool( wxID_ANY, L"加载", wxBitmapBundle::FromSVGResource( pick_resname(wxT("save_alt_svg"), wxT("save_alt_svg_dark")), {24, 24} ), wxNullBitmap, wxITEM_NORMAL, L"加载已保存的设备", wxEmptyString, NULL );

	m_tool_save = m_auiToolBar->AddTool( wxID_SAVE, L"保存", wxBitmapBundle::FromSVGResource( pick_resname(wxT("save_svg"), wxT("save_svg_dark")), {24, 24} ), wxNullBitmap, wxITEM_NORMAL, L"保存设备", wxEmptyString, NULL );

	m_auiToolBar->Realize();
	m_mgr.AddPane( m_auiToolBar, wxAuiPaneInfo() .Top() .CaptionVisible( false ).CloseButton( false ).Gripper().Dock().Resizable().FloatingSize( wxSize( -1,-1 ) ).Floatable( false ).Layer( 10 ).ToolbarPane() );

	m_auiToolBarAdd = new wxAuiToolBar( this, wxID_ANY, wxDefaultPosition, wxDefaultSize, wxAUI_TB_DEFAULT_STYLE|wxAUI_TB_GRIPPER|wxTAB_TRAVERSAL );
	m_auiToolBarAdd->SetToolSeparation( 1 );
	m_staticTextServer = new wxStaticText( m_auiToolBarAdd, wxID_ANY, L"服务器", wxDefaultPosition, wxDefaultSize, wxALIGN_CENTER_HORIZONTAL );
	m_staticTextServer->Wrap( -1 );
	m_staticTextServer->SetToolTip( L"USB-SHARE 服务端主机名或 IP 地址" );

	m_auiToolBarAdd->AddControl( m_staticTextServer );
	m_comboBoxServer = new wxComboBox( m_auiToolBarAdd, wxID_ANY, wxEmptyString, wxDefaultPosition, wxDefaultSize, 0, NULL, wxCB_DROPDOWN|wxCB_SORT );
	m_comboBoxServer->SetToolTip( L"主机名或 IP 地址" );

	m_auiToolBarAdd->AddControl( m_comboBoxServer );
	m_staticTextPort = new wxStaticText( m_auiToolBarAdd, wxID_ANY, L"端口", wxDefaultPosition, wxDefaultSize, wxALIGN_CENTER_HORIZONTAL );
	m_staticTextPort->Wrap( -1 );
	m_staticTextPort->SetToolTip( L"TCP/IP 端口号" );

	m_auiToolBarAdd->AddControl( m_staticTextPort );
	m_spinCtrlPort = new wxSpinCtrl( m_auiToolBarAdd, wxID_ANY, wxEmptyString, wxDefaultPosition, wxDefaultSize, wxALIGN_RIGHT|wxSP_ARROW_KEYS|wxSP_WRAP, 1025, 65535, 0 );
	m_spinCtrlPort->SetToolTip( L"TCP/IP 端口号" );

	m_auiToolBarAdd->AddControl( m_spinCtrlPort );
	m_button_add = new wxButton( m_auiToolBarAdd, wxID_ADD, L"添加设备", wxDefaultPosition, wxDefaultSize, 0 );

	m_button_add->SetDefault();

	m_button_add->SetBitmap( wxBitmapBundle::FromSVGResource( pick_resname(wxT("add_svg"), wxT("add_svg_dark")), {24, 24} ) );
	m_auiToolBarAdd->AddControl( m_button_add );
	m_auiToolBarAdd->Realize();
	m_mgr.AddPane( m_auiToolBarAdd, wxAuiPaneInfo() .Top() .CaptionVisible( false ).CloseButton( false ).Gripper().Dock().Resizable().FloatingSize( wxSize( 137,137 ) ).Floatable( false ).Layer( 10 ).ToolbarPane() );

	m_treeListCtrl = new wxTreeListCtrl( this, wxID_ANY, wxDefaultPosition, wxDefaultSize, wxTL_MULTIPLE );
	m_treeListCtrl->SetFont( wxFont( wxNORMAL_FONT->GetPointSize(), wxFONTFAMILY_DEFAULT, wxFONTSTYLE_NORMAL, wxFONTWEIGHT_NORMAL, false, wxEmptyString ) );
	m_treeListCtrl->SetBackgroundColour( wxSystemSettings::GetColour( wxSYS_COLOUR_WINDOW ) );

	m_mgr.AddPane( m_treeListCtrl, wxAuiPaneInfo() .Center() .Caption( L"USB 设备" ).CloseButton( false ).PaneBorder( false ).Movable( false ).Dock().Resizable().FloatingSize( wxDefaultSize ).DockFixed( true ).BottomDockable( false ).TopDockable( false ).LeftDockable( false ).RightDockable( false ).Floatable( false ) );

	m_treeListCtrl->AppendColumn( L"服务器 / 总线号", wxCOL_WIDTH_AUTOSIZE, wxALIGN_LEFT, wxCOL_REORDERABLE|wxCOL_RESIZABLE|wxCOL_SORTABLE );
	m_treeListCtrl->AppendColumn( L"端口", wxCOL_WIDTH_AUTOSIZE, wxALIGN_CENTER, wxCOL_REORDERABLE|wxCOL_RESIZABLE|wxCOL_SORTABLE );
	m_treeListCtrl->AppendColumn( L"速率", wxCOL_WIDTH_AUTOSIZE, wxALIGN_LEFT, wxCOL_REORDERABLE|wxCOL_RESIZABLE|wxCOL_SORTABLE );
	m_treeListCtrl->AppendColumn( L"厂商", wxCOL_WIDTH_AUTOSIZE, wxALIGN_LEFT, wxCOL_REORDERABLE|wxCOL_RESIZABLE|wxCOL_SORTABLE );
	m_treeListCtrl->AppendColumn( L"产品", wxCOL_WIDTH_AUTOSIZE, wxALIGN_LEFT, wxCOL_REORDERABLE|wxCOL_RESIZABLE|wxCOL_SORTABLE );
	m_treeListCtrl->AppendColumn( L"状态", wxCOL_WIDTH_AUTOSIZE, wxALIGN_CENTER, wxCOL_REORDERABLE|wxCOL_RESIZABLE|wxCOL_SORTABLE );
	m_treeListCtrl->AppendColumn( L"自动", wxCOL_WIDTH_AUTOSIZE, wxALIGN_CENTER, wxCOL_REORDERABLE|wxCOL_RESIZABLE|wxCOL_SORTABLE );
	m_treeListCtrl->AppendColumn( L"备注", wxCOL_WIDTH_AUTOSIZE, wxALIGN_LEFT, wxCOL_REORDERABLE|wxCOL_RESIZABLE|wxCOL_SORTABLE );
	m_treeListCtrl->AppendColumn( L"设备ID", wxCOL_WIDTH_AUTOSIZE, wxALIGN_CENTER, wxCOL_HIDDEN|wxCOL_REORDERABLE|wxCOL_RESIZABLE|wxCOL_SORTABLE );
	m_treeListCtrl->AppendColumn( L"序列号", wxCOL_WIDTH_AUTOSIZE, wxALIGN_LEFT, wxCOL_REORDERABLE|wxCOL_RESIZABLE|wxCOL_SORTABLE );
	m_treeListCtrl->AppendColumn( L"接收模式", wxCOL_WIDTH_AUTOSIZE, wxALIGN_LEFT, wxCOL_REORDERABLE|wxCOL_RESIZABLE|wxCOL_SORTABLE );
	m_treeListCtrl->AppendColumn( L"源ID", wxCOL_WIDTH_AUTOSIZE, wxALIGN_CENTER, wxCOL_HIDDEN );
	m_treeListCtrl->AppendColumn( L"服务器备注", wxCOL_WIDTH_AUTOSIZE, wxALIGN_LEFT, wxCOL_REORDERABLE|wxCOL_RESIZABLE );
	m_treeListCtrl->AppendColumn( L"占用方", wxCOL_WIDTH_AUTOSIZE, wxALIGN_LEFT, wxCOL_REORDERABLE|wxCOL_RESIZABLE );
	m_treeListCtrl->AppendColumn( L"排队等待", wxCOL_WIDTH_AUTOSIZE, wxALIGN_LEFT, wxCOL_REORDERABLE|wxCOL_RESIZABLE );


	m_mgr.Update();
	this->Centre( wxBOTH );

	// Connect Events
	this->Connect( wxEVT_CLOSE_WINDOW, wxCloseEventHandler( Frame::on_close ) );
	this->Connect( wxEVT_MOUSEWHEEL, wxMouseEventHandler( Frame::on_frame_mouse_wheel ) );
	m_menu_file->Bind(wxEVT_COMMAND_MENU_SELECTED, wxCommandEventHandler( Frame::on_load ), this, m_file_load->GetId());
	m_menu_file->Bind(wxEVT_COMMAND_MENU_SELECTED, wxCommandEventHandler( Frame::on_save ), this, m_file_save->GetId());
	m_menu_file->Bind(wxEVT_COMMAND_MENU_SELECTED, wxCommandEventHandler( Frame::on_save_selected ), this, m_file_save_selected->GetId());
	this->Connect( m_file_save_selected->GetId(), wxEVT_UPDATE_UI, wxUpdateUIEventHandler( Frame::on_has_selected_devices_update_ui ) );
	m_menu_file->Bind(wxEVT_COMMAND_MENU_SELECTED, wxCommandEventHandler( Frame::on_start_in_tray ), this, m_file_start_in_tray->GetId());
	this->Connect( m_file_start_in_tray->GetId(), wxEVT_UPDATE_UI, wxUpdateUIEventHandler( Frame::on_start_in_tray_update_ui ) );
	m_menu_file->Bind(wxEVT_COMMAND_MENU_SELECTED, wxCommandEventHandler( Frame::on_close_to_tray ), this, m_file_close_to_tray->GetId());
	this->Connect( m_file_close_to_tray->GetId(), wxEVT_UPDATE_UI, wxUpdateUIEventHandler( Frame::on_close_to_tray_update_ui ) );
	m_menu_file->Bind(wxEVT_COMMAND_MENU_SELECTED, wxCommandEventHandler( Frame::on_exit ), this, m_file_exit->GetId());
	m_menu_edit->Bind(wxEVT_COMMAND_MENU_SELECTED, wxCommandEventHandler( Frame::on_select_all ), this, m_select_all->GetId());
	this->Connect( m_select_all->GetId(), wxEVT_UPDATE_UI, wxUpdateUIEventHandler( Frame::on_has_devices_update_ui ) );
	m_menu_edit->Bind(wxEVT_COMMAND_MENU_SELECTED, wxCommandEventHandler( Frame::on_copy_rows ), this, m_copy_rows->GetId());
	this->Connect( m_copy_rows->GetId(), wxEVT_UPDATE_UI, wxUpdateUIEventHandler( Frame::on_has_any_selected_devices_update_ui ) );
	m_menu_edit->Bind(wxEVT_COMMAND_MENU_SELECTED, wxCommandEventHandler( Frame::on_edit_notes ), this, m_edit_notes->GetId());
	this->Connect( m_edit_notes->GetId(), wxEVT_UPDATE_UI, wxUpdateUIEventHandler( Frame::on_edit_device_update_ui ) );
	m_menu_edit->Bind(wxEVT_COMMAND_MENU_SELECTED, wxCommandEventHandler( Frame::on_flip_auto ), this, m_flip_auto->GetId());
	this->Connect( m_flip_auto->GetId(), wxEVT_UPDATE_UI, wxUpdateUIEventHandler( Frame::on_has_selected_devices_update_ui ) );
	m_menu_edit->Bind(wxEVT_COMMAND_MENU_SELECTED, wxCommandEventHandler( Frame::on_flip_receive_mode ), this, m_flip_receive_mode->GetId());
	this->Connect( m_flip_receive_mode->GetId(), wxEVT_UPDATE_UI, wxUpdateUIEventHandler( Frame::on_has_selected_devices_update_ui ) );
	m_menu_edit->Bind(wxEVT_COMMAND_MENU_SELECTED, wxCommandEventHandler( Frame::on_edit_serial ), this, m_edit_serial->GetId());
	this->Connect( m_edit_serial->GetId(), wxEVT_UPDATE_UI, wxUpdateUIEventHandler( Frame::on_edit_device_update_ui ) );
	m_menu_edit->Bind(wxEVT_COMMAND_MENU_SELECTED, wxCommandEventHandler( Frame::on_edit_gen_serial ), this, m_edit_gen_serial->GetId());
	this->Connect( m_edit_gen_serial->GetId(), wxEVT_UPDATE_UI, wxUpdateUIEventHandler( Frame::on_edit_device_update_ui ) );
	m_menu_columns->Bind(wxEVT_COMMAND_MENU_SELECTED, wxCommandEventHandler( Frame::on_view_column ), this, m_view_busid->GetId());
	this->Connect( m_view_busid->GetId(), wxEVT_UPDATE_UI, wxUpdateUIEventHandler( Frame::on_view_column_update_ui ) );
	m_menu_columns->Bind(wxEVT_COMMAND_MENU_SELECTED, wxCommandEventHandler( Frame::on_view_column ), this, m_view_port->GetId());
	this->Connect( m_view_port->GetId(), wxEVT_UPDATE_UI, wxUpdateUIEventHandler( Frame::on_view_column_update_ui ) );
	m_menu_columns->Bind(wxEVT_COMMAND_MENU_SELECTED, wxCommandEventHandler( Frame::on_view_column ), this, m_view_speed->GetId());
	this->Connect( m_view_speed->GetId(), wxEVT_UPDATE_UI, wxUpdateUIEventHandler( Frame::on_view_column_update_ui ) );
	m_menu_columns->Bind(wxEVT_COMMAND_MENU_SELECTED, wxCommandEventHandler( Frame::on_view_column ), this, m_view_vendor->GetId());
	this->Connect( m_view_vendor->GetId(), wxEVT_UPDATE_UI, wxUpdateUIEventHandler( Frame::on_view_column_update_ui ) );
	m_menu_columns->Bind(wxEVT_COMMAND_MENU_SELECTED, wxCommandEventHandler( Frame::on_view_column ), this, m_view_product->GetId());
	this->Connect( m_view_product->GetId(), wxEVT_UPDATE_UI, wxUpdateUIEventHandler( Frame::on_view_column_update_ui ) );
	m_menu_columns->Bind(wxEVT_COMMAND_MENU_SELECTED, wxCommandEventHandler( Frame::on_view_column ), this, m_view_state->GetId());
	this->Connect( m_view_state->GetId(), wxEVT_UPDATE_UI, wxUpdateUIEventHandler( Frame::on_view_column_update_ui ) );
	m_menu_columns->Bind(wxEVT_COMMAND_MENU_SELECTED, wxCommandEventHandler( Frame::on_view_column ), this, m_view_persistent->GetId());
	this->Connect( m_view_persistent->GetId(), wxEVT_UPDATE_UI, wxUpdateUIEventHandler( Frame::on_view_column_update_ui ) );
	m_menu_columns->Bind(wxEVT_COMMAND_MENU_SELECTED, wxCommandEventHandler( Frame::on_view_column ), this, m_view_notes->GetId());
	this->Connect( m_view_notes->GetId(), wxEVT_UPDATE_UI, wxUpdateUIEventHandler( Frame::on_view_column_update_ui ) );
	m_menu_columns->Bind(wxEVT_COMMAND_MENU_SELECTED, wxCommandEventHandler( Frame::on_view_column ), this, m_view_devid->GetId());
	this->Connect( m_view_devid->GetId(), wxEVT_UPDATE_UI, wxUpdateUIEventHandler( Frame::on_view_column_update_ui ) );
	m_menu_columns->Bind(wxEVT_COMMAND_MENU_SELECTED, wxCommandEventHandler( Frame::on_view_column ), this, m_view_serial->GetId());
	this->Connect( m_view_serial->GetId(), wxEVT_UPDATE_UI, wxUpdateUIEventHandler( Frame::on_view_column_update_ui ) );
	m_menu_columns->Bind(wxEVT_COMMAND_MENU_SELECTED, wxCommandEventHandler( Frame::on_view_column ), this, m_view_receive_mode->GetId());
	this->Connect( m_view_receive_mode->GetId(), wxEVT_UPDATE_UI, wxUpdateUIEventHandler( Frame::on_view_column_update_ui ) );
	m_menu_columns->Bind(wxEVT_COMMAND_MENU_SELECTED, wxCommandEventHandler( Frame::on_view_column ), this, m_view_source_id->GetId());
	this->Connect( m_view_source_id->GetId(), wxEVT_UPDATE_UI, wxUpdateUIEventHandler( Frame::on_view_column_update_ui ) );
	m_menu_columns->Bind(wxEVT_COMMAND_MENU_SELECTED, wxCommandEventHandler( Frame::on_view_column ), this, m_view_mgmt_remark->GetId());
	this->Connect( m_view_mgmt_remark->GetId(), wxEVT_UPDATE_UI, wxUpdateUIEventHandler( Frame::on_view_column_update_ui ) );
	m_menu_columns->Bind(wxEVT_COMMAND_MENU_SELECTED, wxCommandEventHandler( Frame::on_view_column ), this, m_view_mgmt_owner->GetId());
	this->Connect( m_view_mgmt_owner->GetId(), wxEVT_UPDATE_UI, wxUpdateUIEventHandler( Frame::on_view_column_update_ui ) );
	m_menu_columns->Bind(wxEVT_COMMAND_MENU_SELECTED, wxCommandEventHandler( Frame::on_view_column ), this, m_view_mgmt_queue->GetId());
	this->Connect( m_view_mgmt_queue->GetId(), wxEVT_UPDATE_UI, wxUpdateUIEventHandler( Frame::on_view_column_update_ui ) );
	m_view_appearance->Bind(wxEVT_COMMAND_MENU_SELECTED, wxCommandEventHandler( Frame::on_view_appearance ), this, m_appearance_system->GetId());
	m_view_appearance->Bind(wxEVT_COMMAND_MENU_SELECTED, wxCommandEventHandler( Frame::on_view_appearance ), this, m_appearance_light->GetId());
	m_view_appearance->Bind(wxEVT_COMMAND_MENU_SELECTED, wxCommandEventHandler( Frame::on_view_appearance ), this, m_appearance_dark->GetId());
	m_menu_view->Bind(wxEVT_COMMAND_MENU_SELECTED, wxCommandEventHandler( Frame::on_view_zebra ), this, m_view_zebra->GetId());
	this->Connect( m_view_zebra->GetId(), wxEVT_UPDATE_UI, wxUpdateUIEventHandler( Frame::on_view_zebra_update_ui ) );
	m_menu_view->Bind(wxEVT_COMMAND_MENU_SELECTED, wxCommandEventHandler( Frame::on_view_labels ), this, m_view_labels->GetId());
	this->Connect( m_view_labels->GetId(), wxEVT_UPDATE_UI, wxUpdateUIEventHandler( Frame::on_view_labels_update_ui ) );
	m_menu_view->Bind(wxEVT_COMMAND_MENU_SELECTED, wxCommandEventHandler( Frame::on_view_font_increase ), this, m_view_font_increase->GetId());
	m_menu_view->Bind(wxEVT_COMMAND_MENU_SELECTED, wxCommandEventHandler( Frame::on_view_font_decrease ), this, m_view_font_decrease->GetId());
	m_menu_view->Bind(wxEVT_COMMAND_MENU_SELECTED, wxCommandEventHandler( Frame::on_view_font_default ), this, m_view_font_default->GetId());
	m_menu_view->Bind(wxEVT_COMMAND_MENU_SELECTED, wxCommandEventHandler( Frame::on_view_reset ), this, m_view_reset->GetId());
	m_menu_devices->Bind(wxEVT_COMMAND_MENU_SELECTED, wxCommandEventHandler( Frame::on_reload ), this, m_cmd_reload->GetId());
	m_menu_devices->Bind(wxEVT_COMMAND_MENU_SELECTED, wxCommandEventHandler( Frame::add_exported_devices ), this, m_cmd_add->GetId());
	m_menu_devices->Bind(wxEVT_COMMAND_MENU_SELECTED, wxCommandEventHandler( Frame::on_attach ), this, m_cmd_attach->GetId());
	this->Connect( m_cmd_attach->GetId(), wxEVT_UPDATE_UI, wxUpdateUIEventHandler( Frame::on_has_any_selected_devices_update_ui ) );
	m_menu_devices->Bind(wxEVT_COMMAND_MENU_SELECTED, wxCommandEventHandler( Frame::on_attach_once ), this, m_cmd_attach_once->GetId());
	this->Connect( m_cmd_attach_once->GetId(), wxEVT_UPDATE_UI, wxUpdateUIEventHandler( Frame::on_has_any_selected_devices_update_ui ) );
	m_menu_devices->Bind(wxEVT_COMMAND_MENU_SELECTED, wxCommandEventHandler( Frame::on_attach_stop ), this, m_cmd_attach_stop->GetId());
	this->Connect( m_cmd_attach_stop->GetId(), wxEVT_UPDATE_UI, wxUpdateUIEventHandler( Frame::on_has_any_selected_devices_update_ui ) );
	m_menu_devices->Bind(wxEVT_COMMAND_MENU_SELECTED, wxCommandEventHandler( Frame::on_attach_stop_all ), this, m_cmd_attach_stop_all->GetId());
	m_menu_devices->Bind(wxEVT_COMMAND_MENU_SELECTED, wxCommandEventHandler( Frame::on_detach ), this, m_cmd_detach->GetId());
	this->Connect( m_cmd_detach->GetId(), wxEVT_UPDATE_UI, wxUpdateUIEventHandler( Frame::on_has_selected_devices_update_ui ) );
	m_menu_devices->Bind(wxEVT_COMMAND_MENU_SELECTED, wxCommandEventHandler( Frame::on_detach_all ), this, m_cmd_detach_all->GetId());
	m_menu_devices->Bind(wxEVT_COMMAND_MENU_SELECTED, wxCommandEventHandler( Frame::on_remove_server ), this, m_cmd_remove_server->GetId());
	this->Connect( m_cmd_remove_server->GetId(), wxEVT_UPDATE_UI, wxUpdateUIEventHandler( Frame::on_has_any_selected_devices_update_ui ) );
	m_menu_log->Bind(wxEVT_COMMAND_MENU_SELECTED, wxCommandEventHandler( Frame::on_log_show ), this, m_log_toggle->GetId());
	this->Connect( m_log_toggle->GetId(), wxEVT_UPDATE_UI, wxUpdateUIEventHandler( Frame::on_log_show_update_ui ) );
	m_menu_log->Bind(wxEVT_COMMAND_MENU_SELECTED, wxCommandEventHandler( Frame::on_log_verbose ), this, m_log_verbose->GetId());
	this->Connect( m_log_verbose->GetId(), wxEVT_UPDATE_UI, wxUpdateUIEventHandler( Frame::on_log_verbose_update_ui ) );
	m_menu_log->Bind(wxEVT_COMMAND_MENU_SELECTED, wxCommandEventHandler( Frame::on_log_library ), this, m_log_library->GetId());
	this->Connect( m_log_library->GetId(), wxEVT_UPDATE_UI, wxUpdateUIEventHandler( Frame::on_log_library_update_ui ) );
	m_menu_help->Bind(wxEVT_COMMAND_MENU_SELECTED, wxCommandEventHandler( Frame::on_help_about ), this, m_help_about->GetId());
	m_menu_help->Bind(wxEVT_COMMAND_MENU_SELECTED, wxCommandEventHandler( Frame::on_help_about_lib ), this, m_help_about_lib->GetId());
	m_auiToolBar->Connect( wxEVT_MOUSEWHEEL, wxMouseEventHandler( Frame::on_frame_mouse_wheel ), NULL, this );
	this->Connect( m_tool_reload->GetId(), wxEVT_COMMAND_TOOL_CLICKED, wxCommandEventHandler( Frame::on_reload ) );
	this->Connect( m_tool_attach->GetId(), wxEVT_COMMAND_TOOL_CLICKED, wxCommandEventHandler( Frame::on_attach ) );
	this->Connect( m_tool_attach->GetId(), wxEVT_UPDATE_UI, wxUpdateUIEventHandler( Frame::on_has_any_selected_devices_update_ui ) );
	this->Connect( m_tool_attach_stop->GetId(), wxEVT_COMMAND_TOOL_CLICKED, wxCommandEventHandler( Frame::on_attach_stop ) );
	this->Connect( m_tool_attach_stop->GetId(), wxEVT_UPDATE_UI, wxUpdateUIEventHandler( Frame::on_has_any_selected_devices_update_ui ) );
	this->Connect( m_tool_attach_stop_all->GetId(), wxEVT_COMMAND_TOOL_CLICKED, wxCommandEventHandler( Frame::on_attach_stop_all ) );
	this->Connect( m_tool_detach->GetId(), wxEVT_COMMAND_TOOL_CLICKED, wxCommandEventHandler( Frame::on_detach ) );
	this->Connect( m_tool_detach->GetId(), wxEVT_UPDATE_UI, wxUpdateUIEventHandler( Frame::on_has_selected_devices_update_ui ) );
	this->Connect( m_tool_detach_all->GetId(), wxEVT_COMMAND_TOOL_CLICKED, wxCommandEventHandler( Frame::on_detach_all ) );
	this->Connect( m_tool_load->GetId(), wxEVT_COMMAND_TOOL_CLICKED, wxCommandEventHandler( Frame::on_load ) );
	this->Connect( m_tool_save->GetId(), wxEVT_COMMAND_TOOL_CLICKED, wxCommandEventHandler( Frame::on_save ) );
	m_auiToolBarAdd->Connect( wxEVT_MOUSEWHEEL, wxMouseEventHandler( Frame::on_frame_mouse_wheel ), NULL, this );
	m_button_add->Connect( wxEVT_COMMAND_BUTTON_CLICKED, wxCommandEventHandler( Frame::add_exported_devices ), NULL, this );
	m_treeListCtrl->Connect( wxEVT_TREELIST_ITEM_ACTIVATED, wxTreeListEventHandler( Frame::on_item_activated ), NULL, this );
	m_treeListCtrl->Connect( wxEVT_TREELIST_ITEM_CONTEXT_MENU, wxTreeListEventHandler( Frame::on_item_context_menu ), NULL, this );
}

Frame::~Frame()
{
	// Disconnect Events
	this->Disconnect( wxEVT_CLOSE_WINDOW, wxCloseEventHandler( Frame::on_close ) );
	this->Disconnect( wxEVT_MOUSEWHEEL, wxMouseEventHandler( Frame::on_frame_mouse_wheel ) );
	this->Disconnect( wxID_SAVEAS, wxEVT_UPDATE_UI, wxUpdateUIEventHandler( Frame::on_has_selected_devices_update_ui ) );
	this->Disconnect( wxID_ANY, wxEVT_UPDATE_UI, wxUpdateUIEventHandler( Frame::on_start_in_tray_update_ui ) );
	this->Disconnect( wxID_ANY, wxEVT_UPDATE_UI, wxUpdateUIEventHandler( Frame::on_close_to_tray_update_ui ) );
	this->Disconnect( wxID_SELECTALL, wxEVT_UPDATE_UI, wxUpdateUIEventHandler( Frame::on_has_devices_update_ui ) );
	this->Disconnect( wxID_COPY, wxEVT_UPDATE_UI, wxUpdateUIEventHandler( Frame::on_has_any_selected_devices_update_ui ) );
	this->Disconnect( ID_EDIT_NOTES, wxEVT_UPDATE_UI, wxUpdateUIEventHandler( Frame::on_edit_device_update_ui ) );
	this->Disconnect( ID_FLIP_AUTO, wxEVT_UPDATE_UI, wxUpdateUIEventHandler( Frame::on_has_selected_devices_update_ui ) );
	this->Disconnect( ID_FLIP_RECEIVE_MODE, wxEVT_UPDATE_UI, wxUpdateUIEventHandler( Frame::on_has_selected_devices_update_ui ) );
	this->Disconnect( ID_EDIT_SERIAL, wxEVT_UPDATE_UI, wxUpdateUIEventHandler( Frame::on_edit_device_update_ui ) );
	this->Disconnect( ID_EDIT_GEN_SERIAL, wxEVT_UPDATE_UI, wxUpdateUIEventHandler( Frame::on_edit_device_update_ui ) );
	this->Disconnect( wxID_ANY, wxEVT_UPDATE_UI, wxUpdateUIEventHandler( Frame::on_view_column_update_ui ) );
	this->Disconnect( wxID_ANY, wxEVT_UPDATE_UI, wxUpdateUIEventHandler( Frame::on_view_column_update_ui ) );
	this->Disconnect( wxID_ANY, wxEVT_UPDATE_UI, wxUpdateUIEventHandler( Frame::on_view_column_update_ui ) );
	this->Disconnect( wxID_ANY, wxEVT_UPDATE_UI, wxUpdateUIEventHandler( Frame::on_view_column_update_ui ) );
	this->Disconnect( wxID_ANY, wxEVT_UPDATE_UI, wxUpdateUIEventHandler( Frame::on_view_column_update_ui ) );
	this->Disconnect( wxID_ANY, wxEVT_UPDATE_UI, wxUpdateUIEventHandler( Frame::on_view_column_update_ui ) );
	this->Disconnect( wxID_ANY, wxEVT_UPDATE_UI, wxUpdateUIEventHandler( Frame::on_view_column_update_ui ) );
	this->Disconnect( wxID_ANY, wxEVT_UPDATE_UI, wxUpdateUIEventHandler( Frame::on_view_column_update_ui ) );
	this->Disconnect( wxID_ANY, wxEVT_UPDATE_UI, wxUpdateUIEventHandler( Frame::on_view_column_update_ui ) );
	this->Disconnect( wxID_ANY, wxEVT_UPDATE_UI, wxUpdateUIEventHandler( Frame::on_view_column_update_ui ) );
	this->Disconnect( wxID_ANY, wxEVT_UPDATE_UI, wxUpdateUIEventHandler( Frame::on_view_column_update_ui ) );
	this->Disconnect( wxID_ANY, wxEVT_UPDATE_UI, wxUpdateUIEventHandler( Frame::on_view_zebra_update_ui ) );
	this->Disconnect( wxID_ANY, wxEVT_UPDATE_UI, wxUpdateUIEventHandler( Frame::on_view_labels_update_ui ) );
	this->Disconnect( ID_ATTACH, wxEVT_UPDATE_UI, wxUpdateUIEventHandler( Frame::on_has_any_selected_devices_update_ui ) );
	this->Disconnect( ID_ATTACH_ONCE, wxEVT_UPDATE_UI, wxUpdateUIEventHandler( Frame::on_has_any_selected_devices_update_ui ) );
	this->Disconnect( ID_ATTACH_STOP, wxEVT_UPDATE_UI, wxUpdateUIEventHandler( Frame::on_has_any_selected_devices_update_ui ) );
	this->Disconnect( wxID_CLOSE, wxEVT_UPDATE_UI, wxUpdateUIEventHandler( Frame::on_has_selected_devices_update_ui ) );
	this->Disconnect( ID_REMOVE_SERVER, wxEVT_UPDATE_UI, wxUpdateUIEventHandler( Frame::on_has_any_selected_devices_update_ui ) );
	this->Disconnect( ID_TOGGLE_LOG_WINDOW, wxEVT_UPDATE_UI, wxUpdateUIEventHandler( Frame::on_log_show_update_ui ) );
	this->Disconnect( wxID_ANY, wxEVT_UPDATE_UI, wxUpdateUIEventHandler( Frame::on_log_verbose_update_ui ) );
	this->Disconnect( wxID_ANY, wxEVT_UPDATE_UI, wxUpdateUIEventHandler( Frame::on_log_library_update_ui ) );
	m_auiToolBar->Disconnect( wxEVT_MOUSEWHEEL, wxMouseEventHandler( Frame::on_frame_mouse_wheel ), NULL, this );
	this->Disconnect( m_tool_reload->GetId(), wxEVT_COMMAND_TOOL_CLICKED, wxCommandEventHandler( Frame::on_reload ) );
	this->Disconnect( m_tool_attach->GetId(), wxEVT_COMMAND_TOOL_CLICKED, wxCommandEventHandler( Frame::on_attach ) );
	this->Disconnect( m_tool_attach->GetId(), wxEVT_UPDATE_UI, wxUpdateUIEventHandler( Frame::on_has_any_selected_devices_update_ui ) );
	this->Disconnect( m_tool_attach_stop->GetId(), wxEVT_COMMAND_TOOL_CLICKED, wxCommandEventHandler( Frame::on_attach_stop ) );
	this->Disconnect( m_tool_attach_stop->GetId(), wxEVT_UPDATE_UI, wxUpdateUIEventHandler( Frame::on_has_any_selected_devices_update_ui ) );
	this->Disconnect( m_tool_attach_stop_all->GetId(), wxEVT_COMMAND_TOOL_CLICKED, wxCommandEventHandler( Frame::on_attach_stop_all ) );
	this->Disconnect( m_tool_detach->GetId(), wxEVT_COMMAND_TOOL_CLICKED, wxCommandEventHandler( Frame::on_detach ) );
	this->Disconnect( m_tool_detach->GetId(), wxEVT_UPDATE_UI, wxUpdateUIEventHandler( Frame::on_has_selected_devices_update_ui ) );
	this->Disconnect( m_tool_detach_all->GetId(), wxEVT_COMMAND_TOOL_CLICKED, wxCommandEventHandler( Frame::on_detach_all ) );
	this->Disconnect( m_tool_load->GetId(), wxEVT_COMMAND_TOOL_CLICKED, wxCommandEventHandler( Frame::on_load ) );
	this->Disconnect( m_tool_save->GetId(), wxEVT_COMMAND_TOOL_CLICKED, wxCommandEventHandler( Frame::on_save ) );
	m_auiToolBarAdd->Disconnect( wxEVT_MOUSEWHEEL, wxMouseEventHandler( Frame::on_frame_mouse_wheel ), NULL, this );
	m_button_add->Disconnect( wxEVT_COMMAND_BUTTON_CLICKED, wxCommandEventHandler( Frame::add_exported_devices ), NULL, this );
	m_treeListCtrl->Disconnect( wxEVT_TREELIST_ITEM_ACTIVATED, wxTreeListEventHandler( Frame::on_item_activated ), NULL, this );
	m_treeListCtrl->Disconnect( wxEVT_TREELIST_ITEM_CONTEXT_MENU, wxTreeListEventHandler( Frame::on_item_context_menu ), NULL, this );

	m_mgr.UnInit();

}
