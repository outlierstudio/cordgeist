// SPDX-License-Identifier: MIT
// Copyright (c) 2026 libaoahid contributors
/* Gamepad profile: two 16-bit axes in place of the four 8-bit ones aoahid_gamepad_options_init
 * sets, with its twelve buttons and Hat Switch (logical 0 to 7 in four bits, as Android expects).
 */
#include "profile_example.h"

static const aoahid_integer_field k_stick = {-32767, 32767, 16U, {0, 0, 0, 0, 0}};

static const char* const k_send_sequence[] = {
    "aoahid_gamepad_axis(node, 0, 0)   axis index 0, caller-declared range",
    "aoahid_gamepad_dpad(node, 1, 0, 0, 0)         booleans become a canonical Hat value",
    "aoahid_gamepad_button(node, 1, 1)     buttons count from 1",
    "aoahid_node_submit(node, send_ms)     opposite D-pad pairs are rejected", NULL};

int main(void) {
    const aoahid_gamepad_axis_option axes[] = {
        {AOAHID_AXIS_X, 0x01U, 0x30U, k_stick, 0, "ABS_X", "AXIS_X"},
        {AOAHID_AXIS_Y, 0x01U, 0x31U, k_stick, 0, "ABS_Y", "AXIS_Y"}};
    aoahid_gamepad_options options;
    aoahid_spec* spec = NULL;
    aoahid_result result;

    aoahid_gamepad_options_init(&options);
    options.axes = axes;
    options.axis_count = sizeof axes / sizeof axes[0];

    result = aoahid_spec_create_gamepad(&options, &spec);
    return aoahid_example_run("gamepad", AOAHID_PROFILE_GAMEPAD, result, spec, k_send_sequence);
}
