// SPDX-License-Identifier: MIT
// Copyright (c) 2026 libaoahid contributors
/* Validates generated and caller-supplied Input descriptors against explicit
 * HID and target policies; it performs no registration or report I/O. */
#pragma once

/*
 * This file performs construction-time HID descriptor and layout validation.
 * It returns structured static diagnostics and performs no I/O.
 */

#include "aoahid.h"
#include "hid/report_layout.hpp"

#include <cstddef>
#include <cstdint>

namespace aoa::hid {

struct ValidationIssue {
    aoahid_result result{AOAHID_OK};
    const char* field{nullptr};
    const char* reason{"No error."};
    std::size_t offset{};
};

/*
 * Target-neutral parser requirements. A caller compares these maxima with the
 * limits of the exact Linux/Android revision it intends to support.
 */
struct DescriptorRequirements {
    std::uint32_t maximum_fields_per_report{};
    std::uint32_t maximum_global_stack_depth{};
    std::uint32_t maximum_report_size_bits{};
    std::uint64_t maximum_usages{};
    std::uint64_t maximum_report_data_bits{};
};

// battery_layout, when given, is the second Input report the descriptor must
// carry at the end of its Application Collection.
ValidationIssue validate_generated(const std::uint8_t* descriptor, std::size_t descriptor_length,
                                   const ReportLayout& layout,
                                   const ReportLayout* battery_layout = nullptr) noexcept;

ValidationIssue validate_raw(const std::uint8_t* descriptor, std::size_t descriptor_length,
                             const aoahid_raw_report* reports, std::size_t report_count,
                             std::uint8_t* trailing_valid_masks = nullptr,
                             DescriptorRequirements* requirements = nullptr);

bool value_fits(std::int32_t logical_minimum, std::int32_t logical_maximum,
                std::uint8_t bit_width) noexcept;

/* Null when the Report ID option is canonical, else the reason it is not: the
 * flag is exactly zero or one, the reserved bytes are zero, a declared ID is
 * 1 through 255, and a disabled ID is zero. Every Report ID in the public API
 * (options, raw reports, manifests) follows this rule. */
const char* report_id_option_violation(const aoahid_report_id_option& option) noexcept;

/* HID 1.11 section 6.2.2.7 Unit item encoding, independent of dimensions. */
bool unit_is_valid(std::uint32_t unit) noexcept;

} // namespace aoa::hid
