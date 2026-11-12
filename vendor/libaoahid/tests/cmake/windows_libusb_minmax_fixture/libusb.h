// SPDX-License-Identifier: MIT
// Copyright (c) 2026 libaoahid contributors
/*
 * Minimal production-header fixture.  Official libusb 1.0.30 includes
 * <windows.h> on Windows; these are the conflicting Windows SDK macros that
 * NOMINMAX must suppress before that include occurs.
 */
#pragma once

#if !defined(NOMINMAX)
#define min(left, right) (((left) < (right)) ? (left) : (right))
#define max(left, right) (((left) > (right)) ? (left) : (right))
#endif

#define LIBUSB_API_VERSION 0x0100010C
