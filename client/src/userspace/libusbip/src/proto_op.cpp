/*
 * Copyright (c) 2022-2025 Vadym Hrynchyshyn <vadimgrn@gmail.com>
 */

#include <usbip\proto_op.h>
#include <intrin.h>
// /kernel 下 _byteswap_* 默认不内联（会产生外部调用），显式声明为编译器内建
#pragma intrinsic(_byteswap_ulong)
#pragma intrinsic(_byteswap_ushort)

namespace
{

inline void bswap(UINT16 &val)
{
        static_assert(sizeof(val) == sizeof(unsigned short));
        val = _byteswap_ushort(val);
}

inline void bswap(UINT32 &val)
{
        static_assert(sizeof(val) == sizeof(unsigned long));
        val = _byteswap_ulong(val);
}

} // namespace


void usbip::byteswap(usbip_usb_device &d)
{
        bswap(d.busnum);
        bswap(d.devnum);
        bswap(d.speed);

        bswap(d.idVendor);
        bswap(d.idProduct);
        bswap(d.bcdDevice);
}

void usbip::byteswap(op_common &c)
{
        bswap(c.version);
        bswap(c.code);
        bswap(c.status);
}

void usbip::byteswap(op_devlist_reply &r)
{
        bswap(r.ndev);
}
