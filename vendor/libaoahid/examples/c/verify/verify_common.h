// SPDX-License-Identifier: MIT
// Copyright (c) 2026 libaoahid contributors
/* Shared device-connection helper for examples/c/verify/. Each program here
 * is meant to be run against a real phone and watched, not just compiled: it
 * prints what to do, waits, then does one clearly described thing. See
 * docs/QUICKSTART.md for the full walkthrough this directory implements.
 */
#ifndef AOAHID_VERIFY_COMMON_H
#define AOAHID_VERIFY_COMMON_H

#if !defined(_WIN32) && !defined(_POSIX_C_SOURCE)
/* Must be defined before any system header is included in this translation
 * unit, or glibc's feature-test-macro selection will already be locked in
 * and nanosleep() below will not be declared. */
#define _POSIX_C_SOURCE 200809L
#endif

#include "aoahid.h"

#include <stdio.h>

#if defined(_WIN32)
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#else
#include <time.h>
#endif

/* nanosleep is POSIX-only; MSVC/Windows has no equivalent libc symbol, so this
 * dispatches to Sleep() there instead. Every verify_*.c program uses this
 * instead of calling nanosleep directly. */
static void verify_sleep_ms(int milliseconds) {
#if defined(_WIN32)
    Sleep((DWORD)milliseconds);
#else
    struct timespec ts;
    ts.tv_sec = milliseconds / 1000;
    ts.tv_nsec = (long)(milliseconds % 1000) * 1000000L;
    nanosleep(&ts, NULL);
#endif
}

/* Prints the field/reason from the calling thread's last error and returns 1,
 * so every failure path in a verify program can just `return verify_fail(...)`. */
static int verify_fail(const char* what, aoahid_result result) {
    /* Read before aoahid_result_name(), a public call that replaces the record;
     * argument evaluation order would otherwise decide which one is printed. */
    const aoahid_error_detail* detail = aoahid_last_error();
    const char* field = detail != NULL && detail->field != NULL ? detail->field : "no field";
    const char* reason = detail != NULL && detail->reason != NULL ? detail->reason : "no reason";
    fprintf(stderr, "%s failed: %s (%s: %s)\n", what, aoahid_result_name(result), field, reason);
    return 1;
}

/* The library never retries. Right after aoahid_node_open, Android may still
 * be registering the HID and refuse the first report with AOAHID_ERR_STALL; a
 * refused report stays pending, so this program resends it itself, a bounded
 * number of times. A timeout is not resent: it may have been delivered. */
static aoahid_result verify_submit(aoahid_node* node) {
    aoahid_result result = AOAHID_ERR_STALL;
    for (int attempt = 0; attempt < 20 && result == AOAHID_ERR_STALL; ++attempt) {
        if (attempt != 0)
            verify_sleep_ms(1);
        result = aoahid_node_submit_blocking(node, AOAHID_SEND_TIMEOUT_MS);
    }
    return result;
}

/* Discovers the first AOA device this host can see and opens it in its current
 * USB mode. On success the caller owns *out_context and *out_device and
 * must close/destroy them; on failure both are left null and a message has
 * already been printed. */
static int verify_open_first_device(aoahid_context** out_context, aoahid_device** out_device) {
    *out_context = NULL;
    *out_device = NULL;

    aoahid_context_options context_options;
    aoahid_context_options_init(&context_options);
    aoahid_result result = aoahid_context_create(&context_options, out_context);
    if (result != AOAHID_OK)
        return verify_fail("context create", result);

    aoahid_discovery* discovery = NULL;
    result = aoahid_discover(*out_context, AOAHID_CONTROL_TIMEOUT_MS, &discovery);
    if (result != AOAHID_OK) {
        aoahid_context_destroy(*out_context, AOAHID_CONTROL_TIMEOUT_MS, AOAHID_DRAIN_TIMEOUT_MS);
        *out_context = NULL;
        return verify_fail("discover", result);
    }
    const size_t count = aoahid_discovery_count(discovery);
    if (count == 0U) {
        fprintf(stderr, "no USB device found. Is the phone plugged in with a data-capable cable, "
                        "directly or through a data-capable hub port (not a charge-only one)? "
#ifdef _WIN32
                        "Does the phone have a driver libusb can open, such as WinUSB? "
#else
                        "Does the udev rule allow access to it? "
#endif
                        "See docs/QUICKSTART.md.\n");
        aoahid_discovery_destroy(discovery);
        aoahid_context_destroy(*out_context, AOAHID_CONTROL_TIMEOUT_MS, AOAHID_DRAIN_TIMEOUT_MS);
        *out_context = NULL;
        return 1;
    }
    const aoahid_device_info* info = aoahid_discovery_get(discovery, 0U);
    printf("using device: %s (%04x:%04x)\n",
           info != NULL && info->product != NULL ? info->product : "(no name)",
           info != NULL ? (unsigned)info->vendor_id : 0U,
           info != NULL ? (unsigned)info->product_id : 0U);
    if (info == NULL || info->protocol_version < 2U) {
        fprintf(stderr, "device has AOA %u; HID needs version 2\n",
                info != NULL ? (unsigned)info->protocol_version : 0U);
        aoahid_discovery_destroy(discovery);
        aoahid_context_destroy(*out_context, AOAHID_CONTROL_TIMEOUT_MS, AOAHID_DRAIN_TIMEOUT_MS);
        *out_context = NULL;
        return 1;
    }

    aoahid_device_options device_options;
    aoahid_device_options_init(&device_options);

    result = aoahid_device_open(*out_context, info, &device_options, out_device);
    aoahid_discovery_destroy(discovery);
    if (result != AOAHID_OK) {
        aoahid_context_destroy(*out_context, AOAHID_CONTROL_TIMEOUT_MS, AOAHID_DRAIN_TIMEOUT_MS);
        *out_context = NULL;
        return verify_fail("device open", result);
    }
    return 0;
}

#endif
