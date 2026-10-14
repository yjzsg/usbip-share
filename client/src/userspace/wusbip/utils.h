/*
 * Copyright (c) 2023-2026 Vadym Hrynchyshyn <vadimgrn@gmail.com>
 */

#pragma once

#include <libusbip/vhci.h>

#include <string>
#include <string_view>
#include <tuple>

#include <wx/string.h>

namespace win
{

class FileVersion;
const FileVersion& get_file_version();

/*
 * HKCU 下与 wxConfig 完全一致的配置根键,形如 "Software\USB-SHARE\USB-SHARE"。
 *
 * wxConfig 的根路径由 App::set_names() 的 SetVendorName/SetAppName 决定,而这两者
 * 又来自 exe 版本资源的 CompanyName/ProductName。
 *
 * 历史坑:mgmt.cpp 曾把根键硬编码成旧品牌 "Software\usbip-win2\USBip",品牌改名
 * (CompanyName/ProductName 改成 USB-SHARE)后 wxConfig 根路径随之变化,而这段
 * 硬编码没跟着改 —— 于是所有走硬编码的读操作全部落空。典型表现:用户填了端口,
 * 写进新路径、启动时却从旧路径读,新电脑上旧键根本不存在,端口每次都回默认值
 * ("服务器地址保存了,端口没保存")。
 *
 * 本函数让所有配置读写共享同一个真相(版本资源),以后改品牌不会再分裂。
 */
wxString config_registry_root();

/*
 * 品牌改造前的配置根键。只用于一次性迁移 —— 把老用户的 ServerAddr/ServerPort/
 * ClientId 从旧键搬进新路径。迁移完成后不再作为读写目标;旧键保留不删,留作回滚
 * 保险(用户若退回旧版客户端仍能读到自己的设置)。
 */
inline constexpr const wchar_t *legacy_config_registry_root = L"Software\\usbip-win2\\USBip";

/*
 * 「共享访问密码」(服务端启用访问控制时,连接前要提供的那个密码)的本地持久化。
 *
 * 存放位置与 wxConfig 同一个根键(win::config_registry_root()),每台服务器一个
 * 值,值名为 "AccessPwd_<归一后的 host>"。
 *
 * 安全(针对安全审计 SEC-02「配置明文存注册表」的修复):
 * 密码在写盘前先经 Windows DPAPI 的 CryptProtectData 加密,再以 REG_BINARY
 * 存入。DPAPI 用当前用户的登录凭据派生密钥,因此:
 *   - 同一台机器上的其他 Windows 账户解不开;
 *   - 注册表被导出/复制到别的机器上解不开;
 *   - 还额外绑定了本程序特有的熵(secondary entropy),其他程序必须先从本程序
 *     的二进制里把这串熵取出来才能解密 —— 只是提高门槛,不是一道墙(熵是编译进
 *     二进制的常量,反编译即可得到)。
 * 注意这仍然不是"零知识":本机上以**当前用户身份**运行的任何程序,只要能拿到
 * 那串熵(从本程序二进制里读出来即可),就可以调用 DPAPI 解出密文 —— 这是
 * DPAPI 的固有限制,不是本实现的缺陷。它真正防住的是"同机其他账户"和"注册表被
 * 拷到别的机器",不是"同一用户下的其他程序"。
 *
 * 密文解不开时(换账户/换机器/熵常量变更)一律按"没有保存过密码"处理,让用户
 * 重新输入一次即可,不会阻塞连接。
 *
 * @param host 服务器地址(主机名或 IP),大小写不敏感
 */
bool save_access_password(_In_ const wxString &host, _In_ const wxString &password);
bool load_access_password(_In_ const wxString &host, _Out_ wxString &password);
bool erase_access_password(_In_ const wxString &host);

} // namespace win


namespace mgmt
{
struct meta_device;
} // namespace mgmt


namespace usbip
{

bool init(_Inout_ wxString &err);

wxString GetLastErrorMsg(_In_ DWORD msg_id = GetLastError());
Handle& get_vhci();

class UsbIds;
const UsbIds& get_ids();

NullableHandle& get_event();

const wchar_t* get_speed_str(_In_ USB_DEVICE_SPEED speed) noexcept;
bool get_speed_val(_Out_ USB_DEVICE_SPEED &val, _In_ const wxString &speed) noexcept;

wxString to_string(_In_ state state);

/*
 * UI 列"状态"用:把驱动层 state、服务端 meta_device、以及"同时持有本设备的
 * 客户端总数"合并成一个文案,消歧 "未连接" 在他人占用 / 排队中 / 空闲 三种
 * 远端语义下的歧义,并在多人共用时给出 (N) 后缀。
 *
 * 优先级(命中即返回,不再向下走):
 *   本机 plugged/connected + count >= 2  -> "已连接 (N)"
 *   本机 plugged/connected + count == 1  -> "已连接"
 *   本机 unplugging                      -> "断开中"
 *   本机 connecting                      -> "挂载中"
 *   本机 unplugged + meta.busy + count >= 1 -> "他人占用 (N)"
 *   本机 unplugged + meta.self_queued    -> "排队中"
 *   本机 unplugged + meta 存在(!busy)    -> "空闲"
 *   本机 unplugged + meta == nullptr     -> "未连接"
 *
 * @param local          本机驱动层当前状态 (vhci::state)
 * @param meta           服务端元数据快照(可空)。注意:调用方需保证在 meta
 *                       所在容器的锁外不持有该指针——本函数不拷贝,也不加锁。
 * @param self_client_id 本机持久化的 clientId。空字符串时"他人占用"判定退化为
 *                       "meta.busy && count >= 1" 单条件。
 * @param attached_count 服务端 connections 数组长度(包含本机)。0 表示无 meta
 *                       或字段缺失(向后兼容旧服务端)。
 */
wxString to_view_state(
        _In_ state local,
        _In_ const mgmt::meta_device *meta,
        _In_ const std::string &self_client_id,
        _In_ size_t attached_count = 0);

struct usb_device;
imported_device make_imported_device(_In_ std::string hostname, _In_ std::string service, _In_ const usb_device &dev);

wxString make_device_url(_In_ const device_location &loc);

wxString make_server_url(_In_ const device_location &loc);
wxString make_server_url(_In_ const wxString &hostname, _In_ const wxString &service);

bool split_server_url(_In_ const wxString &url, _Inout_ wxString &hostname, _Inout_ wxString &service);

constexpr UINT32 make_devid(_In_ int busnum, _In_ int devnum) noexcept
{
        return (busnum << 16) | (devnum & USHRT_MAX);
}

inline auto wstring_view(_In_ const wxString &s) noexcept
{
        return std::wstring_view(s.wx_str(), s.length());
}

inline auto wx_string(_In_ std::wstring_view s)
{
        return wxString(s.data(), s.length());
}

constexpr auto get_cmp_key(_In_ const device_location &d)
{
        return std::tie(d.hostname, d.service, d.busid); // tuple of lvalue references
}

constexpr auto operator == (_In_ const device_location &a, _In_ const device_location &b)
{
        return get_cmp_key(a) == get_cmp_key(b);
}

constexpr auto operator <=> (_In_ const device_location &a, _In_ const device_location &b)
{
        return get_cmp_key(a) <=> get_cmp_key(b);
}

constexpr auto operator == (_In_ const persistent_device &a, _In_ const persistent_device &b)
{
        return a.location == b.location; // ignore serial
}

constexpr auto operator <=> (_In_ const persistent_device &a, _In_ const persistent_device &b)
{
        return a.location <=> b.location; // ignore serial
}

} // namespace usbip
