// SPDX-License-Identifier: MIT
// Copyright (c) 2026 libaoahid contributors
/* Shared reporting helper for the per-profile examples. It holds no product
 * values: each example supplies its own complete option set. */
#ifndef AOAHID_PROFILE_EXAMPLE_H
#define AOAHID_PROFILE_EXAMPLE_H

#include "aoahid.h"

#include <stddef.h>
#include <stdint.h>
#include <stdio.h>

static const char* aoahid_example_status_name(aoahid_android_status status) {
    switch (status) {
    case AOAHID_ANDROID_PORTABLE_CANDIDATE:
        return "portable candidate";
    case AOAHID_ANDROID_CONDITIONAL:
        return "conditional";
    case AOAHID_ANDROID_CUSTOM_SYSTEM_ONLY:
        return "custom system only";
    case AOAHID_ANDROID_UNSUPPORTED:
        return "unsupported";
    default:
        return "unknown";
    }
}

/* Prints the descriptor and report layout the library derived from the caller's
 * options, then the exact call sequence that would send this profile to a real
 * device, then releases the Spec. Returns a process exit code.
 *
 * `send_sequence` is the per-profile part of that walkthrough: the mutation
 * calls that sit between opening a Node and submitting a report. It is printed
 * rather than executed because none of these examples opens a device. */
static int aoahid_example_run(const char* name, aoahid_profile_kind expected_kind,
                              aoahid_result creation_result, aoahid_spec* spec,
                              const char* const* send_sequence) {
    aoahid_capability_manifest manifest = {0};
    const uint8_t* descriptor = NULL;
    size_t descriptor_length = 0U;
    aoahid_result result = creation_result;
    size_t index;
    size_t step;

    if (result == AOAHID_OK && spec != NULL) {
        manifest.struct_size = (uint32_t)sizeof manifest;
        result = aoahid_spec_manifest(spec, &manifest);
    }
    if (result == AOAHID_OK)
        result = aoahid_spec_descriptor(spec, &descriptor, &descriptor_length);
    if (result != AOAHID_OK || spec == NULL) {
        const aoahid_error_detail* detail = aoahid_last_error();
        const char* field = detail == NULL || detail->field == NULL ? "no field" : detail->field;
        const char* reason =
            detail == NULL || detail->reason == NULL ? "no detail" : detail->reason;
        fprintf(stderr, "%s: %s (%s: %s)\n", name, aoahid_result_name(result), field, reason);
        aoahid_spec_release(spec);
        return 1;
    }
    if (manifest.profile_kind != expected_kind || descriptor_length != manifest.descriptor_length) {
        fprintf(stderr, "%s: manifest disagrees with the requested profile\n", name);
        aoahid_spec_release(spec);
        return 1;
    }

    printf("%s: descriptor %zu bytes, %zu report(s), input %s, status %s\n", name,
           descriptor_length, manifest.report_count, manifest.input_supported != 0U ? "yes" : "no",
           aoahid_example_status_name(manifest.android_status));
    for (index = 0U; index < manifest.report_count; ++index) {
        const aoahid_report_capability report = manifest.reports[index];
        if (report.report_id.enabled != 0U)
            printf("  report id %u: %u wire bytes\n", (unsigned)report.report_id.value,
                   (unsigned)report.wire_length);
        else
            printf("  no report id: %u wire bytes\n", (unsigned)report.wire_length);
    }

    /* The Spec above is complete and needs no device. Everything below is the
     * remaining work to make a phone react to it. The steps are identical for
     * every profile except the mutation calls, which each example supplies. */
    printf("\nTo send this profile to a real device:\n");
    printf("  1. aoahid_context_create()   select caller-poll or internal-thread mode\n");
    printf("  2. aoahid_discover()         then aoahid_discovery_count() to pick a device\n");
    printf("  3. aoahid_device_open()      current USB mode; sends no AOA request\n");
    printf("  4. aoahid_node_open()        registers this Spec as its own AOA HID ID\n");
    printf("     (every call that waits takes its timeout as an argument; none has a default)\n");
    step = 5U;
    for (index = 0U; send_sequence != NULL && send_sequence[index] != NULL; ++index) {
        /* A leading space marks a continuation of the previous step rather
         * than a new call, so it must not consume a step number. */
        if (send_sequence[index][0] == ' ')
            printf("    %s\n", send_sequence[index] + 1);
        else
            printf("  %zu. %s\n", step++, send_sequence[index]);
    }
    printf("  %zu. aoahid_node_close()      completes neutral state before request 55\n", step);
    printf("  %zu. aoahid_device_close(), aoahid_context_destroy()\n", step + 1U);
    printf("\nA complete runnable session is examples/c/multi_profile.c.\n");
    printf("A watchable real-device demo is examples/c/verify/ (docs/QUICKSTART.md).\n");

    aoahid_spec_release(spec);
    return 0;
}

#endif
