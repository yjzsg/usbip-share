/*
 * Copyright (c) 2024-2026 Vadym Hrynchyshyn <vadimgrn@gmail.com>
 */

#include "device_columns.h"
#include "utils.h"

#include <libusbip/vhci.h>
#include <libusbip/src/usb_ids.h>

#include <wx/translation.h>

namespace
{

using namespace usbip;

/*
 * 接收模式的两个取值同时承担三种角色:
 *   1. 在"接收模式"列里显示
 *   2. 作为持久化设备(保存/加载)里的字段值
 *   3. to_receive_mode() 用它反解出 receive_mode
 * 所以汉化时必须同时接受旧版本写下的英文值,否则老用户保存的设备会丢掉
 * "低延迟"设置(validate_receive_mode 会把不认识的值一律重置为零拷贝)。
 *
 * 注意: 这里【不能】用 _("零拷贝")。_() 展开为 wxGetTranslation(const char*),
 * 走的是窄字符串路径,内部用 FromAscii 转换,遇到非 ASCII 字节会直接断言
 * ("c < 0x80" failed in FromAscii)。本工程没有 gettext 目录,统一用宽字面量。
 */
const auto recv_zero_copy = wxString(L"零拷贝");
const auto recv_low_latency = wxString(L"低延迟");

// 0.9.8 及更早版本写入的英文值,仅用于读取兼容
const auto recv_zero_copy_legacy = wxString::FromAscii("zero-copy");
const auto recv_low_latency_legacy = wxString::FromAscii("low-latency");

/*
 * @see get_cmp_key
 */
auto make_cmp_key(_In_ const device_location &loc)
{
        device_columns dc;

        get_url(dc) = make_server_url(loc);
        dc[COL_BUSID] = wxString::FromUTF8(loc.busid);

        return dc;
}

/*
 * @param vendor_id  can be zero
 * @param product_id can be zero
 */
void set_vendor_product(_Inout_ device_columns &dc, _In_ UINT16 vendor_id, _In_ UINT16 product_id) 
{
        auto [vendor, product] = get_ids().find_product(vendor_id, product_id);

        for (auto [col, id, str]: { std::make_tuple(COL_VENDOR, vendor_id, vendor),
                                    std::make_tuple(COL_PRODUCT, product_id, product) }) {

                if (id) {
                        dc[col] = str.empty() ? wxString::Format(L"%08x", id) : wxString::FromAscii(str.data(), str.size());
                }
        }
}

} // namespace


/*
 * port, vendor, product can be zero, speed can be UsbLowSpeed aka zero.
 */
bool usbip::is_empty(_In_ const imported_device &d) noexcept
{
        return !d.devid;
}

const wxString& usbip::to_string(_In_ receive_mode mode) noexcept
{
        return mode == receive_mode::low_latency ? recv_low_latency : recv_zero_copy;
}

auto usbip::to_receive_mode(_In_ const wxString &mode) noexcept -> receive_mode
{
        if (mode == recv_low_latency || mode == recv_low_latency_legacy) {
                return receive_mode::low_latency;
        }
        return receive_mode::zero_copy;
}

void usbip::validate_receive_mode(_Inout_ wxString &mode)
{
        if (mode == recv_zero_copy_legacy) {
                mode = recv_zero_copy;   // 旧英文值迁移为中文,不丢设置
        } else if (mode == recv_low_latency_legacy) {
                mode = recv_low_latency;
        } else if (mode != recv_zero_copy && mode != recv_low_latency) {
                mode = recv_zero_copy;
        }
}

auto usbip::make_persistent_device(
        _In_ const wxString &url, _In_ const wxString &busid,
        _In_ const wxString &serial, _In_ const wxString &recv_mode) -> persistent_device
{
        wxString hostname;
        wxString service;
        split_server_url(url, hostname, service);

        return persistent_device {
                .location {
                        .hostname = hostname.utf8_string(), 
                        .service = service.utf8_string(), 
                        .busid = busid.utf8_string()
                },
                .serial = serial.utf8_string(),
                .recv_mode = to_receive_mode(recv_mode)
        };
}

auto usbip::make_persistent_device(_In_ const device_columns &dc) -> persistent_device
{
	auto &url = get_url(dc);
	return make_persistent_device(url, dc[COL_BUSID], dc[COL_SERIAL], dc[COL_RECEIVE_MODE]);
}

auto usbip::make_device_columns(_In_ const imported_device &dev) ->
        std::pair<device_columns, unsigned int> 
{
        auto res = std::make_pair(make_cmp_key(dev.location), 0U);
        auto& [dc, flags] = res;

        dc[COL_RECEIVE_MODE] = to_string(dev.recv_mode);
        flags |= mkflag(COL_RECEIVE_MODE);

        if (!dev.serial.empty()) {
                dc[COL_SERIAL] = wxString::FromUTF8(dev.serial);
                flags |= mkflag(COL_SERIAL);
        }

        if (is_empty(dev)) {
                wxASSERT(!dev.devid);
                wxASSERT(!dev.speed);
                wxASSERT(!dev.vendor);
                wxASSERT(!dev.product);
                wxASSERT(!dev.port);
                return res;
        }
        
        dc[COL_DEVID] = wxString::Format(L"%08x", dev.devid);
        dc[COL_SPEED] = get_speed_str(dev.speed); // do not use _(), get_speed_val must return original value

        set_vendor_product(dc, dev.vendor, dev.product);
        flags |= mkflags({COL_DEVID, COL_SPEED, COL_VENDOR, COL_PRODUCT});

        if (dev.port) {
                dc[COL_PORT] = wxString::Format(L"%03d", dev.port); // XXX for proper sorting
                flags |= mkflag(COL_PORT);
        }

        return res;
}

auto usbip::make_device_columns(_In_ const device_state &st) ->
        std::pair<device_columns, unsigned int> 
{
        auto ret = make_device_columns(st.device);
        auto &[dc, flags] = ret;

        dc[COL_STATE] = to_string(st.state);
        dc[COL_SOURCE_ID] = to_wxstring(st.source_id);

        flags |= mkflags({COL_STATE, COL_SOURCE_ID});
        return ret;
}

wxString usbip::to_wxstring(_In_ unsigned long source_id)
{
        static_assert(sizeof(ULONG) == sizeof(source_id));

        wxString s;
        if (source_id) {
                s = wxString::Format(L"%lx", source_id);
        }
        return s;
}
