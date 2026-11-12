// SPDX-License-Identifier: MIT
// Copyright (c) 2026 libaoahid contributors
/* Pen profile: a direct-screen digitizer with pressure, tilt, hover, and the
 * HUT Invert selector for the opposite end. Barrel controls are individual
 * Digitizers-page Usage IDs because HUT does not define them as a range. */
#include "profile_example.h"

static const uint16_t k_barrel_usages[] = {0x44U, 0x5AU};

static const char* const k_send_sequence[] = {
    "aoahid_pen_update(node, &sample)   tip, pressure, and in-range stay consistent",
    "aoahid_node_submit(node, send_ms)  Away state is normalized on the wire",
    "aoahid_pen_update(node, &sample)   in_range 0 takes the pen away; repeat the last X/Y",
    "aoahid_node_submit(node, send_ms)  an in-range pen/eraser switch departs first", NULL};

int main(void) {
    aoahid_pen_options options;
    const aoahid_integer_field tilt = {-90, 90, 8U, {0, 0, 0, 0, 0}};
    aoahid_spec* spec = NULL;
    aoahid_result result;

    aoahid_pen_options_init(&options);
    options.enable_tilt = 1U;
    options.tilt_x = tilt;
    options.tilt_y = tilt;
    options.barrel_usages = k_barrel_usages;
    options.barrel_usage_count = sizeof k_barrel_usages / sizeof k_barrel_usages[0];
    options.enable_eraser = 1U;

    result = aoahid_spec_create_pen(&options, &spec);
    return aoahid_example_run("pen", AOAHID_PROFILE_PEN, result, spec, k_send_sequence);
}
