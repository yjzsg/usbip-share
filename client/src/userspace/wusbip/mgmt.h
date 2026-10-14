/*
 * Management-API integration for the USB/IP server (shares the data port by default).
 *
 * Optional enrichment layer between this client and the server's HTTP
 * management endpoint (server/web.py in the usbip-share repository):
 *   GET  /api/devices              -> device names/remarks/owners (password-free)
 *   POST /api/clients/heartbeat    -> register this PC + public IP + attached busids
 *
 * Design goals:
 *  - Everything is optional. If the host does not run the management API,
 *    the client behaves exactly like the upstream GUI (plain usbip protocol).
 *  - A single background worker thread polls every ~10 s and posts results
 *    to the GUI thread via MainFrame::mgmt_apply / CallAfter.
 *  - The management web port is configurable (persisted "WebPort"), so later
 *    "single-port mode" (Phase B) can simply point it at the usbip port.
 */

#pragma once

#include <map>
#include <set>
#include <string>
#include <vector>

#include <wx/string.h>

namespace mgmt
{

struct settings
{
        bool enabled{ true };
        std::string host;              // usbip server host name / IP (ASCII/UTF-8)
        int data_port{ 5555 };         // usbip data port (used by heartbeat)
        int web_port{ 18080 };         // management HTTP port, 0 = use data_port
        std::string token;             // optional X-Admin-Token (unused by read/heartbeat)
        std::string client_id;         // stable per-PC id (persisted)
        std::string client_name;       // computer name shown to other users
        std::string public_ip;         // public IPv4/IPv6 reported in heartbeat, optional
        /*
         * 本机为该服务器保存的「共享访问密码」(服务端启用访问控制时需要)。
         * 空字符串 = 没保存过 —— 此时连接前会弹框让用户输入。
         *
         * 落盘时经 DPAPI 加密(见 win::save_access_password),这里只在进程内存
         * 里以明文形式存在,供"授权过期后静默续期"使用。
         */
        std::string access_password;
};

struct meta_device
{
        std::string busid;
        wxString display_name;         // server displayName (alias or description)
        wxString alias;                // non-empty iff the user set a server-side alias
        wxString remark;               // server-side remark (web UI)
        wxString owner;                // assembled "谁在用" text
        wxString queue;                // assembled "谁在排队" text (FIFO order)
        std::string holder_id;         // clientId currently holding the device ("" if free)
        bool has_alias{ false };
        // 服务端是否已把该设备共享出来。缺省 false:服务端没给这个字段时
        // 我们无法证明它已共享,就按"不共享"处理(不补进客户端列表)——
        // 与 USB/IP DEVLIST 只返回已 bind 设备的语义一致。
        bool shared{ false };
        bool busy{ false };            // held by *another* client; attach must queue
        bool self_queued{ false };     // this PC is already in the server-side waitlist
        // 当前持有这台设备的所有客户端总数(来自服务端 connections 数组)。
        // 包含本机:本机 attached 时 count >= 1。语义上 = 同时打开着这台设备
        // 的客户端数,UI 用它拼"已连接 (N)"/"他人占用 (N)"后缀。
        size_t attached_count{ 0 };
};

using meta_map = std::map<std::string, meta_device>; // keyed by busid (ASCII)

/*
 * One pending queue wakeup delivered by the server on a heartbeat. The
 * server writes these when the device a client is waiting for becomes free,
 * and drains the mailbox on the next heartbeat — so each entry is a
 * one-shot "now it is your turn" trigger.
 */
struct queue_notice
{
        std::string busid;
        std::string action; // "attach" (your turn) or "dropped" (device went away)
};

/*
 * 把品牌改造前写在旧根键 (win::legacy_config_registry_root) 下的用户设置一次性
 * 迁入当前根路径 (win::config_registry_root())。
 *
 * 背景:客户端品牌从 usbip-win2 改成 USB-SHARE 后,wxConfig 的根路径随之从
 * "Software\usbip-win2\USBip" 变成 "Software\USB-SHARE\USB-SHARE",而历史上读
 * ServerPort 的代码把旧路径写死了 —— 新路径写进去的值永远读不回来。用户看到的
 * 就是"服务器地址保存了、端口没保存"(新电脑上旧键根本不存在,读到的永远是空)。
 *
 * 迁移规则:
 *  - 只在当前路径**缺少**该项时才迁入,绝不覆盖用户在新版里已经设置好的值;
 *  - 旧键**保留不删**,用户若退回旧版客户端仍能读回自己的设置;
 *  - 旧键不存在(新电脑首次安装)时直接返回,不做任何事。
 *
 * 必须在任何配置读取之前调用(目前由 MainFrame 构造函数最先执行)。
 */
void migrate_legacy_config();

} // namespace mgmt
