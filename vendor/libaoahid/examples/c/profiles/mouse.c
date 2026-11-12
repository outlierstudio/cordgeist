// SPDX-License-Identifier: MIT
// Copyright (c) 2026 libaoahid contributors
/* Mouse profile as aoahid_mouse_options_init sets it: five buttons, 16-bit relative X and Y, and
 * an 8-bit Wheel and AC Pan. */
#include "profile_example.h"

static const char* const k_send_sequence[] = {
    "aoahid_mouse_move(node, 10, -5)   pending totals are 64-bit and signed",
    "aoahid_mouse_scroll(node, 1, 0)   Wheel and AC Pan are independent",
    "aoahid_mouse_button(node, 1, 1)   buttons count from 1",
    "aoahid_node_submit(node, send_ms)  only the submitted fragment is consumed", NULL};

int main(void) {
    aoahid_mouse_options options;
    aoahid_spec* spec = NULL;
    aoahid_result result;

    aoahid_mouse_options_init(&options);

    result = aoahid_spec_create_mouse(&options, &spec);
    return aoahid_example_run("mouse", AOAHID_PROFILE_MOUSE, result, spec, k_send_sequence);
}
