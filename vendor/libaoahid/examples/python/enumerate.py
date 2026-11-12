#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
# Copyright (c) 2026 libaoahid contributors
"""Enumerate one point-in-time list using only caller-supplied choices."""

from __future__ import annotations

import ctypes as c
import sys

from aoahid.native import (
    AOAHID_ERR_INTERNAL,
    AOAHID_EVENT_CALLER_POLL,
    AOAHID_LOG_DISABLED,
    AOAHID_OK,
    ContextOptions,
    ContextP,
    DiscoveryP,
    LogSink,
    load,
)


def main() -> int:
    if len(sys.argv) != 3:
        print("usage: enumerate.py LIBAOAHID_PATH CONTROL_TIMEOUT_MS", file=sys.stderr)
        return 2
    try:
        timeout_ms = int(sys.argv[2])
    except ValueError:
        print("CONTROL_TIMEOUT_MS must be a positive uint32", file=sys.stderr)
        return 2
    if not 1 <= timeout_ms <= 0xFFFF_FFFF:
        print("CONTROL_TIMEOUT_MS must be a positive uint32", file=sys.stderr)
        return 2
    library = load(sys.argv[1])
    options = ContextOptions()
    options.struct_size = c.sizeof(ContextOptions)
    options.reserved = 0
    options.event_mode = AOAHID_EVENT_CALLER_POLL
    options.log_level = AOAHID_LOG_DISABLED
    options.log_sink = LogSink()
    options.log_user = None
    context = ContextP()
    status = library.aoahid_context_create(c.byref(options), c.byref(context))
    if status != AOAHID_OK:
        return int(status)
    discovery = DiscoveryP()
    status = library.aoahid_discover(context, timeout_ms, c.byref(discovery))
    if status == AOAHID_OK:
        count = int(library.aoahid_discovery_count(discovery))
        if count == 0:
            detail = library.aoahid_last_error()
            if not detail:
                status = AOAHID_ERR_INTERNAL
            elif detail.contents.code != AOAHID_OK:
                status = detail.contents.code
        for index in range(count if status == AOAHID_OK else 0):
            device_pointer = library.aoahid_discovery_get(discovery, index)
            if not device_pointer:
                detail = library.aoahid_last_error()
                status = detail.contents.code if detail else AOAHID_ERR_INTERNAL
                break
            device = device_pointer.contents
            product = device.product.decode(errors="replace") if device.product else ""
            serial = device.serial.decode(errors="replace") if device.serial else ""
            print(
                f"{device.vendor_id:04x}:{device.product_id:04x} "
                f"aoa={device.protocol_version} {product} {serial}"
            )
        library.aoahid_discovery_destroy(discovery)
    close_status = library.aoahid_context_destroy_blocking(context, timeout_ms, timeout_ms, timeout_ms)
    return int(status if status != AOAHID_OK else close_status)


if __name__ == "__main__":
    raise SystemExit(main())
