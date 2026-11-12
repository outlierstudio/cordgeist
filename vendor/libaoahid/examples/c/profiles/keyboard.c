// SPDX-License-Identifier: MIT
// Copyright (c) 2026 libaoahid contributors
/* Keyboard profile: a full-NKRO one-bit Variable bitmap over one contiguous
 * Usage range. Every simultaneously pressed key gets its own bit, so there is
 * no array slot count and no ErrorRollOver overflow to configure. */
#include "profile_example.h"

static const char* const k_send_sequence[] = {
    "aoahid_keyboard_key(node, 0x04, 1)   press Keyboard a/A; the library sets its bitmap bit",
    "aoahid_node_submit(node, send_ms)  one accepted edge, one request 57",
    "aoahid_keyboard_key(node, 0x04, 0)   release it; a held edge must reach one report",
    "aoahid_node_submit(node, send_ms)  submitting an opposite edge too early is BUSY", NULL};

int main(void) {
    aoahid_keyboard_options options;
    aoahid_spec* spec = NULL;
    aoahid_result result;

    aoahid_keyboard_options_init(&options);

    result = aoahid_spec_create_keyboard(&options, &spec);
    return aoahid_example_run("keyboard", AOAHID_PROFILE_KEYBOARD, result, spec, k_send_sequence);
}
