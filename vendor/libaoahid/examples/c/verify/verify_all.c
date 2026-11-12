// SPDX-License-Identifier: MIT
// Copyright (c) 2026 libaoahid contributors
/* Runs the touch, keyboard, and mouse profiles against one real phone, first
 * one at a time so each effect is easy to attribute, then interleaved so fast
 * it looks simultaneous on screen.
 *
 * "Simultaneous" here means interleaved from a single thread, not concurrent
 * OS threads: every aoahid_result docstring in aoahid.h requires the caller
 * to serialize all Context/Device/Node calls, so this program never calls
 * into the library from more than one thread at a time. Interleaving single
 * small steps across all three nodes many times a second is indistinguishable
 * to a person watching the phone, and needs no extra locking to stay correct.
 *
 * Before running: open a text field on the phone (Notes app, a search box,
 * anything with a blinking text cursor) so the keyboard phase has somewhere
 * to type into. See docs/QUICKSTART.md. Ctrl+C to stop early. */
#include "verify_common.h"

#include <math.h>

#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif

/* ---- Shared per-node state, advanced one small step per call ---- */

typedef struct {
    aoahid_node* node;
    int32_t center_x;
    int32_t center_y;
    int32_t radius;
    double angle;
    double step_angle;
    uint32_t contact_id;
    int down;
} touch_circle_state;

typedef struct {
    aoahid_node* node;
    double angle;
    double step_angle;
    double radius;
    double prev_x;
    double prev_y;
} mouse_circle_state;

typedef struct {
    aoahid_node* node;
    const char* message;
    size_t index;
    int key_is_down;
    uint16_t current_usage;
} keyboard_typing_state;

/* HUT 1.7 section 10, Keyboard/Keypad Page 0x07. Zero means "not mapped by
 * this small demo", not "not a valid HID Usage". */
static uint16_t usage_for_char(char c) {
    if (c >= 'a' && c <= 'z')
        return (uint16_t)(0x04 + (c - 'a'));
    if (c == ' ')
        return 0x2C;
    if (c >= '1' && c <= '9')
        return (uint16_t)(0x1E + (c - '1'));
    if (c == '0')
        return 0x27;
    return 0;
}

/* Advances the circular touch drag by one small step. Places the contact on
 * first call, then moves it every call after; the caller lifts it separately
 * when the phase ends. Returns 0 on success. */
static int touch_circle_step(touch_circle_state* state) {
    state->angle += state->step_angle;
    const int32_t x = state->center_x + (int32_t)(state->radius * cos(state->angle));
    const int32_t y = state->center_y + (int32_t)(state->radius * sin(state->angle));
    aoahid_result result =
        aoahid_touchscreen_contact(state->node, state->contact_id, 1U, x, y, NULL);
    if (result != AOAHID_OK)
        return verify_fail("touch move", result);
    result = verify_submit(state->node);
    if (result != AOAHID_OK)
        return verify_fail("submit touch move", result);
    state->down = 1;
    return 0;
}

static int touch_circle_lift(touch_circle_state* state) {
    if (!state->down)
        return 0;
    aoahid_result result = aoahid_touchscreen_contact(state->node, state->contact_id, 0U,
                                                      state->center_x, state->center_y, NULL);
    if (result != AOAHID_OK)
        return verify_fail("touch up", result);
    result = verify_submit(state->node);
    if (result != AOAHID_OK)
        return verify_fail("submit touch up", result);
    state->down = 0;
    return 0;
}

static int mouse_circle_step(mouse_circle_state* state) {
    state->angle += state->step_angle;
    const double x = state->radius * cos(state->angle);
    const double y = state->radius * sin(state->angle);
    const int32_t dx = (int32_t)lround(x - state->prev_x);
    const int32_t dy = (int32_t)lround(y - state->prev_y);
    state->prev_x += dx;
    state->prev_y += dy;
    if (dx == 0 && dy == 0)
        return 0;
    aoahid_result result = aoahid_mouse_move(state->node, dx, dy);
    if (result != AOAHID_OK)
        return verify_fail("mouse move", result);
    result = verify_submit(state->node);
    if (result != AOAHID_OK)
        return verify_fail("submit mouse move", result);
    return 0;
}

/* Types the next character of the message, holding each key down for one
 * call and releasing it on the next, so it can be interleaved with the other
 * two nodes' steps one HID report at a time. Wraps back to the start of the
 * message when it reaches the end. Returns 0 on success. */
static int keyboard_typing_step(keyboard_typing_state* state) {
    if (state->key_is_down) {
        aoahid_result result = aoahid_keyboard_key(state->node, state->current_usage, 0U);
        if (result != AOAHID_OK)
            return verify_fail("key up", result);
        result = verify_submit(state->node);
        if (result != AOAHID_OK)
            return verify_fail("submit key up", result);
        state->key_is_down = 0;
        return 0;
    }
    char c = state->message[state->index];
    if (c == '\0') {
        state->index = 0;
        c = state->message[0];
    }
    state->index += 1;
    const uint16_t usage = usage_for_char(c);
    if (usage == 0)
        return 0; /* Silently skip characters this small demo does not map. */
    aoahid_result result = aoahid_keyboard_key(state->node, usage, 1U);
    if (result != AOAHID_OK)
        return verify_fail("key down", result);
    result = verify_submit(state->node);
    if (result != AOAHID_OK)
        return verify_fail("submit key down", result);
    state->current_usage = usage;
    state->key_is_down = 1;
    return 0;
}

/* ---- One-profile-at-a-time phases ---- */

static int phase_touch_circle(aoahid_node* touch_node, int seconds) {
    printf("-- touch: drawing circles on screen for %d seconds --\n", seconds);
    touch_circle_state state = {touch_node, 16000, 16000, 6000, 0.0, 2.0 * M_PI / 30.0, 0U, 0};
    const int steps = seconds * 1000 / 40;
    for (int i = 0; i < steps; ++i) {
        if (touch_circle_step(&state) != 0)
            return 1;
        verify_sleep_ms(40);
    }
    return touch_circle_lift(&state);
}

static int phase_keyboard_hello(aoahid_node* keyboard_node, int repeats) {
    printf("-- keyboard: typing \"hello world\" %d time(s) --\n", repeats);
    keyboard_typing_state state = {keyboard_node, "hello world ", 0, 0, 0};
    /* Two report submissions (down, then up) per character. */
    const int steps = repeats * 24;
    for (int i = 0; i < steps; ++i) {
        if (keyboard_typing_step(&state) != 0)
            return 1;
        verify_sleep_ms(70);
    }
    if (state.key_is_down) {
        aoahid_result result = aoahid_keyboard_key(keyboard_node, state.current_usage, 0U);
        if (result != AOAHID_OK)
            return verify_fail("key up", result);
        result = verify_submit(keyboard_node);
        if (result != AOAHID_OK)
            return verify_fail("submit key up", result);
    }
    return 0;
}

static int phase_mouse_circle(aoahid_node* mouse_node, int seconds) {
    printf("-- mouse: moving the cursor in circles for %d seconds --\n", seconds);
    printf("   (many phones show no visible pointer; a clean run with no error\n"
           "   below is still useful evidence even with nothing visible)\n");
    mouse_circle_state state = {mouse_node, 0.0, 2.0 * M_PI / 30.0, 18.0, 18.0, 0.0};
    const int steps = seconds * 1000 / 30;
    for (int i = 0; i < steps; ++i) {
        if (mouse_circle_step(&state) != 0)
            return 1;
        verify_sleep_ms(30);
    }
    return 0;
}

/* ---- All three interleaved ---- */

static int phase_all_at_once(aoahid_node* touch_node, aoahid_node* keyboard_node,
                             aoahid_node* mouse_node, int seconds) {
    printf("-- all three at once: touch circling, typing, and mouse circling --\n");
    touch_circle_state touch_state = {touch_node,        16000, 16000, 6000, 0.0,
                                      2.0 * M_PI / 60.0, 0U,    0};
    keyboard_typing_state keyboard_state = {keyboard_node, "hello world ", 0, 0, 0};
    mouse_circle_state mouse_state = {mouse_node, 0.0, 2.0 * M_PI / 60.0, 18.0, 18.0, 0.0};
    const int steps = seconds * 1000 / 40;
    for (int i = 0; i < steps; ++i) {
        if (touch_circle_step(&touch_state) != 0)
            return 1;
        if (mouse_circle_step(&mouse_state) != 0)
            return 1;
        if (i % 2 == 0) {
            /* Typed roughly half as often as the other two so one held key
             * per report stays readable instead of a blur of characters. */
            if (keyboard_typing_step(&keyboard_state) != 0)
                return 1;
        }
        verify_sleep_ms(40);
    }
    if (keyboard_state.key_is_down) {
        aoahid_result result = aoahid_keyboard_key(keyboard_node, keyboard_state.current_usage, 0U);
        if (result != AOAHID_OK)
            return verify_fail("key up", result);
        result = verify_submit(keyboard_node);
        if (result != AOAHID_OK)
            return verify_fail("submit key up", result);
    }
    return touch_circle_lift(&touch_state);
}

int main(void) {
    aoahid_context* context = NULL;
    aoahid_device* device = NULL;
    if (verify_open_first_device(&context, &device) != 0)
        return 1;

    aoahid_touchscreen_options touchscreen_options;
    aoahid_touchscreen_options_init(&touchscreen_options);
    touchscreen_options.maximum_contacts = 1U;
    touchscreen_options.contacts_per_report = 1U;
    touchscreen_options.contact_count = aoahid_integer_field_make(0, 1, 1U);

    aoahid_keyboard_options keyboard_options;
    aoahid_keyboard_options_init(&keyboard_options);

    aoahid_mouse_options mouse_options;
    aoahid_mouse_options_init(&mouse_options);

    aoahid_spec* touch_spec = NULL;
    aoahid_spec* keyboard_spec = NULL;
    aoahid_spec* mouse_spec = NULL;
    aoahid_node* touch_node = NULL;
    aoahid_node* keyboard_node = NULL;
    aoahid_node* mouse_node = NULL;
    int exit_code = 0;
    aoahid_result result;

    result = aoahid_spec_create_touchscreen(&touchscreen_options, &touch_spec);
    if (result != AOAHID_OK) {
        exit_code = verify_fail("touchscreen spec create", result);
        goto cleanup;
    }
    result = aoahid_spec_create_keyboard(&keyboard_options, &keyboard_spec);
    if (result != AOAHID_OK) {
        exit_code = verify_fail("keyboard spec create", result);
        goto cleanup;
    }
    result = aoahid_spec_create_mouse(&mouse_options, &mouse_spec);
    if (result != AOAHID_OK) {
        exit_code = verify_fail("mouse spec create", result);
        goto cleanup;
    }

    {
        const aoahid_node_options node_options = {(uint32_t)sizeof(aoahid_node_options), 0U, 1U,
                                                  0U};
        result = aoahid_node_open(device, touch_spec, &node_options, AOAHID_CONTROL_TIMEOUT_MS,
                                  &touch_node);
        if (result != AOAHID_OK) {
            exit_code = verify_fail("touchscreen node open", result);
            goto cleanup;
        }
        result = aoahid_node_open(device, keyboard_spec, &node_options, AOAHID_CONTROL_TIMEOUT_MS,
                                  &keyboard_node);
        if (result != AOAHID_OK) {
            exit_code = verify_fail("keyboard node open", result);
            goto cleanup;
        }
        result = aoahid_node_open(device, mouse_spec, &node_options, AOAHID_CONTROL_TIMEOUT_MS,
                                  &mouse_node);
        if (result != AOAHID_OK) {
            exit_code = verify_fail("mouse node open", result);
            goto cleanup;
        }
    }

    printf("Open a text field on the phone now.\n");
    printf("Starting in 3 seconds...\n");
    verify_sleep_ms(3000);

    for (int round = 1; round <= 2 && exit_code == 0; ++round) {
        printf("\n=== round %d of 2: one profile at a time ===\n", round);
        if (phase_touch_circle(touch_node, 3) != 0 || phase_keyboard_hello(keyboard_node, 1) != 0 ||
            phase_mouse_circle(mouse_node, 3) != 0) {
            exit_code = 1;
        }
    }

    if (exit_code == 0) {
        printf("\n=== final check: all three interleaved ===\n");
        if (phase_all_at_once(touch_node, keyboard_node, mouse_node, 6) != 0)
            exit_code = 1;
    }

    if (exit_code == 0)
        printf("\ndone. Expected result: circles + \"hello world\" alternated twice, then all "
               "three together.\n");

cleanup:
    if (touch_node != NULL)
        aoahid_node_close(touch_node, AOAHID_CONTROL_TIMEOUT_MS, AOAHID_DRAIN_TIMEOUT_MS);
    if (keyboard_node != NULL)
        aoahid_node_close(keyboard_node, AOAHID_CONTROL_TIMEOUT_MS, AOAHID_DRAIN_TIMEOUT_MS);
    if (mouse_node != NULL)
        aoahid_node_close(mouse_node, AOAHID_CONTROL_TIMEOUT_MS, AOAHID_DRAIN_TIMEOUT_MS);
    if (touch_spec != NULL)
        aoahid_spec_release(touch_spec);
    if (keyboard_spec != NULL)
        aoahid_spec_release(keyboard_spec);
    if (mouse_spec != NULL)
        aoahid_spec_release(mouse_spec);
    if (device != NULL)
        aoahid_device_close(device, AOAHID_CONTROL_TIMEOUT_MS, AOAHID_DRAIN_TIMEOUT_MS);
    if (context != NULL)
        aoahid_context_destroy(context, AOAHID_CONTROL_TIMEOUT_MS, AOAHID_DRAIN_TIMEOUT_MS);
    return exit_code;
}
