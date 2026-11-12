// SPDX-License-Identifier: MIT
// Copyright (c) 2026 libaoahid contributors
/* Drags one finger back and forth across the screen five times, so a person
 * watching the phone can confirm real touch input actually arrives.
 *
 * See docs/QUICKSTART.md for the full setup this assumes. Any screen can be
 * on when you run this; no text field is needed. Ctrl+C to stop early. */
#include "verify_common.h"

int main(void) {
    aoahid_context* context = NULL;
    aoahid_device* device = NULL;
    if (verify_open_first_device(&context, &device) != 0)
        return 1;

    /* A single fixed-slot Multi-Touch contact. The logical range 0..32767 is
     * unitless on the wire; the target's input stack maps it onto the actual
     * screen extent, so this drags fully across the screen regardless of its
     * physical size. */
    aoahid_touchscreen_options options;
    aoahid_touchscreen_options_init(&options);
    options.maximum_contacts = 1U;
    options.contacts_per_report = 1U;
    options.contact_count = aoahid_integer_field_make(0, 1, 1U);

    aoahid_spec* spec = NULL;
    aoahid_result result = aoahid_spec_create_touchscreen(&options, &spec);
    if (result != AOAHID_OK) {
        aoahid_device_close(device, AOAHID_CONTROL_TIMEOUT_MS, AOAHID_DRAIN_TIMEOUT_MS);
        aoahid_context_destroy(context, AOAHID_CONTROL_TIMEOUT_MS, AOAHID_DRAIN_TIMEOUT_MS);
        return verify_fail("touchscreen spec create", result);
    }

    const aoahid_node_options node_options = {(uint32_t)sizeof(aoahid_node_options), 0U, 1U, 0U};
    aoahid_node* node = NULL;
    result = aoahid_node_open(device, spec, &node_options, AOAHID_CONTROL_TIMEOUT_MS, &node);
    aoahid_spec_release(spec);
    if (result != AOAHID_OK) {
        aoahid_device_close(device, AOAHID_CONTROL_TIMEOUT_MS, AOAHID_DRAIN_TIMEOUT_MS);
        aoahid_context_destroy(context, AOAHID_CONTROL_TIMEOUT_MS, AOAHID_DRAIN_TIMEOUT_MS);
        return verify_fail("touchscreen node open", result);
    }
    /* Android registers the HID asynchronously; input sent at once may be
     * refused or lost (docs/API.md). */
    verify_sleep_ms(100);

    printf("dragging across the screen 5 times over about 6 seconds. Watch the phone.\n");

    const int32_t y = 16000;
    const int steps = 40;
    for (int cycle = 0; cycle < 5; ++cycle) {
        const int32_t start_x = (cycle % 2 == 0) ? 3000 : 29000;
        const int32_t end_x = (cycle % 2 == 0) ? 29000 : 3000;

        result = aoahid_touchscreen_contact(node, 0U, 1U, start_x, y, NULL);
        if (result != AOAHID_OK) {
            aoahid_node_close(node, AOAHID_CONTROL_TIMEOUT_MS, AOAHID_DRAIN_TIMEOUT_MS);
            aoahid_device_close(device, AOAHID_CONTROL_TIMEOUT_MS, AOAHID_DRAIN_TIMEOUT_MS);
            aoahid_context_destroy(context, AOAHID_CONTROL_TIMEOUT_MS, AOAHID_DRAIN_TIMEOUT_MS);
            return verify_fail("touch down", result);
        }
        result = verify_submit(node);
        if (result != AOAHID_OK) {
            aoahid_node_close(node, AOAHID_CONTROL_TIMEOUT_MS, AOAHID_DRAIN_TIMEOUT_MS);
            aoahid_device_close(device, AOAHID_CONTROL_TIMEOUT_MS, AOAHID_DRAIN_TIMEOUT_MS);
            aoahid_context_destroy(context, AOAHID_CONTROL_TIMEOUT_MS, AOAHID_DRAIN_TIMEOUT_MS);
            return verify_fail("submit touch down", result);
        }

        for (int i = 1; i <= steps; ++i) {
            const int32_t x = start_x + (int32_t)((int64_t)(end_x - start_x) * i / steps);
            result = aoahid_touchscreen_contact(node, 0U, 1U, x, y, NULL);
            if (result != AOAHID_OK) {
                aoahid_node_close(node, AOAHID_CONTROL_TIMEOUT_MS, AOAHID_DRAIN_TIMEOUT_MS);
                aoahid_device_close(device, AOAHID_CONTROL_TIMEOUT_MS, AOAHID_DRAIN_TIMEOUT_MS);
                aoahid_context_destroy(context, AOAHID_CONTROL_TIMEOUT_MS, AOAHID_DRAIN_TIMEOUT_MS);
                return verify_fail("touch move", result);
            }
            result = verify_submit(node);
            if (result != AOAHID_OK) {
                aoahid_node_close(node, AOAHID_CONTROL_TIMEOUT_MS, AOAHID_DRAIN_TIMEOUT_MS);
                aoahid_device_close(device, AOAHID_CONTROL_TIMEOUT_MS, AOAHID_DRAIN_TIMEOUT_MS);
                aoahid_context_destroy(context, AOAHID_CONTROL_TIMEOUT_MS, AOAHID_DRAIN_TIMEOUT_MS);
                return verify_fail("submit touch move", result);
            }
            verify_sleep_ms(30);
        }

        result = aoahid_touchscreen_contact(node, 0U, 0U, end_x, y, NULL);
        if (result != AOAHID_OK) {
            aoahid_node_close(node, AOAHID_CONTROL_TIMEOUT_MS, AOAHID_DRAIN_TIMEOUT_MS);
            aoahid_device_close(device, AOAHID_CONTROL_TIMEOUT_MS, AOAHID_DRAIN_TIMEOUT_MS);
            aoahid_context_destroy(context, AOAHID_CONTROL_TIMEOUT_MS, AOAHID_DRAIN_TIMEOUT_MS);
            return verify_fail("touch up", result);
        }
        result = verify_submit(node);
        if (result != AOAHID_OK) {
            aoahid_node_close(node, AOAHID_CONTROL_TIMEOUT_MS, AOAHID_DRAIN_TIMEOUT_MS);
            aoahid_device_close(device, AOAHID_CONTROL_TIMEOUT_MS, AOAHID_DRAIN_TIMEOUT_MS);
            aoahid_context_destroy(context, AOAHID_CONTROL_TIMEOUT_MS, AOAHID_DRAIN_TIMEOUT_MS);
            return verify_fail("submit touch up", result);
        }
        verify_sleep_ms(300);
    }

    printf("done. Expected result: 5 left-right drags visible on screen.\n");
    aoahid_node_close(node, AOAHID_CONTROL_TIMEOUT_MS, AOAHID_DRAIN_TIMEOUT_MS);
    aoahid_device_close(device, AOAHID_CONTROL_TIMEOUT_MS, AOAHID_DRAIN_TIMEOUT_MS);
    aoahid_context_destroy(context, AOAHID_CONTROL_TIMEOUT_MS, AOAHID_DRAIN_TIMEOUT_MS);
    return 0;
}
