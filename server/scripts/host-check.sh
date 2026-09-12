#!/bin/sh
# Run on the Linux host, not inside the container.
set -u

ok=0
warn=0
printf '%s\n' '== Linux USB/IP host check =='
printf 'architecture: '
uname -m 2>/dev/null || { printf '%s\n' 'unknown'; warn=1; }
printf 'kernel: '
uname -r 2>/dev/null || { printf '%s\n' 'unknown'; warn=1; }

kernel=$(uname -r 2>/dev/null || printf '')
if [ -n "$kernel" ] && [ -e "/lib/modules/$kernel/kernel/drivers/usb/usbip/usbip-core.ko" -o -e "/lib/modules/$kernel/kernel/drivers/usb/usbip/usbip-core.ko.xz" -o -e "/lib/modules/$kernel/kernel/drivers/usb/usbip/usbip-core.ko.zst" ]; then
    printf '%s\n' 'usbip-core module: present'
else
    printf '%s\n' 'usbip-core module: NOT FOUND'
    warn=1
fi

if [ -n "$kernel" ] && [ -e "/lib/modules/$kernel/kernel/drivers/usb/usbip/usbip-host.ko" -o -e "/lib/modules/$kernel/kernel/drivers/usb/usbip/usbip-host.ko.xz" -o -e "/lib/modules/$kernel/kernel/drivers/usb/usbip/usbip-host.ko.zst" ]; then
    printf '%s\n' 'usbip-host module: present'
else
    printf '%s\n' 'usbip-host module: NOT FOUND'
    warn=1
fi

if grep -q '^usbip_host ' /proc/modules 2>/dev/null; then
    printf '%s\n' 'usbip_host loaded: yes'
else
    printf '%s\n' 'usbip_host loaded: no (the container will try modprobe)'
fi

if command -v lsusb >/dev/null 2>&1; then
    printf '%s\n' 'USB devices:'
    lsusb || warn=1
else
    printf '%s\n' 'lsusb: not installed (not fatal; the container can enumerate USB)'
fi

if [ "$warn" -ne 0 ]; then
    printf '%s\n' 'Result: review the warnings before deploying.'
    exit 1
fi
printf '%s\n' 'Result: basic host prerequisites look present.'
exit "$ok"
