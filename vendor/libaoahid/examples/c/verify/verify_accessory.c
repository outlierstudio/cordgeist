// SPDX-License-Identifier: MIT
// Copyright (c) 2026 libaoahid contributors
/* Switches the first phone into AOA accessory mode, then types one letter
 * through AOA HID on the re-enumerated device, and, when USB debugging is on,
 * opens (and closes) a Bulk Channel on its ADB interface. Every step is an
 * explicit call; the library itself never waits for re-enumeration or retries.
 *
 * Usage: aoahid_verify_accessory <manufacturer> <model>
 * Android matches these two strings against an app's accessory filter; they
 * are your product values, not library defaults. With no matching app
 * installed, Android may show a "no app for this accessory" prompt; HID input
 * works either way.
 *
 * Before running: open a text field on the phone. To leave accessory mode
 * afterwards, unplug and replug the cable. */
#include "verify_common.h"

#include <string.h>

/* AOA 1.0/2.0: Google VID 0x18D1, PID 0x2D00-0x2D05 is accessory mode; 0x2D01
 * is accessory plus ADB. */
static int is_accessory(const aoahid_device_info* info) {
    return info->vendor_id == 0x18D1U && info->product_id >= 0x2D00U && info->product_id <= 0x2D05U;
}

static int same_port(const aoahid_device_info* info, const uint8_t* path, size_t length,
                     uint8_t bus) {
    return info->bus_number == bus && info->port_path_length == length &&
           (length == 0U || memcmp(info->port_path, path, length) == 0);
}

int main(int argc, char** argv) {
    if (argc != 3) {
        fprintf(stderr, "usage: %s <manufacturer> <model>\n", argv[0]);
        return 2;
    }
    aoahid_context_options context_options;
    aoahid_context_options_init(&context_options);
    aoahid_context* context = NULL;
    aoahid_result result = aoahid_context_create(&context_options, &context);
    if (result != AOAHID_OK)
        return verify_fail("context create", result);

    /* 1. Find the phone in its current mode and remember its physical port. */
    aoahid_discovery* discovery = NULL;
    result = aoahid_discover(context, AOAHID_CONTROL_TIMEOUT_MS, &discovery);
    if (result != AOAHID_OK || aoahid_discovery_count(discovery) == 0U) {
        fprintf(stderr, "no AOA-capable device found; see docs/QUICKSTART.md\n");
        aoahid_discovery_destroy(discovery);
        aoahid_context_destroy(context, AOAHID_CONTROL_TIMEOUT_MS, AOAHID_DRAIN_TIMEOUT_MS);
        return 1;
    }
    const aoahid_device_info* phone = aoahid_discovery_get(discovery, 0U);
    uint8_t path[7] = {0};
    const size_t path_length =
        phone->port_path_length <= sizeof path ? phone->port_path_length : sizeof path;
    memcpy(path, phone->port_path, path_length);
    const uint8_t bus = phone->bus_number;

    /* 2. Request accessory mode: requests 51, 52, 53, then return at once. */
    if (!is_accessory(phone)) {
        aoahid_accessory_options accessory;
        aoahid_accessory_options_init(&accessory);
        accessory.strings.manufacturer = argv[1];
        accessory.strings.model = argv[2];
        accessory.strings.description = "libaoahid verify_accessory";
        result = aoahid_accessory_start(context, phone, &accessory, AOAHID_CONTROL_TIMEOUT_MS);
        aoahid_discovery_destroy(discovery);
        discovery = NULL;
        if (result != AOAHID_OK) {
            aoahid_context_destroy(context, AOAHID_CONTROL_TIMEOUT_MS, AOAHID_DRAIN_TIMEOUT_MS);
            return verify_fail("accessory start", result);
        }
        printf("accessory mode requested; waiting for the phone to reconnect...\n");
    }

    /* 3. The phone disconnects and re-enumerates. Waiting is this program's
     * choice: rediscover until the same port shows an accessory, up to ~10 s
     * (Android cancels the request if the host does not follow up in time). */
    const aoahid_device_info* found = NULL;
    for (int attempt = 0; attempt < 50 && found == NULL; ++attempt) {
        if (discovery == NULL) {
            verify_sleep_ms(200);
            if (aoahid_discover(context, AOAHID_CONTROL_TIMEOUT_MS, &discovery) != AOAHID_OK)
                continue;
        }
        for (size_t index = 0U; index < aoahid_discovery_count(discovery); ++index) {
            const aoahid_device_info* info = aoahid_discovery_get(discovery, index);
            if (is_accessory(info) && same_port(info, path, path_length, bus))
                found = info;
        }
        if (found == NULL) {
            aoahid_discovery_destroy(discovery);
            discovery = NULL;
        }
    }
    if (found == NULL) {
        aoahid_context_destroy(context, AOAHID_CONTROL_TIMEOUT_MS, AOAHID_DRAIN_TIMEOUT_MS);
        fprintf(stderr, "the phone did not reappear in accessory mode\n");
        return 1;
    }
    printf("accessory mode: %04x:%04x\n", (unsigned)found->vendor_id, (unsigned)found->product_id);

    /* 4. From here on it is the ordinary AOA HID sequence. */
    aoahid_device_options device_options;
    aoahid_device_options_init(&device_options);
    aoahid_device* device = NULL;
    result = aoahid_device_open(context, found, &device_options, &device);
    const int with_adb = found->product_id == 0x2D01U;
    aoahid_discovery_destroy(discovery);
    if (result != AOAHID_OK) {
        aoahid_context_destroy(context, AOAHID_CONTROL_TIMEOUT_MS, AOAHID_DRAIN_TIMEOUT_MS);
        return verify_fail("device open", result);
    }
    aoahid_keyboard_options keyboard;
    aoahid_keyboard_options_init(&keyboard);
    aoahid_spec* spec = NULL;
    result = aoahid_spec_create_keyboard(&keyboard, &spec);
    aoahid_node* node = NULL;
    if (result == AOAHID_OK) {
        aoahid_node_options node_options;
        aoahid_node_options_init(&node_options);
        result = aoahid_node_open(device, spec, &node_options, AOAHID_CONTROL_TIMEOUT_MS, &node);
        aoahid_spec_release(spec);
    }
    int exit_code = 0;
    if (result != AOAHID_OK) {
        exit_code = verify_fail("keyboard open", result);
    } else {
        verify_sleep_ms(3000); /* time to focus a text field */
        if (aoahid_keyboard_key(node, 0x04U, 1U) != AOAHID_OK || verify_submit(node) != AOAHID_OK ||
            aoahid_keyboard_key(node, 0x04U, 0U) != AOAHID_OK || verify_submit(node) != AOAHID_OK) {
            exit_code = verify_fail("type 'a'", aoahid_last_error()->code);
        } else {
            printf("typed 'a' through AOA HID in accessory mode\n");
        }
    }

    /* 5. Accessory + ADB shares the one handle: a Channel on the ADB
     * interface (AOSP adb.h: 0xFF/0x42/0x01) sits next to the HID Node. */
    if (exit_code == 0 && with_adb) {
        aoahid_channel_options channel_options;
        aoahid_channel_options_init(&channel_options);
        aoahid_channel* channel = NULL;
        result = aoahid_channel_open(device, &channel_options, AOAHID_DRAIN_TIMEOUT_MS, &channel);
        if (result != AOAHID_OK) {
            exit_code = verify_fail("ADB channel open", result);
        } else {
            printf("ADB channel open on the same USB handle\n");
            result = aoahid_channel_close(channel, AOAHID_DRAIN_TIMEOUT_MS);
            if (result != AOAHID_OK)
                exit_code = verify_fail("ADB channel close", result);
        }
    }

    if (node != NULL)
        (void)aoahid_node_close(node, AOAHID_CONTROL_TIMEOUT_MS, AOAHID_DRAIN_TIMEOUT_MS);
    (void)aoahid_device_close(device, AOAHID_CONTROL_TIMEOUT_MS, AOAHID_DRAIN_TIMEOUT_MS);
    (void)aoahid_context_destroy(context, AOAHID_CONTROL_TIMEOUT_MS, AOAHID_DRAIN_TIMEOUT_MS);
    return exit_code;
}
