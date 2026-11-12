// SPDX-License-Identifier: MIT
// Copyright (c) 2026 libaoahid contributors
/* Raw profile: a caller-supplied descriptor that the library validates but does
 * not generate. It bypasses every generated profile, so the caller must
 * acknowledge that no Android support is claimed for it. */
#include "profile_example.h"

/* Generic Desktop Pointer collection, Report ID 1, one 1-bit Input field. */
static const uint8_t k_descriptor[] = {0x05U, 0x01U, 0x09U, 0x06U, 0xA1U, 0x01U, 0x85U,
                                       0x01U, 0x15U, 0x00U, 0x25U, 0x01U, 0x75U, 0x01U,
                                       0x95U, 0x01U, 0x09U, 0x01U, 0x81U, 0x02U, 0xC0U};
static const aoahid_raw_report k_reports[] = {{{1U, 1U, {}}, 2U}};

static const char* const k_send_sequence[] = {
    "aoahid_raw_submit(node, report, sizeof report, send_ms)   caller-supplied report bytes",
    " no state is generated: the report id, length, and padding are only checked",
    " against the immutable parsed layout of the caller descriptor", NULL};

int main(void) {
    aoahid_raw_options options = {0};
    aoahid_spec* spec = NULL;
    aoahid_result result;

    options.struct_size = (uint32_t)sizeof options;
    options.descriptor = k_descriptor;
    options.descriptor_length = sizeof k_descriptor;
    options.reports = k_reports;
    options.report_count = sizeof k_reports / sizeof k_reports[0];
    options.acknowledges_no_android_support = 1U;

    result = aoahid_spec_create_raw(&options, &spec);
    return aoahid_example_run("raw", AOAHID_PROFILE_RAW, result, spec, k_send_sequence);
}
