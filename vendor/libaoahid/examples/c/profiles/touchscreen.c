// SPDX-License-Identifier: MIT
// Copyright (c) 2026 libaoahid contributors
/* Touchscreen profile: fixed multi-touch with three contacts per report and the
 * full optional field set enabled. Azimuth declares degrees through the four
 * HID 1.11 Global physical items rather than assuming a unit. */
#include "profile_example.h"

static const char* const k_send_sequence[] = {
    "aoahid_touchscreen_contact(node, 0, 1, 500, 500, NULL)   contact id 0, tip down",
    "aoahid_node_submit(node, send_ms)          Contact Count and Scan Time are derived",
    "aoahid_touchscreen_contact(node, 0, 0, 500, 500, NULL)   tip up keeps the same stable id",
    "aoahid_node_submit(node, send_ms)          closes the frame", NULL};

int main(void) {
    aoahid_touchscreen_options options;
    const aoahid_integer_field byte_range = {0, 255, 8U, {0, 0, 0, 0, 0}};
    aoahid_spec* spec = NULL;
    aoahid_result result;

    aoahid_touchscreen_options_init(&options);
    options.maximum_contacts = 3U;
    options.contacts_per_report = 3U;
    options.contact_count = (aoahid_integer_field){0, 3, 2U, {0, 0, 0, 0, 0}};
    options.enable_pressure = 1U;
    options.pressure = byte_range;
    options.enable_width = 1U;
    options.width = byte_range;
    options.enable_height = 1U;
    options.height = byte_range;
    options.enable_azimuth = 1U;
    options.azimuth = (aoahid_integer_field){0, 36000, 16U, {1U, 0, 360, 0, 0x14U}};
    options.enable_scan_time = 1U;
    options.scan_time = (aoahid_integer_field){0, 65535, 16U, {0, 0, 0, 0, 0}};
    options.scan_time_unit_100us = 1U;

    result = aoahid_spec_create_touchscreen(&options, &spec);
    return aoahid_example_run("touchscreen", AOAHID_PROFILE_TOUCHSCREEN, result, spec,
                              k_send_sequence);
}
