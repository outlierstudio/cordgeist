// SPDX-License-Identifier: MIT
// Copyright (c) 2026 libaoahid contributors
/* Isolates platform-specific libusb header discovery below the transport
 * boundary so no libusb type can enter a public header. */
#pragma once

/*
 * This is the only header that selects the libusb include spelling. The
 * transport's public/private boundary continues to expose no libusb type.
 */

/*
 * libusb 1.0.30 includes <windows.h> from its public header on Windows.
 * The Windows SDK otherwise defines function-like min/max macros, which
 * corrupt both std::min(...) and std::numeric_limits<T>::max() in the
 * transport implementation.  This must precede the libusb include; the fake
 * test header does not include <windows.h> and therefore cannot mask a
 * production-only MSVC failure.
 */
#if defined(_WIN32) && !defined(NOMINMAX)
#define NOMINMAX 1
#endif

#if __has_include(<libusb.h>)
#include <libusb.h>
#elif __has_include(<libusb-1.0/libusb.h>)
#include <libusb-1.0/libusb.h>
#else
#error "libusb 1.0 headers were not found"
#endif

// libusb 1.0.30 publishes LIBUSB_API_VERSION 0x0100010C. The transport uses
// libusb_init_context() and initialization-time context log options directly;
// it deliberately has no compatibility path through deprecated libusb_init().
#if !defined(LIBUSB_API_VERSION) || LIBUSB_API_VERSION < 0x0100010C
#error "libaoahid requires libusb 1.0.30 or newer headers"
#endif
