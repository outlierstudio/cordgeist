// SPDX-License-Identifier: MIT
// Copyright (c) 2026 libaoahid contributors
/* Moves the mouse cursor in a circle repeatedly, so a person watching the
 * phone can confirm real relative pointer motion actually arrives.
 *
 * Many Android phones show no visible pointer at all unless an
 * accessibility/DeX-style pointer mode is active. A clean exit with no error
 * here is still useful evidence that the reports were accepted, even with
 * nothing visible on screen; it does not by itself prove the target
 * consumed the motion. See docs/QUICKSTART.md. Ctrl+C to stop early. */
#include "verify_common.h"

#include <math.h>

#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif

int main(void) {
    aoahid_context* context = NULL;
    aoahid_device* device = NULL;
    if (verify_open_first_device(&context, &device) != 0)
        return 1;

    aoahid_mouse_options options;
    aoahid_mouse_options_init(&options);

    aoahid_spec* spec = NULL;
    aoahid_result result = aoahid_spec_create_mouse(&options, &spec);
    if (result != AOAHID_OK) {
        aoahid_device_close(device, AOAHID_CONTROL_TIMEOUT_MS, AOAHID_DRAIN_TIMEOUT_MS);
        aoahid_context_destroy(context, AOAHID_CONTROL_TIMEOUT_MS, AOAHID_DRAIN_TIMEOUT_MS);
        return verify_fail("mouse spec create", result);
    }

    const aoahid_node_options node_options = {(uint32_t)sizeof(aoahid_node_options), 0U, 1U, 0U};
    aoahid_node* node = NULL;
    result = aoahid_node_open(device, spec, &node_options, AOAHID_CONTROL_TIMEOUT_MS, &node);
    aoahid_spec_release(spec);
    if (result != AOAHID_OK) {
        aoahid_device_close(device, AOAHID_CONTROL_TIMEOUT_MS, AOAHID_DRAIN_TIMEOUT_MS);
        aoahid_context_destroy(context, AOAHID_CONTROL_TIMEOUT_MS, AOAHID_DRAIN_TIMEOUT_MS);
        return verify_fail("mouse node open", result);
    }
    /* Android registers the HID asynchronously; input sent at once may be
     * refused or lost (docs/API.md). */
    verify_sleep_ms(100);

    printf("moving the cursor in circles for a few seconds. If nothing is visibly\n");
    printf("moving, that alone does not mean it failed -- see the note above.\n");

    const double radius = 18.0;
    const int steps_per_circle = 36;
    const int circles = 6;
    const double step_angle = 2.0 * M_PI / steps_per_circle;
    double angle = 0.0;
    double prev_x = radius;
    double prev_y = 0.0;

    for (int circle = 0; circle < circles; ++circle) {
        for (int step = 0; step < steps_per_circle; ++step) {
            angle += step_angle;
            const double x = radius * cos(angle);
            const double y = radius * sin(angle);
            const int32_t dx = (int32_t)lround(x - prev_x);
            const int32_t dy = (int32_t)lround(y - prev_y);
            prev_x += dx;
            prev_y += dy;
            if (dx != 0 || dy != 0) {
                result = aoahid_mouse_move(node, dx, dy);
                if (result != AOAHID_OK) {
                    aoahid_node_close(node, AOAHID_CONTROL_TIMEOUT_MS, AOAHID_DRAIN_TIMEOUT_MS);
                    aoahid_device_close(device, AOAHID_CONTROL_TIMEOUT_MS, AOAHID_DRAIN_TIMEOUT_MS);
                    aoahid_context_destroy(context, AOAHID_CONTROL_TIMEOUT_MS,
                                           AOAHID_DRAIN_TIMEOUT_MS);
                    return verify_fail("mouse move", result);
                }
                result = verify_submit(node);
                if (result != AOAHID_OK) {
                    aoahid_node_close(node, AOAHID_CONTROL_TIMEOUT_MS, AOAHID_DRAIN_TIMEOUT_MS);
                    aoahid_device_close(device, AOAHID_CONTROL_TIMEOUT_MS, AOAHID_DRAIN_TIMEOUT_MS);
                    aoahid_context_destroy(context, AOAHID_CONTROL_TIMEOUT_MS,
                                           AOAHID_DRAIN_TIMEOUT_MS);
                    return verify_fail("submit mouse move", result);
                }
            }
            verify_sleep_ms(25);
        }
    }

    printf("done. Expected result: the cursor traced circles, if your phone shows one.\n");
    aoahid_node_close(node, AOAHID_CONTROL_TIMEOUT_MS, AOAHID_DRAIN_TIMEOUT_MS);
    aoahid_device_close(device, AOAHID_CONTROL_TIMEOUT_MS, AOAHID_DRAIN_TIMEOUT_MS);
    aoahid_context_destroy(context, AOAHID_CONTROL_TIMEOUT_MS, AOAHID_DRAIN_TIMEOUT_MS);
    return 0;
}
