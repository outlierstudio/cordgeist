// SPDX-License-Identifier: MIT
// Copyright (c) 2026 libaoahid contributors
/* Names only USB-IF-audited Usage values needed by implemented profiles; it
 * does not assert Linux or Android support for those Usages. */
#pragma once

/*
 * This file names only Usage values checked against HID Usage Tables 1.7.
 * It does not claim that an operating system maps every listed Usage.
 */

#include <cstdint>

namespace aoa::hid::usage {

constexpr std::uint16_t page_generic_desktop = 0x01U;
constexpr std::uint16_t page_simulation = 0x02U;
constexpr std::uint16_t page_keyboard = 0x07U;
constexpr std::uint16_t page_button = 0x09U;
constexpr std::uint16_t page_telephony = 0x0BU;
constexpr std::uint16_t page_consumer = 0x0CU;
constexpr std::uint16_t page_digitizers = 0x0DU;
constexpr std::uint16_t page_generic_device_controls = 0x06U;
constexpr std::uint16_t page_camera_control = 0x90U;

constexpr std::uint16_t pointer = 0x01U;
constexpr std::uint16_t mouse = 0x02U;
constexpr std::uint16_t gamepad = 0x05U;
constexpr std::uint16_t keyboard = 0x06U;
constexpr std::uint16_t x = 0x30U;
constexpr std::uint16_t y = 0x31U;
constexpr std::uint16_t z = 0x32U;
constexpr std::uint16_t rx = 0x33U;
constexpr std::uint16_t ry = 0x34U;
constexpr std::uint16_t rz = 0x35U;
constexpr std::uint16_t slider = 0x36U;
constexpr std::uint16_t dial = 0x37U;
constexpr std::uint16_t wheel = 0x38U;
constexpr std::uint16_t hat_switch = 0x39U;
constexpr std::uint16_t dpad_up = 0x90U;
constexpr std::uint16_t dpad_down = 0x91U;
constexpr std::uint16_t dpad_right = 0x92U;
constexpr std::uint16_t dpad_left = 0x93U;
constexpr std::uint16_t system_control = 0x80U;

constexpr std::uint16_t simulation_rudder = 0xBAU;
constexpr std::uint16_t simulation_throttle = 0xBBU;
constexpr std::uint16_t simulation_accelerator = 0xC4U;
constexpr std::uint16_t simulation_brake = 0xC5U;
constexpr std::uint16_t simulation_steering = 0xC8U;

constexpr std::uint16_t consumer_control = 0x01U;
constexpr std::uint16_t ac_pan = 0x0238U;

constexpr std::uint16_t phone = 0x01U;

constexpr std::uint16_t camera_auto_focus = 0x20U;
constexpr std::uint16_t camera_shutter = 0x21U;

constexpr std::uint16_t digitizer = 0x01U;
constexpr std::uint16_t pen = 0x02U;
constexpr std::uint16_t touch_screen = 0x04U;
constexpr std::uint16_t touch_pad = 0x05U;
constexpr std::uint16_t stylus = 0x20U;
constexpr std::uint16_t finger = 0x22U;
constexpr std::uint16_t tip_pressure = 0x30U;
constexpr std::uint16_t in_range = 0x32U;
constexpr std::uint16_t invert = 0x3CU;
constexpr std::uint16_t azimuth = 0x3FU;
constexpr std::uint16_t x_tilt = 0x3DU;
constexpr std::uint16_t y_tilt = 0x3EU;
constexpr std::uint16_t twist = 0x41U;
constexpr std::uint16_t tip_switch = 0x42U;
constexpr std::uint16_t barrel_switch = 0x44U;
constexpr std::uint16_t eraser = 0x45U;
constexpr std::uint16_t width = 0x48U;
constexpr std::uint16_t height = 0x49U;
constexpr std::uint16_t contact_identifier = 0x51U;
constexpr std::uint16_t contact_count = 0x54U;
constexpr std::uint16_t contact_count_maximum = 0x55U;
constexpr std::uint16_t scan_time = 0x56U;
constexpr std::uint16_t secondary_barrel_switch = 0x5AU;

constexpr std::uint16_t keyboard_left_control = 0xE0U;
constexpr std::uint16_t keyboard_right_gui = 0xE7U;

constexpr std::uint16_t background_nonuser_controls = 0x01U;
constexpr std::uint16_t battery_strength = 0x20U;

} // namespace aoa::hid::usage
