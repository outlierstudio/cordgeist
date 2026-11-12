// SPDX-License-Identifier: MIT
// Copyright (c) 2026 libaoahid contributors
/* Defines the immutable bit-level report model shared by builders and
 * serializers; it contains no mutable Node or USB state. */
#pragma once

#include "aoahid.h"

/*
 * This file records the immutable wire layout produced with each descriptor.
 * It contains no profile state and performs no I/O.
 */

#include <cstddef>
#include <cstdint>
#include <vector>

namespace aoa::hid {

enum class FieldSemantic : std::uint16_t {
    modifier,
    key_bitmap,
    buttons,
    x,
    y,
    wheel,
    pan,
    consumer_usage,
    gamepad_axis,
    hat,
    tip,
    in_range,
    eraser,
    contact_id,
    pressure,
    width,
    height,
    azimuth,
    scan_time,
    contact_count,
    tilt_x,
    tilt_y,
    twist,
    battery_strength,
    constant_padding
};

struct FieldLayout {
    FieldSemantic semantic{};
    std::uint16_t instance{};
    std::size_t bit_offset{};
    std::uint8_t bit_width{};
    bool is_signed{};
    bool has_null_state{};
    std::int32_t logical_minimum{};
    std::int32_t logical_maximum{};
    aoahid_physical_properties physical{};
};

struct ReportLayout {
    bool has_report_id{};
    std::uint8_t report_id{};
    std::size_t wire_bytes{};
    std::vector<FieldLayout> fields;
};

// A Battery Strength Input report that shares the profile's Application
// Collection under its own Report ID.
struct BatteryField {
    std::uint8_t report_id{};
    std::int32_t logical_minimum{};
    std::int32_t logical_maximum{};
    std::uint8_t bit_width{};
    aoahid_physical_properties physical{};
};

} // namespace aoa::hid
