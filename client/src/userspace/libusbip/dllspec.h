/*
 * Copyright (c) 2023-2026 Vadym Hrynchyshyn <vadimgrn@gmail.com>
 */

#pragma once

#ifdef USBIP_STATIC
  #define USBIP_API // static library: no dllimport/dllexport
#elif defined(USBIP_EXPORTS)
  #define USBIP_API __declspec(dllexport)
#else
  #define USBIP_API __declspec(dllimport)
#endif
