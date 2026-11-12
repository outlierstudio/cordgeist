// SPDX-License-Identifier: MIT
// Copyright (c) 2026 libaoahid contributors
/* Exercises caller-owned discovery, multi-profile state, reconnection, and teardown through the C
 * ABI. Options start from the *_init functions; docs/EXAMPLES.md explains each value changed. */
#if !defined(_WIN32) && !defined(_POSIX_C_SOURCE)
/* Must be defined before any system header is included in this translation
 * unit, or glibc's feature-test-macro selection will already be locked in
 * and nanosleep() below will not be declared. */
#define _POSIX_C_SOURCE 200809L
#endif
#include "aoahid.h"

#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#if defined(_WIN32)
#define WIN32_LEAN_AND_MEAN
#include <process.h>
#include <windows.h>
#else
#include <pthread.h>
#include <time.h>
#endif

/* A human watching a real device needs the press/tap to be held for a
 * perceptible instant and needs time to focus a text field first. None of
 * this affects CI: it is only reached when at least one real device was
 * actually discovered, and continuous integration runs against a fake
 * backend that discovers zero devices. */
static void demo_sleep_ms(int milliseconds) {
#if defined(_WIN32)
    Sleep((DWORD)milliseconds);
#else
    struct timespec ts;
    ts.tv_sec = milliseconds / 1000;
    ts.tv_nsec = (long)(milliseconds % 1000) * 1000000L;
    nanosleep(&ts, NULL);
#endif
}

/* The values this example changes from the *_init functions, and its input; docs/EXAMPLES.md
 * lists them. */
static const uint32_t k_control_timeout_ms = 500U;
static const uint32_t k_host_report_policy_bytes = 4088U;
static const uint32_t k_pool_slots = 8U;
static const uint32_t k_reserved_slots_per_node = 1U;
static const uint32_t k_close_drain_timeout_ms = 1000U;
static const uint16_t k_keyboard_a_usage = 0x04U;           /* HUT 1.7 section 10. */
static const uint16_t k_keyboard_application_usage = 0x65U; /* HUT 1.7 section 10. */
static const uint32_t k_pointer_button_count = 3U;
static const int32_t k_relative_minimum = -127;
static const int32_t k_relative_maximum = 127;
static const uint32_t k_relative_bits = 8U;
static const uint32_t k_touch_contact_id = 1U;
static const int32_t k_example_touch_x = 1000;
static const int32_t k_example_touch_y = 2000;
static const int32_t k_example_mouse_dx = 30;
static const int32_t k_example_mouse_dy = -20;

typedef struct locator {
    uint8_t bus_number;
    uint8_t* port_path;
    size_t port_path_length;
    char* serial;
} locator;

typedef struct specs {
    aoahid_spec* keyboard;
    aoahid_spec* mouse;
    aoahid_spec* touchscreen;
} specs;

typedef struct session {
    locator identity;
    aoahid_device* device;
    aoahid_node* keyboard;
    aoahid_node* mouse;
    aoahid_node* touchscreen;
    int needs_reopen;
    int ready_to_submit;
    aoahid_result open_error;
} session;

typedef struct worker_task {
    const locator* identity;
    const specs* definitions;
    int result;
} worker_task;

static int failed(aoahid_result result, const char* operation) {
    if (result == AOAHID_OK)
        return 0;
    const aoahid_error_detail* detail = aoahid_last_error();
    /* aoahid_result_name() is itself a public call and therefore clears this
     * thread's diagnostic record. Copy both borrowed strings before calling it. */
    const char* field_source = detail == NULL || detail->field == NULL ? "no field" : detail->field;
    const char* reason_source =
        detail == NULL || detail->reason == NULL ? "no detail" : detail->reason;
    const size_t field_length = strlen(field_source);
    const size_t reason_length = strlen(reason_source);
    char* field = field_length == SIZE_MAX ? NULL : (char*)malloc(field_length + 1U);
    char* reason = reason_length == SIZE_MAX ? NULL : (char*)malloc(reason_length + 1U);
    if (field != NULL)
        memcpy(field, field_source, field_length + 1U);
    if (reason != NULL)
        memcpy(reason, reason_source, reason_length + 1U);
    fprintf(stderr, "%s: %s (%s: %s)\n", operation, aoahid_result_name(result),
            field == NULL ? "diagnostic copy failed" : field,
            reason == NULL ? "diagnostic copy failed" : reason);
    free(field);
    free(reason);
    return 1;
}

static const aoahid_device_info* discovery_entry(const aoahid_discovery* discovery, size_t index,
                                                 aoahid_result* status) {
    const aoahid_device_info* info = aoahid_discovery_get(discovery, index);
    if (info == NULL) {
        const aoahid_error_detail* detail = aoahid_last_error();
        *status = detail == NULL || detail->code == AOAHID_OK ? AOAHID_ERR_INTERNAL : detail->code;
    }
    return info;
}

static aoahid_result discovery_count_checked(const aoahid_discovery* discovery, size_t* count) {
    *count = aoahid_discovery_count(discovery);
    if (*count != 0U)
        return AOAHID_OK;
    const aoahid_error_detail* detail = aoahid_last_error();
    return detail == NULL ? AOAHID_ERR_INTERNAL : detail->code;
}

static aoahid_context_options context_options(void) {
    aoahid_context_options options;
    aoahid_context_options_init(&options);
    options.event_mode = AOAHID_EVENT_CALLER_POLL;
    return options;
}

static aoahid_device_options device_options(void) {
    aoahid_device_options options;
    aoahid_device_options_init(&options);
    options.descriptor_fragment_bytes = 64U; /* AOA fixes no fragment size. */
    options.transfer_pool_slots = k_pool_slots;
    options.maximum_report_bytes = k_host_report_policy_bytes;
    options.host_control_buffer_policy_bytes = k_host_report_policy_bytes;
    options.validate_reports = 1U;
    return options;
}

static aoahid_node_options node_options(void) {
    aoahid_node_options options;
    aoahid_node_options_init(&options);
    options.has_reserved_slots = 1U;
    options.reserved_slots = k_reserved_slots_per_node;
    return options;
}

static aoahid_result create_specs(specs* result) {
    aoahid_keyboard_options keyboard;
    aoahid_keyboard_options_init(&keyboard);
    keyboard.usage_maximum = k_keyboard_application_usage;

    const aoahid_integer_field relative =
        aoahid_integer_field_make(k_relative_minimum, k_relative_maximum, k_relative_bits);
    aoahid_mouse_options mouse;
    aoahid_mouse_options_init(&mouse);
    mouse.button_count = k_pointer_button_count;
    mouse.x = relative;
    mouse.y = relative;
    mouse.wheel = relative;
    mouse.enable_pan = 0U;
    mouse.pan = aoahid_integer_field_make(0, 0, 0U);

    aoahid_touchscreen_options touch;
    aoahid_touchscreen_options_init(&touch);
    touch.maximum_contacts = 1U;
    touch.contacts_per_report = 1U;
    touch.contact_count = aoahid_integer_field_make(0, 1, 1U);

    *result = (specs){NULL, NULL, NULL};
    aoahid_result status = aoahid_spec_create_keyboard(&keyboard, &result->keyboard);
    if (status == AOAHID_OK)
        status = aoahid_spec_create_mouse(&mouse, &result->mouse);
    if (status == AOAHID_OK)
        status = aoahid_spec_create_touchscreen(&touch, &result->touchscreen);
    return status;
}

static void release_specs(specs* value) {
    aoahid_spec_release(value->keyboard);
    aoahid_spec_release(value->mouse);
    aoahid_spec_release(value->touchscreen);
    *value = (specs){NULL, NULL, NULL};
}

static char* copy_text(const char* text) {
    if (text == NULL)
        text = "";
    const size_t length = strlen(text);
    if (length == SIZE_MAX)
        return NULL;
    char* result = (char*)malloc(length + 1U);
    if (result != NULL)
        memcpy(result, text, length + 1U);
    return result;
}

static aoahid_result copy_locator(const aoahid_device_info* info, locator* result) {
    *result = (locator){0U, NULL, 0U, NULL};
    /* This caller refuses bus-only correlation: without a physical path it
     * cannot establish which rediscovered device is the one that disappeared. */
    if (info->port_path_length == 0U || info->port_path == NULL)
        return AOAHID_ERR_UNSUPPORTED;
    result->bus_number = info->bus_number;
    result->port_path_length = info->port_path_length;
    result->port_path = (uint8_t*)malloc(info->port_path_length);
    if (result->port_path == NULL)
        return AOAHID_ERR_INTERNAL;
    memcpy(result->port_path, info->port_path, info->port_path_length);
    result->serial = copy_text(info->serial);
    if (result->serial == NULL) {
        free(result->port_path);
        *result = (locator){0U, NULL, 0U, NULL};
        return AOAHID_ERR_INTERNAL;
    }
    return AOAHID_OK;
}

static void free_locator(locator* value) {
    free(value->port_path);
    free(value->serial);
    *value = (locator){0U, NULL, 0U, NULL};
}

static int locator_matches(const locator* identity, const aoahid_device_info* info) {
    if (identity->bus_number != info->bus_number ||
        identity->port_path_length != info->port_path_length)
        return 0;
    if (identity->port_path_length != 0U && info->port_path == NULL)
        return 0;
    if (identity->port_path_length != 0U &&
        memcmp(identity->port_path, info->port_path, identity->port_path_length) != 0)
        return 0;
    return identity->serial[0] == '\0' ||
           (info->serial != NULL && strcmp(identity->serial, info->serial) == 0);
}

static void print_info(const aoahid_device_info* info) {
    printf("bus=%u address=%u port=", (unsigned int)info->bus_number,
           (unsigned int)info->device_address);
    for (size_t index = 0U; index < info->port_path_length; ++index)
        printf("%s%u", index == 0U ? "" : ".", (unsigned int)info->port_path[index]);
    printf(" vid:pid=%04x:%04x aoa=%u product=\"%s\" serial=\"%s\"\n",
           (unsigned int)info->vendor_id, (unsigned int)info->product_id,
           (unsigned int)info->protocol_version, info->product == NULL ? "" : info->product,
           info->serial == NULL ? "" : info->serial);
}

static aoahid_result open_session(aoahid_context* context, const aoahid_device_info* info,
                                  const specs* definitions, session* value) {
    /* Open sends no AOA request, so the version check is the caller's. */
    if (info->protocol_version < 2U) {
        fprintf(stderr, "device open: AOA %u has no HID; version 2 is required\n",
                (unsigned int)info->protocol_version);
        return AOAHID_ERR_NOT_AOA;
    }
    const aoahid_device_options open_options = device_options();
    const aoahid_node_options nodes = node_options();
    aoahid_result status = aoahid_device_open(context, info, &open_options, &value->device);
    if (status == AOAHID_OK)
        status = aoahid_node_open(value->device, definitions->keyboard, &nodes,
                                  k_control_timeout_ms, &value->keyboard);
    if (status == AOAHID_OK)
        status = aoahid_node_open(value->device, definitions->mouse, &nodes, k_control_timeout_ms,
                                  &value->mouse);
    if (status == AOAHID_OK)
        status = aoahid_node_open(value->device, definitions->touchscreen, &nodes,
                                  k_control_timeout_ms, &value->touchscreen);
    if (status != AOAHID_OK) {
        /* Copy/report TLS diagnostics before cleanup makes another public call. */
        (void)failed(status, value->device == NULL ? "device open" : "profile registration");
    }
    if (status != AOAHID_OK && value->device != NULL) {
        /* Every result of a valid close consumes the Device and its Nodes. */
        (void)aoahid_device_close(value->device, k_control_timeout_ms, k_close_drain_timeout_ms);
        value->device = NULL;
        value->keyboard = NULL;
        value->mouse = NULL;
        value->touchscreen = NULL;
    }
    return status;
}

static aoahid_result close_device(session* value) {
    if (value->device == NULL)
        return AOAHID_OK;
    const aoahid_result status =
        aoahid_device_close(value->device, k_control_timeout_ms, k_close_drain_timeout_ms);
    /* Every result of a valid close consumes the Device and its Nodes. */
    value->device = NULL;
    value->keyboard = NULL;
    value->mouse = NULL;
    value->touchscreen = NULL;
    if (status != AOAHID_OK && status != AOAHID_ERR_NO_DEVICE)
        (void)failed(status, "device close");
    return status;
}

static aoahid_result close_touchscreen(aoahid_context* context, session* value) {
    while (value->touchscreen != NULL) {
        const aoahid_result status =
            aoahid_node_close(value->touchscreen, k_control_timeout_ms, k_close_drain_timeout_ms);
        if (status != AOAHID_CLOSE_PENDING) {
            /* Any other result consumed the Node; an error says what failed on the way out. */
            value->touchscreen = NULL;
            return status;
        }
        /* The Node is still ours: let its report finish, then close it again. */
        const aoahid_result poll = aoahid_context_poll(context, k_control_timeout_ms);
        if (poll != AOAHID_OK)
            return poll;
    }
    return AOAHID_OK;
}

static aoahid_result destroy_context(aoahid_context* context) {
    for (;;) {
        const aoahid_result status =
            aoahid_context_destroy(context, k_control_timeout_ms, k_close_drain_timeout_ms);
        if (status != AOAHID_CLOSE_PENDING)
            return status; /* Every non-pending valid destroy result consumes Context. */
        const aoahid_result poll = aoahid_context_poll(context, k_control_timeout_ms);
        if (poll != AOAHID_OK)
            (void)failed(poll, "context poll during shutdown");
    }
}

static aoahid_result first_failure(const aoahid_result* values, size_t count) {
    for (size_t index = 0U; index < count; ++index) {
        if (values[index] != AOAHID_OK)
            return values[index];
    }
    return AOAHID_OK;
}

static aoahid_result update_session(session* value) {
    const aoahid_result statuses[] = {
        aoahid_keyboard_key(value->keyboard, k_keyboard_a_usage, 1U),
        aoahid_mouse_move(value->mouse, k_example_mouse_dx, k_example_mouse_dy),
        aoahid_touchscreen_contact(value->touchscreen, k_touch_contact_id, 1U, k_example_touch_x,
                                   k_example_touch_y, NULL),
    };
    return first_failure(statuses, sizeof(statuses) / sizeof(statuses[0]));
}

static aoahid_result submit_session(session* value) {
    const aoahid_result statuses[] = {
        aoahid_node_submit_blocking(value->keyboard, k_control_timeout_ms),
        aoahid_node_submit_blocking(value->mouse, k_control_timeout_ms),
        aoahid_node_submit_blocking(value->touchscreen, k_control_timeout_ms),
    };
    return first_failure(statuses, sizeof(statuses) / sizeof(statuses[0]));
}

static aoahid_result update_then_submit(session* sessions, size_t count) {
    aoahid_result result = AOAHID_OK;
    /* Update every device first. The second-pass USB submission keeps one
     * device's USB operation out of the next device's state preparation. */
    for (size_t index = 0U; index < count; ++index) {
        session* current = &sessions[index];
        current->ready_to_submit = 0;
        if (current->device == NULL)
            continue;
        const aoahid_result status = update_session(current);
        if (status == AOAHID_ERR_NO_DEVICE) {
            current->needs_reopen = 1;
            (void)close_device(current);
        } else if (status != AOAHID_OK) {
            (void)failed(status, "profile update");
            if (result == AOAHID_OK)
                result = status;
        } else {
            current->ready_to_submit = 1;
        }
    }
    for (size_t index = 0U; index < count; ++index) {
        session* current = &sessions[index];
        if (current->device == NULL || !current->ready_to_submit)
            continue;
        const aoahid_result status = submit_session(current);
        if (status == AOAHID_ERR_NO_DEVICE) {
            current->needs_reopen = 1;
            (void)close_device(current);
        } else if (status != AOAHID_OK) {
            (void)failed(status, "profile submit");
            if (result == AOAHID_OK)
                result = status;
        }
    }
    return result;
}

static aoahid_result reopen_missing(aoahid_context* context, session* sessions, size_t count,
                                    const specs* definitions) {
    int has_missing = 0;
    for (size_t index = 0U; index < count; ++index)
        has_missing |= sessions[index].needs_reopen;
    if (!has_missing)
        return AOAHID_OK;
    aoahid_discovery* discovery = NULL;
    aoahid_result status = aoahid_discover(context, k_control_timeout_ms, &discovery);
    if (status != AOAHID_OK)
        return status;
    aoahid_result result = AOAHID_OK;
    size_t discovered_count = 0U;
    status = discovery_count_checked(discovery, &discovered_count);
    if (status != AOAHID_OK) {
        aoahid_discovery_destroy(discovery);
        return status;
    }
    for (size_t device_index = 0U; device_index < discovered_count; ++device_index) {
        aoahid_result entry_status = AOAHID_OK;
        const aoahid_device_info* info = discovery_entry(discovery, device_index, &entry_status);
        if (info == NULL) {
            result = entry_status;
            break;
        }
        for (size_t session_index = 0U; session_index < count; ++session_index) {
            session* current = &sessions[session_index];
            if (!current->needs_reopen || !locator_matches(&current->identity, info))
                continue;
            const aoahid_result open_status = open_session(context, info, definitions, current);
            if (open_status == AOAHID_OK)
                current->needs_reopen = 0;
            else {
                if (result == AOAHID_OK)
                    result = open_status;
            }
            break;
        }
    }
    aoahid_discovery_destroy(discovery);
    if (result != AOAHID_OK)
        return result;
    for (size_t index = 0U; index < count; ++index) {
        if (sessions[index].needs_reopen)
            return AOAHID_ERR_NO_DEVICE;
    }
    return AOAHID_OK;
}

static aoahid_result continue_without_touch(aoahid_context* context, session* sessions,
                                            size_t count) {
    aoahid_result result = AOAHID_OK;
    for (size_t index = 0U; index < count; ++index) {
        session* current = &sessions[index];
        if (current->device == NULL)
            continue;
        const aoahid_result close = close_touchscreen(context, current);
        if (close != AOAHID_OK) {
            (void)failed(close, "touchscreen close");
            result = close;
        }
    }
    /* Keyboard and mouse remain live after the touchscreen alone is gone. */
    for (size_t index = 0U; index < count; ++index) {
        session* current = &sessions[index];
        current->ready_to_submit = 0;
        if (current->device == NULL)
            continue;
        const aoahid_result statuses[] = {
            aoahid_keyboard_key(current->keyboard, k_keyboard_a_usage, 0U),
            aoahid_mouse_button(current->mouse, 1U, 1U), /* Caller selects pointer button 1. */
        };
        const aoahid_result status =
            first_failure(statuses, sizeof(statuses) / sizeof(statuses[0]));
        if (status == AOAHID_ERR_NO_DEVICE) {
            (void)close_device(current);
        } else if (status != AOAHID_OK) {
            result = status;
        } else {
            current->ready_to_submit = 1;
        }
    }
    for (size_t index = 0U; index < count; ++index) {
        session* current = &sessions[index];
        if (current->device == NULL || !current->ready_to_submit)
            continue;
        const aoahid_result statuses[] = {
            aoahid_node_submit_blocking(current->keyboard, k_control_timeout_ms),
            aoahid_node_submit_blocking(current->mouse, k_control_timeout_ms),
        };
        const aoahid_result status =
            first_failure(statuses, sizeof(statuses) / sizeof(statuses[0]));
        if (status != AOAHID_OK)
            result = status;
    }
    return result;
}

static aoahid_result enumerate_sessions(aoahid_context* context, const specs* definitions,
                                        session** out_sessions, size_t* out_count) {
    aoahid_discovery* discovery = NULL;
    aoahid_result status = aoahid_discover(context, k_control_timeout_ms, &discovery);
    if (status != AOAHID_OK)
        return status;
    size_t count = 0U;
    status = discovery_count_checked(discovery, &count);
    if (status != AOAHID_OK) {
        aoahid_discovery_destroy(discovery);
        return status;
    }
    if (count > SIZE_MAX / sizeof(session)) {
        aoahid_discovery_destroy(discovery);
        return AOAHID_ERR_OVERFLOW;
    }
    session* sessions = count == 0U ? NULL : (session*)calloc(count, sizeof(session));
    if (count != 0U && sessions == NULL) {
        aoahid_discovery_destroy(discovery);
        return AOAHID_ERR_INTERNAL;
    }
    for (size_t index = 0U; index < count; ++index) {
        const aoahid_device_info* info = discovery_entry(discovery, index, &status);
        if (info == NULL)
            break;
        print_info(info);
        const aoahid_result identity_status = copy_locator(info, &sessions[index].identity);
        if (identity_status != AOAHID_OK) {
            sessions[index].open_error = identity_status;
            fprintf(stderr, "stable device locator: %s\n",
                    identity_status == AOAHID_ERR_UNSUPPORTED
                        ? "caller policy requires a physical port path"
                        : "caller could not copy the reported identity");
            continue;
        }
        const aoahid_result opened = open_session(context, info, definitions, &sessions[index]);
        sessions[index].open_error = opened;
    }
    aoahid_discovery_destroy(discovery);
    if (status != AOAHID_OK) {
        for (size_t index = 0U; index < count; ++index) {
            (void)close_device(&sessions[index]);
            free_locator(&sessions[index].identity);
        }
        free(sessions);
        return status;
    }
    *out_sessions = sessions;
    *out_count = count;
    return AOAHID_OK;
}

static int run_shared(void) {
    const aoahid_context_options options = context_options();
    aoahid_context* context = NULL;
    aoahid_result status = aoahid_context_create(&options, &context);
    if (status != AOAHID_OK)
        return failed(status, "context create");
    specs definitions = {NULL, NULL, NULL};
    status = create_specs(&definitions);
    if (status != AOAHID_OK) {
        (void)failed(status, "profile spec creation");
        release_specs(&definitions);
        (void)destroy_context(context);
        return 1;
    }
    session* sessions = NULL;
    size_t count = 0U;
    status = enumerate_sessions(context, &definitions, &sessions, &count);
    int exit_code = status == AOAHID_OK ? 0 : failed(status, "discovery");
    if (status == AOAHID_OK) {
        for (size_t index = 0U; index < count; ++index) {
            if (sessions[index].open_error != AOAHID_OK)
                exit_code = 1;
        }
        /* Human-observable path only: a real device was actually found, so a
         * person watching its screen needs time to focus a text field and
         * needs the state below held long enough to see and for Android's
         * input pipeline to register it as a real key/tap rather than noise.
         * A continuous-integration run against the fake backend discovers
         * zero devices and never reaches this branch, so test timing and
         * determinism are unaffected. */
        int has_open_device = 0;
        for (size_t index = 0U; index < count; ++index) {
            if (sessions[index].device != NULL)
                has_open_device = 1;
        }
        if (has_open_device) {
            printf("Device found. Focus a text field on the phone now.\n");
            printf("Pressing 'a', moving the mouse, and touching the screen in 2 seconds...\n");
            demo_sleep_ms(2000);
        }
        status = update_then_submit(sessions, count);
        if (status != AOAHID_OK)
            exit_code = 1;
        if (has_open_device)
            demo_sleep_ms(
                400); /* Hold the pressed/touched state long enough to see and register. */
        /* Only missing sessions are reopened; every live sibling stays open. */
        status = reopen_missing(context, sessions, count, &definitions);
        if (status != AOAHID_OK)
            exit_code |= failed(status, "rediscovery");
        status = continue_without_touch(context, sessions, count);
        if (status != AOAHID_OK)
            exit_code = 1;
    }
    for (size_t index = 0U; index < count; ++index) {
        (void)close_device(&sessions[index]);
        free_locator(&sessions[index].identity);
    }
    free(sessions);
    release_specs(&definitions);
    status = destroy_context(context);
    if (status != AOAHID_OK)
        exit_code |= failed(status, "context destroy");
    return exit_code;
}

static int run_one_worker(worker_task* task) {
    const aoahid_context_options options = context_options();
    aoahid_context* context = NULL;
    aoahid_result status = aoahid_context_create(&options, &context);
    if (status != AOAHID_OK)
        return 1;
    session value = {0};
    value.identity = *task->identity; /* Borrowed until this joined worker exits. */
    aoahid_discovery* discovery = NULL;
    status = aoahid_discover(context, k_control_timeout_ms, &discovery);
    int found = 0;
    if (status == AOAHID_OK) {
        size_t count = 0U;
        status = discovery_count_checked(discovery, &count);
        for (size_t index = 0U; index < count; ++index) {
            const aoahid_device_info* info = discovery_entry(discovery, index, &status);
            if (info == NULL)
                break;
            if (locator_matches(task->identity, info)) {
                found = 1;
                status = open_session(context, info, task->definitions, &value);
                break;
            }
        }
        aoahid_discovery_destroy(discovery);
        if (status == AOAHID_OK && !found)
            status = AOAHID_ERR_NO_DEVICE;
    }
    if (status == AOAHID_OK && value.device != NULL) {
        printf("Device found (bus %u). Focus a text field on the phone now.\n",
               (unsigned int)task->identity->bus_number);
        printf("Pressing 'a', moving the mouse, and touching the screen in 2 seconds...\n");
        demo_sleep_ms(2000);
        status = update_then_submit(&value, 1U); /* This worker selects one Device. */
        if (status == AOAHID_OK) {
            demo_sleep_ms(
                400); /* Hold the pressed/touched state long enough to see and register. */
            status = reopen_missing(context, &value, 1U, task->definitions);
        }
        if (status == AOAHID_OK)
            status = continue_without_touch(context, &value, 1U);
    }
    (void)close_device(&value);
    const aoahid_result close = destroy_context(context);
    return status == AOAHID_OK && close == AOAHID_OK ? 0 : 1;
}

#if defined(_WIN32)
static unsigned __stdcall worker_entry(void* argument) {
    worker_task* task = (worker_task*)argument;
    task->result = run_one_worker(task);
    return 0U; /* The detailed result is stored in the joined task object. */
}
#else
static void* worker_entry(void* argument) {
    worker_task* task = (worker_task*)argument;
    task->result = run_one_worker(task);
    return NULL;
}
#endif

static int run_threaded(void) {
    const aoahid_context_options options = context_options();
    aoahid_context* enumeration_context = NULL;
    aoahid_result status = aoahid_context_create(&options, &enumeration_context);
    if (status != AOAHID_OK)
        return failed(status, "enumeration context create");
    specs definitions = {NULL, NULL, NULL};
    status = create_specs(&definitions);
    if (status != AOAHID_OK) {
        release_specs(&definitions);
        (void)destroy_context(enumeration_context);
        return 1;
    }
    session* discovered = NULL;
    size_t count = 0U;
    /* Opening is intentionally omitted from this snapshot: each worker owns
     * the only Context and Device handles it will ever touch. */
    aoahid_discovery* snapshot = NULL;
    status = aoahid_discover(enumeration_context, k_control_timeout_ms, &snapshot);
    if (status == AOAHID_OK) {
        status = discovery_count_checked(snapshot, &count);
        if (status == AOAHID_OK && count <= SIZE_MAX / sizeof(session))
            discovered = count == 0U ? NULL : (session*)calloc(count, sizeof(session));
        else if (status == AOAHID_OK)
            status = AOAHID_ERR_OVERFLOW;
        if (status == AOAHID_OK && count != 0U && discovered == NULL)
            status = AOAHID_ERR_INTERNAL;
        for (size_t index = 0U; status == AOAHID_OK && index < count; ++index) {
            const aoahid_device_info* info = discovery_entry(snapshot, index, &status);
            if (info == NULL)
                break;
            print_info(info);
            status = copy_locator(info, &discovered[index].identity);
            if (status != AOAHID_OK)
                fprintf(stderr, "stable device locator: %s\n",
                        status == AOAHID_ERR_UNSUPPORTED
                            ? "caller policy requires a physical port path"
                            : "caller could not copy the reported identity");
        }
        aoahid_discovery_destroy(snapshot);
    }
    const aoahid_result enumeration_close = destroy_context(enumeration_context);
    if (status == AOAHID_OK)
        status = enumeration_close;

    worker_task* tasks = NULL;
#if defined(_WIN32)
    HANDLE* threads = NULL;
#else
    pthread_t* threads = NULL;
#endif
    if (status == AOAHID_OK && count != 0U) {
        if (count > SIZE_MAX / sizeof(worker_task))
            status = AOAHID_ERR_OVERFLOW;
        else
            tasks = (worker_task*)calloc(count, sizeof(worker_task));
        if (count > SIZE_MAX / sizeof(*threads))
            status = AOAHID_ERR_OVERFLOW;
        else
            threads = calloc(count, sizeof(*threads));
        if (tasks == NULL || threads == NULL)
            status = AOAHID_ERR_INTERNAL;
    }
    size_t started = 0U;
    while (status == AOAHID_OK && started < count) {
        tasks[started].identity = &discovered[started].identity;
        tasks[started].definitions = &definitions;
#if defined(_WIN32)
        const uintptr_t native_thread =
            _beginthreadex(NULL, 0U, worker_entry, &tasks[started], 0U, NULL);
        threads[started] = (HANDLE)native_thread;
        /* The worker uses the CRT, so Microsoft requires _beginthreadex rather
         * than CreateThread. See the dated source entry in docs/EXAMPLES.md. */
        if (native_thread == 0U)
            status = AOAHID_ERR_INTERNAL;
#else
        if (pthread_create(&threads[started], NULL, worker_entry, &tasks[started]) != 0)
            status = AOAHID_ERR_INTERNAL;
#endif
        if (status == AOAHID_OK)
            ++started;
    }
    /* Every worker is started before any join, so joining one never prevents a
     * sibling from running. The count is exactly the runtime discovery count. */
    for (size_t index = 0U; index < started; ++index) {
#if defined(_WIN32)
        if (WaitForSingleObject(threads[index], INFINITE) != WAIT_OBJECT_0)
            status = AOAHID_ERR_INTERNAL;
        if (!CloseHandle(threads[index]))
            status = AOAHID_ERR_INTERNAL;
#else
        if (pthread_join(threads[index], NULL) != 0)
            status = AOAHID_ERR_INTERNAL;
#endif
        if (tasks[index].result != 0)
            status = AOAHID_ERR_IO;
    }
    for (size_t index = 0U; index < count; ++index)
        free_locator(&discovered[index].identity);
    free(discovered);
    free(tasks);
    free(threads);
    release_specs(&definitions);
    return status == AOAHID_OK ? 0 : 1;
}

int main(int argc, char** argv) {
    if (argc == 1)
        return run_shared();
    if (argc == 2 && strcmp(argv[1], "shared") == 0)
        return run_shared();
    if (argc == 2 && strcmp(argv[1], "threaded") == 0)
        return run_threaded();
    fprintf(stderr, "usage: aoahid_example_c [shared|threaded]\n");
    return 2;
}
