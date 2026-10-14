/*
 * Copyright (c) 2024-2026 Vadym Hrynchyshyn <vadimgrn@gmail.com>
 */

#pragma once

#include <wx/string.h>
#include <array>

namespace usbip
{

enum class receive_mode;
struct persistent_device;
struct device_location;
struct imported_device;
struct device_state;

enum column_pos_t { // columns order in the tree
        COL_BUSID,
        COL_PORT,
        COL_SPEED,
        COL_VENDOR,
        COL_PRODUCT,
        COL_STATE,
        COL_PERSISTENT,
        COL_NOTES,
        COL_DEVID,
        COL_SERIAL,
        COL_RECEIVE_MODE,
        COL_LAST_VISIBLE = COL_RECEIVE_MODE,
        COL_SOURCE_ID, // hidden

        UPD_COL_FIRST = COL_PORT,
        UPD_COL_LAST  = COL_SOURCE_ID,
};

/*
 * 由服务器管理接口(mgmt)填充的两列,frame.cpp 把它们追加在树控件末尾。
 *
 * 刻意不并入 column_pos_t: 该枚举既表示列序号、又被 mkflag() 当作位序号使用
 * (1U << col),而 device_columns 数组只覆盖 COL_BUSID..COL_RECEIVE_MODE 加一个
 * URL 槽位。把这两列塞进去会让 dc[COL_MGMT_*] 越界或误撞 URL 槽位。
 *
 * 因此单独定义,并与 frame.cpp 中 AppendColumn 的顺序绑定(有静态断言兜底)。
 */
enum mgmt_column_pos_t {
        MGMT_COL_REMARK = COL_SOURCE_ID + 1, // 12 服务器备注
        MGMT_COL_OWNER  = COL_SOURCE_ID + 2, // 13 占用方
        MGMT_COL_QUEUE  = COL_SOURCE_ID + 3, // 14 排队等待(服务器等待名单)
        MGMT_COL_COUNT  = COL_SOURCE_ID + 4, // 15 树控件实际列数
};

enum { // for device_columns only
        DEV_COL_URL = COL_SOURCE_ID + 1, // use get_url()
        DEV_COL_CNT
};

using device_columns = std::array<wxString, DEV_COL_CNT>; // indexed by visible columns only and DEV_COL_URL

template<typename Array>
constexpr auto& get_url(_In_ Array &v) { return v[DEV_COL_URL]; }

constexpr auto mkflag(_In_ column_pos_t col) { return 1U << col; }

constexpr auto mkflags(_In_ std::initializer_list<column_pos_t> columns) noexcept
{
	auto flags = 0U;
	for (auto col: columns) {
		flags |= mkflag(col);
	}
	return flags;
}
static_assert(mkflags({COL_PORT, COL_SPEED, COL_VENDOR}) == 0b1110);

const wxString& to_string(_In_ receive_mode mode) noexcept;
receive_mode to_receive_mode(_In_ const wxString &mode) noexcept;
void validate_receive_mode(_Inout_ wxString &mode);

persistent_device make_persistent_device(_In_ const device_columns &dc);

persistent_device make_persistent_device(
        _In_ const wxString &url, _In_ const wxString &busid, _In_ const wxString &serial, _In_ const wxString &recv_mode);

std::pair<device_columns, unsigned int> make_device_columns(_In_ const imported_device &dev);
std::pair<device_columns, unsigned int> make_device_columns(_In_ const device_state &st);

bool is_empty(_In_ const imported_device &d) noexcept;
wxString to_wxstring(_In_ unsigned long source_id);

/*
 * @see make_cmp_key
 */
constexpr auto get_cmp_key(_In_ const device_columns &dc)
{
	return std::tie(get_url(dc), dc[COL_BUSID]); // tuple of lvalue references
}

constexpr auto operator == (_In_ const device_columns &a, _In_ const device_columns &b)
{
        return get_cmp_key(a) == get_cmp_key(b);
}

constexpr auto operator <=> (_In_ const device_columns &a, _In_ const device_columns &b)
{
        return get_cmp_key(a) <=> get_cmp_key(b);
}

} // namespace usbip


namespace std
{
	using usbip::operator ==; // ADL does not help because std::tuple is defined in namespace std
	using usbip::operator <=>;
}
