// SPDX-License-Identifier: MIT
// Copyright (c) 2026 libaoahid contributors
/* Covers HID item encoding, parser boundaries, and strict raw validation
 * without exercising USB transport. */
#include "hid/descriptor_builder.hpp"
#include "hid/item_writer.hpp"
#include "hid/validator.hpp"
#include "test.hpp"

#include <array>
#include <cstdint>
#include <limits>
#include <vector>

namespace {

void append_u32_item(std::vector<std::uint8_t>* descriptor, const std::uint8_t prefix,
                     const std::uint32_t value) {
    descriptor->push_back(prefix);
    descriptor->push_back(static_cast<std::uint8_t>(value & 0xFFU));
    descriptor->push_back(static_cast<std::uint8_t>((value >> 8U) & 0xFFU));
    descriptor->push_back(static_cast<std::uint8_t>((value >> 16U) & 0xFFU));
    descriptor->push_back(static_cast<std::uint8_t>((value >> 24U) & 0xFFU));
}

std::vector<std::uint8_t> application_preamble() {
    // Usage Page (Generic Desktop), Usage (Keyboard), Application Collection.
    return {0x05U, 0x01U, 0x09U, 0x06U, 0xA1U, 0x01U};
}

void append_globals(std::vector<std::uint8_t>* descriptor, const std::uint8_t report_size,
                    const std::uint32_t report_count) {
    // Logical Minimum 0, Logical Maximum 1, Report Size, Report Count.
    descriptor->insert(descriptor->end(), {0x15U, 0x00U, 0x25U, 0x01U, 0x75U, report_size});
    append_u32_item(descriptor, 0x97U, report_count);
}

aoahid_raw_report idless_report(const std::uint32_t wire_length) {
    aoahid_raw_report report{};
    report.wire_length = wire_length;
    return report;
}

void test_target_neutral_raw_requirements() {
    // The state transitions under test are from the exact target source:
    // https://android.googlesource.com/kernel/common/+/35556bed836f8dc07ac55f69c8d17dce3e7f0e25/drivers/hid/hid-core.c
    // hid_parser_global, hid_parser_local, hid_add_field, hid_register_field.
    {
        auto descriptor = application_preamble();
        append_globals(&descriptor, 1U, 1U);
        // Usage Minimum 0 through Usage Maximum 20000 is 20001 Local Usages.
        descriptor.insert(descriptor.end(),
                          {0x1AU, 0x00U, 0x00U, 0x2AU, 0x20U, 0x4EU, 0x81U, 0x02U, 0xC0U});
        const aoahid_raw_report report = idless_report(1U);
        aoa::hid::DescriptorRequirements requirements{};
        const aoa::hid::ValidationIssue result = aoa::hid::validate_raw(
            descriptor.data(), descriptor.size(), &report, 1U, nullptr, &requirements);
        AOAHID_CHECK(result.result == AOAHID_OK);
        AOAHID_CHECK(requirements.maximum_fields_per_report == 1U);
        AOAHID_CHECK(requirements.maximum_global_stack_depth == 0U);
        AOAHID_CHECK(requirements.maximum_report_size_bits == 1U);
        AOAHID_CHECK(requirements.maximum_usages == 20001U);
        AOAHID_CHECK(requirements.maximum_report_data_bits == 1U);
    }
    {
        auto descriptor = application_preamble();
        append_globals(&descriptor, 1U, 1U);
        // A nearly full 32-bit Usage range requires a 64-bit count and must
        // not be expanded into individual entries by validation.
        descriptor.insert(descriptor.end(), {0x1BU, 0x00U, 0x00U, 0x00U, 0x00U, 0x2BU, 0xFEU, 0xFFU,
                                             0xFFU, 0xFFU, 0x81U, 0x02U, 0xC0U});
        const aoahid_raw_report report = idless_report(1U);
        aoa::hid::DescriptorRequirements requirements{};
        const aoa::hid::ValidationIssue result = aoa::hid::validate_raw(
            descriptor.data(), descriptor.size(), &report, 1U, nullptr, &requirements);
        AOAHID_CHECK(result.result == AOAHID_OK);
        AOAHID_CHECK(requirements.maximum_fields_per_report == 1U);
        AOAHID_CHECK(requirements.maximum_global_stack_depth == 0U);
        AOAHID_CHECK(requirements.maximum_report_size_bits == 1U);
        AOAHID_CHECK(requirements.maximum_usages ==
                     static_cast<std::uint64_t>(std::numeric_limits<std::uint32_t>::max()));
        AOAHID_CHECK(requirements.maximum_report_data_bits == 1U);
    }
    {
        auto descriptor = application_preamble();
        // 524280 one-bit values occupy exactly 65535 bytes. Validation must
        // use checked arithmetic, not a Report Count-sized loop.
        append_globals(&descriptor, 1U, 524280U);
        descriptor.insert(descriptor.end(), {0x09U, 0x01U, 0x81U, 0x02U, 0xC0U});
        const aoahid_raw_report report = idless_report(65535U);
        aoa::hid::DescriptorRequirements requirements{};
        const aoa::hid::ValidationIssue result = aoa::hid::validate_raw(
            descriptor.data(), descriptor.size(), &report, 1U, nullptr, &requirements);
        AOAHID_CHECK(result.result == AOAHID_OK);
        AOAHID_CHECK(requirements.maximum_fields_per_report == 1U);
        AOAHID_CHECK(requirements.maximum_global_stack_depth == 0U);
        AOAHID_CHECK(requirements.maximum_report_size_bits == 1U);
        AOAHID_CHECK(requirements.maximum_usages == 524280U);
        AOAHID_CHECK(requirements.maximum_report_data_bits == 524280U);
    }
    {
        auto descriptor = application_preamble();
        append_globals(&descriptor, 1U, 1U);
        for (std::uint32_t field = 0U; field < 257U; ++field) {
            descriptor.insert(descriptor.end(), {0x09U, 0x01U, 0x81U, 0x02U});
        }
        descriptor.push_back(0xC0U);
        const aoahid_raw_report report = idless_report(33U);
        std::uint8_t trailing_mask = 0U;
        aoa::hid::DescriptorRequirements requirements{};
        const aoa::hid::ValidationIssue result = aoa::hid::validate_raw(
            descriptor.data(), descriptor.size(), &report, 1U, &trailing_mask, &requirements);
        AOAHID_CHECK(result.result == AOAHID_OK);
        AOAHID_CHECK(requirements.maximum_fields_per_report == 257U);
        AOAHID_CHECK(requirements.maximum_global_stack_depth == 0U);
        AOAHID_CHECK(requirements.maximum_report_size_bits == 1U);
        AOAHID_CHECK(requirements.maximum_usages == 1U);
        AOAHID_CHECK(requirements.maximum_report_data_bits == 257U);
        AOAHID_CHECK(trailing_mask == 0x01U);
    }
    {
        auto descriptor = application_preamble();
        descriptor.insert(descriptor.end(), {0x85U, 0x01U});
        append_globals(&descriptor, 1U, 1U);
        for (std::uint32_t field = 0U; field < 2U; ++field) {
            descriptor.insert(descriptor.end(), {0x09U, 0x01U, 0x81U, 0x02U});
        }
        descriptor.insert(descriptor.end(), {0x85U, 0x02U});
        for (std::uint32_t field = 0U; field < 3U; ++field) {
            descriptor.insert(descriptor.end(), {0x09U, 0x01U, 0x81U, 0x02U});
        }
        descriptor.push_back(0xC0U);
        const std::array<aoahid_raw_report, 2U> reports{aoahid_raw_report{{1U, 1U, {}}, 2U},
                                                        aoahid_raw_report{{1U, 2U, {}}, 2U}};
        aoa::hid::DescriptorRequirements requirements{};
        const aoa::hid::ValidationIssue result =
            aoa::hid::validate_raw(descriptor.data(), descriptor.size(), reports.data(),
                                   reports.size(), nullptr, &requirements);
        AOAHID_CHECK(result.result == AOAHID_OK);
        AOAHID_CHECK(requirements.maximum_fields_per_report == 3U);
        AOAHID_CHECK(requirements.maximum_global_stack_depth == 0U);
        AOAHID_CHECK(requirements.maximum_report_size_bits == 1U);
        AOAHID_CHECK(requirements.maximum_usages == 1U);
        AOAHID_CHECK(requirements.maximum_report_data_bits == 3U);
    }
    {
        auto descriptor = application_preamble();
        // Global Push/Pop are zero-byte short items. Five balanced levels are
        // valid descriptor state for a target-neutral Spec, but exceed the
        // selected target revision's HID_GLOBAL_STACK_SIZE policy of four.
        descriptor.insert(descriptor.end(),
                          {0xA4U, 0xA4U, 0xA4U, 0xA4U, 0xA4U, 0xB4U, 0xB4U, 0xB4U, 0xB4U, 0xB4U});
        append_globals(&descriptor, 1U, 1U);
        descriptor.insert(descriptor.end(), {0x09U, 0x01U, 0x81U, 0x02U, 0xC0U});
        const aoahid_raw_report report = idless_report(1U);
        aoa::hid::DescriptorRequirements requirements{};
        const aoa::hid::ValidationIssue result = aoa::hid::validate_raw(
            descriptor.data(), descriptor.size(), &report, 1U, nullptr, &requirements);
        AOAHID_CHECK(result.result == AOAHID_OK);
        AOAHID_CHECK(requirements.maximum_fields_per_report == 1U);
        AOAHID_CHECK(requirements.maximum_global_stack_depth == 5U);
        AOAHID_CHECK(requirements.maximum_report_size_bits == 1U);
        AOAHID_CHECK(requirements.maximum_usages == 1U);
        AOAHID_CHECK(requirements.maximum_report_data_bits == 1U);
    }
    {
        auto descriptor = application_preamble();
        // The exact target rejects a Report Size declaration above 256 even
        // if a later Global item overwrites it before Input. A target-neutral
        // Spec records the declaration so Node registration can apply policy.
        descriptor.insert(descriptor.end(), {0x77U, 0x01U, 0x01U, 0x00U, 0x00U});
        append_globals(&descriptor, 1U, 1U);
        descriptor.insert(descriptor.end(), {0x09U, 0x01U, 0x81U, 0x02U, 0xC0U});
        const aoahid_raw_report report = idless_report(1U);
        aoa::hid::DescriptorRequirements requirements{};
        const aoa::hid::ValidationIssue result = aoa::hid::validate_raw(
            descriptor.data(), descriptor.size(), &report, 1U, nullptr, &requirements);
        AOAHID_CHECK(result.result == AOAHID_OK);
        AOAHID_CHECK(requirements.maximum_report_size_bits == 257U);
        AOAHID_CHECK(requirements.maximum_report_data_bits == 1U);
    }
    {
        auto descriptor = application_preamble();
        append_globals(&descriptor, 1U, 1U);
        descriptor.insert(descriptor.end(), {0x09U, 0x01U, 0x81U, 0x02U, 0xC0U});
        // Local Usage items are parsed even after the final Collection. The
        // exact target rejects the 12,289th entry; requirements must not only
        // sample Local state when a Main item appears.
        descriptor.insert(descriptor.end(), 12289U, 0x08U);
        const aoahid_raw_report report = idless_report(1U);
        aoa::hid::DescriptorRequirements requirements{};
        const aoa::hid::ValidationIssue result = aoa::hid::validate_raw(
            descriptor.data(), descriptor.size(), &report, 1U, nullptr, &requirements);
        AOAHID_CHECK(result.result == AOAHID_OK);
        AOAHID_CHECK(requirements.maximum_usages == 12289U);
    }
    {
        auto descriptor = application_preamble();
        append_globals(&descriptor, 1U, 1U);
        // The audited target's inclusive u32 loop can wrap for some Local
        // states when the raw Maximum is UINT32_MAX. The validated raw subset
        // conservatively rejects every such Maximum as project policy.
        descriptor.insert(descriptor.end(), {0x1BU, 0xFFU, 0xFFU, 0xFFU, 0xFFU, 0x2BU, 0xFFU, 0xFFU,
                                             0xFFU, 0xFFU, 0x81U, 0x02U, 0xC0U});
        const aoahid_raw_report report = idless_report(1U);
        AOAHID_CHECK(
            aoa::hid::validate_raw(descriptor.data(), descriptor.size(), &report, 1U).result ==
            AOAHID_ERR_UNSUPPORTED);
    }
}

void test_local_state_and_delimiters() {
    {
        auto descriptor = application_preamble();
        append_globals(&descriptor, 1U, 1U);
        // HID 1.11 section 6.2.2.8 does not state that Usage Maximum without
        // Usage Minimum is invalid or define a default minimum. The exact
        // target zero-initializes local state, so a lone Maximum of 3 expands
        // the raw range 0 through 3. Preserve that implementation observation
        // instead of inventing a stricter conformance rule.
        descriptor.insert(descriptor.end(), {0x29U, 0x03U, 0x81U, 0x02U, 0xC0U});
        const aoahid_raw_report report = idless_report(1U);
        aoa::hid::DescriptorRequirements requirements{};
        const aoa::hid::ValidationIssue result = aoa::hid::validate_raw(
            descriptor.data(), descriptor.size(), &report, 1U, nullptr, &requirements);
        AOAHID_CHECK(result.result == AOAHID_OK);
        AOAHID_CHECK(requirements.maximum_usages == 4U);
    }
    {
        auto descriptor = application_preamble();
        append_globals(&descriptor, 1U, 1U);
        descriptor.insert(descriptor.end(),
                          {// Branch zero Usage.
                           0x09U, 0x01U,
                           // First delimiter branch: five more Usages.
                           0xA9U, 0x01U, 0x19U, 0x0AU, 0x29U, 0x0EU, 0xA9U, 0x00U,
                           // Second branch is ignored by the audited parser.
                           0xA9U, 0x01U, 0x09U, 0x7FU, 0xA9U, 0x00U, 0x81U, 0x02U, 0xC0U});
        const aoahid_raw_report report = idless_report(1U);
        aoa::hid::DescriptorRequirements requirements{};
        const aoa::hid::ValidationIssue result = aoa::hid::validate_raw(
            descriptor.data(), descriptor.size(), &report, 1U, nullptr, &requirements);
        AOAHID_CHECK(result.result == AOAHID_OK);
        AOAHID_CHECK(requirements.maximum_fields_per_report == 1U);
        AOAHID_CHECK(requirements.maximum_usages == 6U);
    }
    {
        // The five-Usage range belongs to the Collection Main item and is
        // reset afterwards. The following constant Input is padding, not a
        // registered field.
        std::vector<std::uint8_t> descriptor{0x05U, 0x01U, 0x19U, 0x01U,
                                             0x29U, 0x05U, 0xA1U, 0x01U};
        append_globals(&descriptor, 1U, 1U);
        descriptor.insert(descriptor.end(), {0x81U, 0x01U, 0xC0U});
        const aoahid_raw_report report = idless_report(1U);
        aoa::hid::DescriptorRequirements requirements{};
        const aoa::hid::ValidationIssue result = aoa::hid::validate_raw(
            descriptor.data(), descriptor.size(), &report, 1U, nullptr, &requirements);
        AOAHID_CHECK(result.result == AOAHID_OK);
        AOAHID_CHECK(requirements.maximum_fields_per_report == 0U);
        AOAHID_CHECK(requirements.maximum_usages == 5U);
    }
    for (const std::vector<std::uint8_t>& suffix :
         {std::vector<std::uint8_t>{0xA9U, 0x01U, 0xA9U, 0x01U},
          std::vector<std::uint8_t>{0xA9U, 0x00U}}) {
        auto descriptor = application_preamble();
        descriptor.insert(descriptor.end(), suffix.begin(), suffix.end());
        descriptor.push_back(0xC0U);
        const aoahid_raw_report report = idless_report(1U);
        aoa::hid::DescriptorRequirements requirements{99U, 99U, 99U, 99U, 99U};
        const aoa::hid::ValidationIssue result = aoa::hid::validate_raw(
            descriptor.data(), descriptor.size(), &report, 1U, nullptr, &requirements);
        AOAHID_CHECK(result.result == AOAHID_ERR_PARAM);
        AOAHID_CHECK(requirements.maximum_fields_per_report == 0U);
        AOAHID_CHECK(requirements.maximum_global_stack_depth == 0U);
        AOAHID_CHECK(requirements.maximum_report_size_bits == 0U);
        AOAHID_CHECK(requirements.maximum_usages == 0U);
        AOAHID_CHECK(requirements.maximum_report_data_bits == 0U);
    }
    {
        auto descriptor = application_preamble();
        append_globals(&descriptor, 1U, 1U);
        descriptor.insert(descriptor.end(), {0x19U, 0x05U, 0x29U, 0x04U, 0x81U, 0x02U, 0xC0U});
        const aoahid_raw_report report = idless_report(1U);
        aoa::hid::DescriptorRequirements requirements{99U, 99U, 99U, 99U, 99U};
        const aoa::hid::ValidationIssue result = aoa::hid::validate_raw(
            descriptor.data(), descriptor.size(), &report, 1U, nullptr, &requirements);
        AOAHID_CHECK(result.result == AOAHID_ERR_PARAM);
        AOAHID_CHECK(requirements.maximum_fields_per_report == 0U);
        AOAHID_CHECK(requirements.maximum_global_stack_depth == 0U);
        AOAHID_CHECK(requirements.maximum_report_size_bits == 0U);
        AOAHID_CHECK(requirements.maximum_usages == 0U);
        AOAHID_CHECK(requirements.maximum_report_data_bits == 0U);
    }
    {
        // HID 1.11 section 6.2.2.8 forbids delimiter aliases for an
        // Application Collection Usage.
        const std::vector<std::uint8_t> descriptor{0x05U, 0x01U, 0xA9U, 0x01U, 0x09U, 0x06U,
                                                   0xA9U, 0x00U, 0xA1U, 0x01U, 0xC0U};
        const aoahid_raw_report report = idless_report(1U);
        AOAHID_CHECK(
            aoa::hid::validate_raw(descriptor.data(), descriptor.size(), &report, 1U).result ==
            AOAHID_ERR_PARAM);
    }
    {
        // The same section forbids delimiter aliases for Array items.
        auto descriptor = application_preamble();
        append_globals(&descriptor, 8U, 1U);
        descriptor.insert(descriptor.end(),
                          {0xA9U, 0x01U, 0x09U, 0x04U, 0xA9U, 0x00U, 0x81U, 0x00U, 0xC0U});
        const aoahid_raw_report report = idless_report(1U);
        AOAHID_CHECK(
            aoa::hid::validate_raw(descriptor.data(), descriptor.size(), &report, 1U).result ==
            AOAHID_ERR_PARAM);
    }
    {
        // A delimiter set brackets alternatives and must close before the
        // consuming Main item.
        auto descriptor = application_preamble();
        append_globals(&descriptor, 1U, 1U);
        descriptor.insert(descriptor.end(), {0xA9U, 0x01U, 0x09U, 0x01U, 0x81U, 0x02U, 0xC0U});
        const aoahid_raw_report report = idless_report(1U);
        AOAHID_CHECK(
            aoa::hid::validate_raw(descriptor.data(), descriptor.size(), &report, 1U).result ==
            AOAHID_ERR_PARAM);
    }
}

void test_raw_arithmetic_and_failure_paths() {
    {
        // With nonnegative Logical Minimum, HID 1.11 section 6.2.2.7 and
        // hid_parser_global() interpret the Maximum item as unsigned even
        // though the target stores its bit pattern in an s32.
        auto descriptor = application_preamble();
        descriptor.insert(descriptor.end(),
                          {0x15U, 0x00U, 0x27U, 0xFFU, 0xFFU, 0xFFU, 0xFFU, 0x75U, 0x20U, 0x95U,
                           0x01U, 0x09U, 0x01U, 0x81U, 0x02U, 0xC0U});
        const aoahid_raw_report report = idless_report(4U);
        AOAHID_CHECK(
            aoa::hid::validate_raw(descriptor.data(), descriptor.size(), &report, 1U).result ==
            AOAHID_OK);

        descriptor[14U] = 0x1FU; // Report Size 31 cannot represent 0xffffffff.
        AOAHID_CHECK(
            aoa::hid::validate_raw(descriptor.data(), descriptor.size(), &report, 1U).result ==
            AOAHID_ERR_PARAM);
    }
    {
        auto descriptor = application_preamble();
        descriptor.insert(descriptor.end(),
                          {0x15U, 0xFFU, 0x27U, 0xFFU, 0xFFU, 0xFFU, 0xFFU, 0x75U, 0x20U, 0x95U,
                           0x01U, 0x09U, 0x01U, 0x81U, 0x02U, 0xC0U});
        const aoahid_raw_report report = idless_report(4U);
        // Minimum -1 makes the four-byte Maximum signed, so 0xffffffff is -1.
        AOAHID_CHECK(
            aoa::hid::validate_raw(descriptor.data(), descriptor.size(), &report, 1U).result ==
            AOAHID_OK);
        // A signed 0x80000000 Maximum is below -1.
        descriptor[9U] = 0x00U;
        descriptor[10U] = 0x00U;
        descriptor[11U] = 0x00U;
        descriptor[12U] = 0x80U;
        AOAHID_CHECK(
            aoa::hid::validate_raw(descriptor.data(), descriptor.size(), &report, 1U).result ==
            AOAHID_ERR_PARAM);
    }
    {
        auto descriptor = application_preamble();
        append_globals(&descriptor, 33U, 1U);
        descriptor.insert(descriptor.end(), {0x09U, 0x01U, 0x81U, 0x01U, 0xC0U});
        const aoahid_raw_report report = idless_report(5U);
        aoa::hid::DescriptorRequirements requirements{99U, 99U, 99U, 99U, 99U};
        const aoa::hid::ValidationIssue result = aoa::hid::validate_raw(
            descriptor.data(), descriptor.size(), &report, 1U, nullptr, &requirements);
        AOAHID_CHECK(result.result == AOAHID_ERR_PARAM);
        AOAHID_CHECK(requirements.maximum_fields_per_report == 0U);
        AOAHID_CHECK(requirements.maximum_global_stack_depth == 0U);
        AOAHID_CHECK(requirements.maximum_report_size_bits == 0U);
        AOAHID_CHECK(requirements.maximum_usages == 0U);
        AOAHID_CHECK(requirements.maximum_report_data_bits == 0U);
    }
    {
        // The second 31-bit value begins at bit 31 and spans five bytes.
        auto descriptor = application_preamble();
        append_globals(&descriptor, 31U, 2U);
        descriptor.insert(descriptor.end(), {0x09U, 0x01U, 0x81U, 0x02U, 0xC0U});
        const aoahid_raw_report report = idless_report(8U);
        const aoa::hid::ValidationIssue result =
            aoa::hid::validate_raw(descriptor.data(), descriptor.size(), &report, 1U);
        AOAHID_CHECK(result.result == AOAHID_ERR_PARAM);
    }
    {
        auto descriptor = application_preamble();
        append_globals(&descriptor, 32U, std::numeric_limits<std::uint32_t>::max());
        descriptor.insert(descriptor.end(), {0x09U, 0x01U, 0x81U, 0x02U, 0xC0U});
        const aoahid_raw_report report = idless_report(1U);
        // On a 64-bit host the computed wire length exceeds the public u32
        // table field; on a 32-bit host the Size*Count product overflows
        // size_t first. Both paths are an arithmetic overflow, not a generic
        // table mismatch.
        AOAHID_CHECK(
            aoa::hid::validate_raw(descriptor.data(), descriptor.size(), &report, 1U).result ==
            AOAHID_ERR_OVERFLOW);
    }
    {
        auto descriptor = application_preamble();
        append_globals(&descriptor, 1U, 1U);
        descriptor.insert(descriptor.end(), {0x09U, 0x01U, 0x91U, 0x02U, 0xC0U});
        const aoahid_raw_report report = idless_report(1U);
        const aoa::hid::ValidationIssue result =
            aoa::hid::validate_raw(descriptor.data(), descriptor.size(), &report, 1U);
        AOAHID_CHECK(result.result == AOAHID_ERR_UNSUPPORTED);
    }
    {
        auto descriptor = application_preamble();
        append_globals(&descriptor, 1U, 1U);
        descriptor.insert(descriptor.end(), {0x09U, 0x01U, 0x81U, 0x02U, 0xC0U});
        // In the selected target's fetch_item(), every tag-15 prefix denotes
        // a long item, not only the canonical FE prefix.
        descriptor.insert(descriptor.end(), {0xF0U, 0x00U, 0x00U});
        const aoahid_raw_report report = idless_report(1U);
        const aoa::hid::ValidationIssue result =
            aoa::hid::validate_raw(descriptor.data(), descriptor.size(), &report, 1U);
        AOAHID_CHECK(result.result == AOAHID_ERR_UNSUPPORTED);
    }
    {
        auto descriptor = application_preamble();
        append_globals(&descriptor, 1U, 1U);
        descriptor.insert(descriptor.end(), {0x09U, 0x01U, 0x81U, 0x02U, 0xC0U});
        // hid_open_report() rejects a Local delimiter left open at EOF.
        descriptor.insert(descriptor.end(), {0xA9U, 0x01U});
        const aoahid_raw_report report = idless_report(1U);
        const aoa::hid::ValidationIssue result =
            aoa::hid::validate_raw(descriptor.data(), descriptor.size(), &report, 1U);
        AOAHID_CHECK(result.result == AOAHID_ERR_PARAM);
    }
    {
        auto descriptor = application_preamble();
        // hid_add_field() checks logical ordering before it discards a
        // Constant padding item that has no local Usage.
        descriptor.insert(descriptor.end(), {0x15U, 0x02U, 0x25U, 0x01U, 0x75U, 0x08U, 0x95U, 0x01U,
                                             0x81U, 0x01U, 0xC0U});
        const aoahid_raw_report report = idless_report(1U);
        const aoa::hid::ValidationIssue result =
            aoa::hid::validate_raw(descriptor.data(), descriptor.size(), &report, 1U);
        AOAHID_CHECK(result.result == AOAHID_ERR_PARAM);
    }
    {
        auto descriptor = application_preamble();
        // HID 1.11 section 6.2.2.7 reserves Global tags 12 through 15;
        // C5 00 is a one-byte tag-12 Global item. hid_parser_global() in the
        // exact audited target revision rejects unknown Global tags.
        descriptor.insert(descriptor.end(), {0xC5U, 0x00U});
        append_globals(&descriptor, 1U, 1U);
        descriptor.insert(descriptor.end(), {0x09U, 0x01U, 0x81U, 0x02U, 0xC0U});
        const aoahid_raw_report report = idless_report(1U);
        const aoa::hid::ValidationIssue result =
            aoa::hid::validate_raw(descriptor.data(), descriptor.size(), &report, 1U);
        AOAHID_CHECK(result.result == AOAHID_ERR_PARAM);
    }
    {
        auto descriptor = application_preamble();
        // HID 1.11 section 6.2.2.7 defines tags 3 through 6 as Physical
        // Minimum, Physical Maximum, Unit Exponent, and Unit. The exact target
        // hid_parser_global() accepts each of them.
        descriptor.insert(descriptor.end(),
                          {0x35U, 0x00U, 0x45U, 0x01U, 0x55U, 0x0CU, 0x65U, 0x11U});
        append_globals(&descriptor, 1U, 1U);
        descriptor.insert(descriptor.end(), {0x09U, 0x01U, 0x81U, 0x02U, 0xC0U});
        const aoahid_raw_report report = idless_report(1U);
        const aoa::hid::ValidationIssue result =
            aoa::hid::validate_raw(descriptor.data(), descriptor.size(), &report, 1U);
        AOAHID_CHECK(result.result == AOAHID_OK);
    }
    {
        auto descriptor = application_preamble();
        append_globals(&descriptor, 1U, 1U);
        // Unit None before the Main item removes the HUT 1.7 section 3.3
        // metadata condition; a transient earlier Unit does not qualify the
        // later Input after Global state is explicitly reset to zero.
        descriptor.insert(descriptor.end(),
                          {0x65U, 0x11U, 0x65U, 0x00U, 0x09U, 0x01U, 0x81U, 0x02U, 0xC0U});
        const aoahid_raw_report report = idless_report(1U);
        AOAHID_CHECK(
            aoa::hid::validate_raw(descriptor.data(), descriptor.size(), &report, 1U).result ==
            AOAHID_OK);
    }
    {
        auto descriptor = application_preamble();
        append_globals(&descriptor, 1U, 1U);
        // HUT 1.7 section 3.3 requires Physical Minimum, Physical Maximum,
        // and Unit Exponent when a non-None Unit qualifies any data Main item,
        // including Constant padding with no local Usage.
        descriptor.insert(descriptor.end(), {0x65U, 0x11U, 0x81U, 0x01U, 0xC0U});
        const aoahid_raw_report report = idless_report(1U);
        AOAHID_CHECK(
            aoa::hid::validate_raw(descriptor.data(), descriptor.size(), &report, 1U).result ==
            AOAHID_ERR_PARAM);
    }
    for (std::uint32_t omitted = 0U; omitted < 3U; ++omitted) {
        auto descriptor = application_preamble();
        append_globals(&descriptor, 1U, 1U);
        if (omitted != 0U) {
            descriptor.insert(descriptor.end(), {0x35U, 0x00U});
        }
        if (omitted != 1U) {
            descriptor.insert(descriptor.end(), {0x45U, 0x01U});
        }
        if (omitted != 2U) {
            descriptor.insert(descriptor.end(), {0x55U, 0x0CU});
        }
        descriptor.insert(descriptor.end(), {0x65U, 0x11U, 0x09U, 0x01U, 0x81U, 0x02U, 0xC0U});
        const aoahid_raw_report report = idless_report(1U);
        AOAHID_CHECK(
            aoa::hid::validate_raw(descriptor.data(), descriptor.size(), &report, 1U).result ==
            AOAHID_ERR_PARAM);
    }
    {
        auto descriptor = application_preamble();
        append_globals(&descriptor, 1U, 1U);
        descriptor.insert(descriptor.end(),
                          {0x35U, 0x00U, 0x45U, 0x01U, 0x55U, 0x0CU, 0x65U, 0x11U, 0xA4U, 0x65U,
                           0x00U, 0xB4U, 0x09U, 0x01U, 0x81U, 0x02U, 0xC0U});
        const aoahid_raw_report report = idless_report(1U);
        AOAHID_CHECK(
            aoa::hid::validate_raw(descriptor.data(), descriptor.size(), &report, 1U).result ==
            AOAHID_OK);
    }
    {
        auto descriptor = application_preamble();
        // The target tolerates legacy full-byte sign extension, but HID 1.11
        // section 6.2.2.7 defines the Unit Exponent as a four-bit code.
        descriptor.insert(descriptor.end(), {0x55U, 0xFCU});
        append_globals(&descriptor, 1U, 1U);
        descriptor.insert(descriptor.end(), {0x09U, 0x01U, 0x81U, 0x02U, 0xC0U});
        const aoahid_raw_report report = idless_report(1U);
        AOAHID_CHECK(
            aoa::hid::validate_raw(descriptor.data(), descriptor.size(), &report, 1U).result ==
            AOAHID_ERR_PARAM);
    }
}

void test_required_control_items_and_collection_usage() {
    const aoahid_raw_report report = idless_report(1U);
    const auto check_rejected = [&report](const std::vector<std::uint8_t>& descriptor) {
        AOAHID_CHECK(
            aoa::hid::validate_raw(descriptor.data(), descriptor.size(), &report, 1U).result ==
            AOAHID_ERR_PARAM);
    };

    // HID 1.11 section 6.2.2.6: every Collection has an associated Usage.
    check_rejected({0x05U, 0x01U, 0xA1U, 0x01U, 0xC0U});

    // HID 1.11 section 6.2.2.2: each non-constant control-data Main item
    // needs Usage Page, Usage, Logical Minimum, Logical Maximum, Report Size,
    // and Report Count. The collection preamble consumes its own local Usage.
    for (const std::vector<std::uint8_t>& globals :
         {// Missing Usage for the Input item.
          std::vector<std::uint8_t>{0x15U, 0x00U, 0x25U, 0x01U, 0x75U, 0x01U, 0x95U, 0x01U},
          // Missing explicit Logical Minimum.
          std::vector<std::uint8_t>{0x09U, 0x01U, 0x25U, 0x01U, 0x75U, 0x01U, 0x95U, 0x01U},
          // Missing explicit Logical Maximum.
          std::vector<std::uint8_t>{0x09U, 0x01U, 0x15U, 0x00U, 0x75U, 0x01U, 0x95U, 0x01U}}) {
        auto descriptor = application_preamble();
        descriptor.insert(descriptor.end(), globals.begin(), globals.end());
        descriptor.insert(descriptor.end(), {0x81U, 0x02U, 0xC0U});
        check_rejected(descriptor);
    }

    // A four-byte Usage embeds its Usage Page, but HID 1.11 section 6.2.2.2
    // still lists the Usage Page item itself as required descriptor state.
    const std::vector<std::uint8_t> missing_usage_page{
        0x0BU, 0x06U, 0x00U, 0x01U, 0x00U, 0xA1U, 0x01U, 0x0BU, 0x01U, 0x00U, 0x01U, 0x00U,
        0x15U, 0x00U, 0x25U, 0x01U, 0x75U, 0x01U, 0x95U, 0x01U, 0x81U, 0x02U, 0xC0U};
    check_rejected(missing_usage_page);
}

void test_strict_hid_encoding_rules() {
    {
        auto descriptor = application_preamble();
        append_globals(&descriptor, 1U, 1U);
        // The specified target expands a short Usage range from the raw Local
        // item values, then hid_add_usage() concatenates the Usage Page. The
        // loop is therefore fffe..ffff (two entries), not a 32-bit wrap.
        descriptor.insert(descriptor.end(), {0x06U, 0xFFU, 0xFFU, 0x1AU, 0xFEU, 0xFFU, 0x2AU, 0xFFU,
                                             0xFFU, 0x81U, 0x02U, 0xC0U});
        const aoahid_raw_report report = idless_report(1U);
        aoa::hid::DescriptorRequirements requirements{};
        AOAHID_CHECK(aoa::hid::validate_raw(descriptor.data(), descriptor.size(), &report, 1U,
                                            nullptr, &requirements)
                         .result == AOAHID_OK);
        AOAHID_CHECK(requirements.maximum_usages == 2U);
    }
    {
        auto descriptor = application_preamble();
        append_globals(&descriptor, 1U, 1U);
        // HID 1.11 gives the short Minimum page 1 and the extended Maximum an
        // embedded page 1. The specified target nevertheless expands its raw
        // item_udata range 1..00010002 before adding pages. Record that exact
        // parser requirement so its 12,288-Usage policy can reject it before
        // registration instead of understating it as two entries.
        descriptor.insert(descriptor.end(),
                          {0x19U, 0x01U, 0x2BU, 0x02U, 0x00U, 0x01U, 0x00U, 0x81U, 0x02U, 0xC0U});
        const aoahid_raw_report report = idless_report(1U);
        aoa::hid::DescriptorRequirements requirements{};
        AOAHID_CHECK(aoa::hid::validate_raw(descriptor.data(), descriptor.size(), &report, 1U,
                                            nullptr, &requirements)
                         .result == AOAHID_OK);
        AOAHID_CHECK(requirements.maximum_usages == 65538U);
    }
    {
        auto descriptor = application_preamble();
        append_globals(&descriptor, 1U, 1U);
        descriptor.insert(descriptor.end(),
                          {0x1BU, 0x01U, 0x00U, 0x01U, 0x00U, 0x29U, 0x02U, 0x81U, 0x02U, 0xC0U});
        const aoahid_raw_report report = idless_report(1U);
        AOAHID_CHECK(
            aoa::hid::validate_raw(descriptor.data(), descriptor.size(), &report, 1U).result ==
            AOAHID_ERR_PARAM);
    }
    for (const std::vector<std::uint8_t>& input :
         {std::vector<std::uint8_t>{0x82U, 0x80U, 0x00U},
          std::vector<std::uint8_t>{0x82U, 0x00U, 0x02U}}) {
        auto descriptor = application_preamble();
        append_globals(&descriptor, 1U, 1U);
        descriptor.push_back(0x09U);
        descriptor.push_back(0x01U);
        descriptor.insert(descriptor.end(), input.begin(), input.end());
        descriptor.push_back(0xC0U);
        const aoahid_raw_report report = idless_report(1U);
        AOAHID_CHECK(
            aoa::hid::validate_raw(descriptor.data(), descriptor.size(), &report, 1U).result ==
            AOAHID_ERR_PARAM);
    }
    {
        auto descriptor = application_preamble();
        append_globals(&descriptor, 1U, 1U);
        descriptor.insert(descriptor.end(), {0x09U, 0x01U, 0x81U, 0x02U, 0x75U, 0x08U, 0x95U, 0x01U,
                                             0x09U, 0x02U, 0x82U, 0x00U, 0x01U, 0xC0U});
        const aoahid_raw_report report = idless_report(2U);
        AOAHID_CHECK(
            aoa::hid::validate_raw(descriptor.data(), descriptor.size(), &report, 1U).result ==
            AOAHID_ERR_PARAM);
    }
    {
        auto descriptor = application_preamble();
        append_globals(&descriptor, 8U, 1U);
        descriptor.insert(descriptor.end(), {0x09U, 0x01U, 0x82U, 0x00U, 0x01U, 0xC0U});
        const aoahid_raw_report report = idless_report(1U);
        AOAHID_CHECK(
            aoa::hid::validate_raw(descriptor.data(), descriptor.size(), &report, 1U).result ==
            AOAHID_OK);
    }
    for (const std::vector<std::uint8_t>& delimited :
         {std::vector<std::uint8_t>{0xA9U, 0x02U, 0x09U, 0x01U, 0xA9U, 0x00U, 0x81U, 0x02U},
          std::vector<std::uint8_t>{0xA9U, 0x01U, 0x09U, 0x01U, 0xA9U, 0x00U, 0x81U, 0x01U}}) {
        auto descriptor = application_preamble();
        append_globals(&descriptor, 1U, 1U);
        descriptor.insert(descriptor.end(), delimited.begin(), delimited.end());
        descriptor.push_back(0xC0U);
        const aoahid_raw_report report = idless_report(1U);
        AOAHID_CHECK(
            aoa::hid::validate_raw(descriptor.data(), descriptor.size(), &report, 1U).result ==
            AOAHID_ERR_PARAM);
    }

    AOAHID_CHECK(aoa::hid::unit_is_valid(0U));
    AOAHID_CHECK(aoa::hid::unit_is_valid(0x11U));
    AOAHID_CHECK(aoa::hid::unit_is_valid(0x14U));
    AOAHID_CHECK(aoa::hid::unit_is_valid(0x0FU));
    AOAHID_CHECK(!aoa::hid::unit_is_valid(0x05U));
    AOAHID_CHECK(!aoa::hid::unit_is_valid(0x10000011U));
    for (const std::vector<std::uint8_t>& unit :
         {std::vector<std::uint8_t>{0x65U, 0x05U},
          std::vector<std::uint8_t>{0x67U, 0x11U, 0x00U, 0x00U, 0x10U}}) {
        auto descriptor = application_preamble();
        descriptor.insert(descriptor.end(), unit.begin(), unit.end());
        append_globals(&descriptor, 1U, 1U);
        descriptor.insert(descriptor.end(), {0x09U, 0x01U, 0x81U, 0x02U, 0xC0U});
        const aoahid_raw_report report = idless_report(1U);
        AOAHID_CHECK(
            aoa::hid::validate_raw(descriptor.data(), descriptor.size(), &report, 1U).result ==
            AOAHID_ERR_PARAM);
    }
    {
        auto descriptor = application_preamble();
        append_globals(&descriptor, 1U, 1U);
        descriptor.insert(descriptor.end(), {0x35U, 0x00U, 0x45U, 0x01U, 0x55U, 0x00U, 0x65U, 0x0FU,
                                             0x09U, 0x01U, 0x81U, 0x02U, 0xC0U});
        const aoahid_raw_report report = idless_report(1U);
        AOAHID_CHECK(
            aoa::hid::validate_raw(descriptor.data(), descriptor.size(), &report, 1U).result ==
            AOAHID_OK);
    }

    const auto nested_collection = [](const std::vector<std::uint8_t>& collection) {
        auto descriptor = application_preamble();
        descriptor.insert(descriptor.end(), {0x09U, 0x01U});
        descriptor.insert(descriptor.end(), collection.begin(), collection.end());
        append_globals(&descriptor, 1U, 1U);
        descriptor.insert(descriptor.end(), {0x09U, 0x01U, 0x81U, 0x02U, 0xC0U, 0xC0U});
        return descriptor;
    };
    for (const std::vector<std::uint8_t>& collection :
         {std::vector<std::uint8_t>{0xA1U, 0x07U},
          std::vector<std::uint8_t>{0xA2U, 0x00U, 0x01U}}) {
        const auto descriptor = nested_collection(collection);
        const aoahid_raw_report report = idless_report(1U);
        AOAHID_CHECK(
            aoa::hid::validate_raw(descriptor.data(), descriptor.size(), &report, 1U).result ==
            AOAHID_ERR_PARAM);
    }
    {
        const auto descriptor = nested_collection({0xA1U, 0x80U});
        const aoahid_raw_report report = idless_report(1U);
        AOAHID_CHECK(
            aoa::hid::validate_raw(descriptor.data(), descriptor.size(), &report, 1U).result ==
            AOAHID_OK);
    }
    {
        auto descriptor = application_preamble();
        append_globals(&descriptor, 1U, 1U);
        // HID 1.11 section 6.2.2.4 reserves Main tags other than Input,
        // Output, Feature, Collection, and End Collection.
        descriptor.insert(descriptor.end(), {0x09U, 0x01U, 0xD0U, 0x81U, 0x02U, 0xC0U});
        const aoahid_raw_report report = idless_report(1U);
        AOAHID_CHECK(
            aoa::hid::validate_raw(descriptor.data(), descriptor.size(), &report, 1U).result ==
            AOAHID_ERR_PARAM);
    }
    {
        auto descriptor = application_preamble();
        append_globals(&descriptor, 1U, 1U);
        // Designator Index (tag 3) and String Index (tag 7) are defined Local
        // items. The selected target ignores both, which does not change the
        // Input report layout.
        descriptor.insert(descriptor.end(),
                          {0x39U, 0x01U, 0x79U, 0x01U, 0x09U, 0x01U, 0x81U, 0x02U, 0xC0U});
        const aoahid_raw_report report = idless_report(1U);
        AOAHID_CHECK(
            aoa::hid::validate_raw(descriptor.data(), descriptor.size(), &report, 1U).result ==
            AOAHID_OK);

        // Local tag 6 is unassigned by HID 1.11 section 6.2.2.8.
        descriptor.insert(descriptor.end() - 1, {0x69U, 0x00U});
        AOAHID_CHECK(
            aoa::hid::validate_raw(descriptor.data(), descriptor.size(), &report, 1U).result ==
            AOAHID_ERR_PARAM);
    }
    {
        auto descriptor = application_preamble();
        append_globals(&descriptor, 1U, 1U);
        // bType 3 is reserved by HID 1.11 section 6.2.2.2. The audited Linux
        // parser ignores it, but the strict raw subset must not bless it.
        descriptor.insert(descriptor.end(), {0x0DU, 0x00U, 0x09U, 0x01U, 0x81U, 0x02U, 0xC0U});
        const aoahid_raw_report report = idless_report(1U);
        AOAHID_CHECK(
            aoa::hid::validate_raw(descriptor.data(), descriptor.size(), &report, 1U).result ==
            AOAHID_ERR_PARAM);
    }
}

void test_generated_collection_balance() {
    {
        aoa::hid::DescriptorBuilder builder(nullptr, 64U, nullptr);
        AOAHID_CHECK(!builder.begin_logical(0x01U, 0x01U));
        AOAHID_CHECK(!builder.good());
    }
    {
        aoa::hid::DescriptorBuilder builder(nullptr, 64U, nullptr);
        AOAHID_CHECK(!builder.end_collection());
        AOAHID_CHECK(!builder.good());
    }
    {
        aoa::hid::DescriptorBuilder builder(nullptr, 64U, nullptr);
        AOAHID_CHECK(builder.begin_application(0x01U, 0x06U));
        builder.finish();
        AOAHID_CHECK(!builder.good());
    }
    {
        aoa::hid::DescriptorBuilder builder(nullptr, 64U, nullptr);
        AOAHID_CHECK(builder.begin_application(0x01U, 0x06U));
        AOAHID_CHECK(builder.end_collection());
        builder.finish();
        AOAHID_CHECK(builder.good());
    }
    {
        aoa::hid::DescriptorBuilder builder(nullptr, 64U, nullptr);
        AOAHID_CHECK(builder.begin_application(0x01U, 0x06U));
        AOAHID_CHECK(builder.set_report_id(true, 1U));
        AOAHID_CHECK(!builder.set_report_id(false, 0U));
        AOAHID_CHECK(!builder.good());
    }
    {
        aoa::hid::DescriptorBuilder builder(nullptr, 64U, nullptr);
        AOAHID_CHECK(builder.begin_application(0x01U, 0x06U));
        AOAHID_CHECK(builder.set_report_id(true, 1U));
        AOAHID_CHECK(!builder.set_report_id(true, 2U));
        AOAHID_CHECK(!builder.good());
    }
    {
        aoa::hid::DescriptorBuilder builder(nullptr, 64U, nullptr);
        AOAHID_CHECK(builder.begin_application(0x01U, 0x06U));
        AOAHID_CHECK(!builder.constant_padding(8U));
        AOAHID_CHECK(!builder.good());
    }
    {
        std::array<std::uint8_t, 64U> descriptor{};
        aoa::hid::ReportLayout layout{};
        aoa::hid::DescriptorBuilder builder(descriptor.data(), descriptor.size(), &layout);
        AOAHID_CHECK(builder.begin_application(0x01U, 0x06U));
        AOAHID_CHECK(builder.set_report_id(true, 1U));
        AOAHID_CHECK(builder.constant_padding(8U));
        AOAHID_CHECK(builder.end_collection());
        builder.finish();
        AOAHID_CHECK(builder.good());
        AOAHID_CHECK(
            aoa::hid::validate_generated(descriptor.data(), builder.descriptor_size(), layout)
                .result == AOAHID_OK);
        descriptor[7U] = 2U;
        AOAHID_CHECK(
            aoa::hid::validate_generated(descriptor.data(), builder.descriptor_size(), layout)
                .result == AOAHID_ERR_INTERNAL);
    }
    {
        std::array<std::uint8_t, 128U> descriptor{};
        aoa::hid::ReportLayout layout{};
        aoa::hid::DescriptorBuilder builder(descriptor.data(), descriptor.size(), &layout);
        AOAHID_CHECK(builder.begin_application(0x01U, 0x06U));
        AOAHID_CHECK(builder.set_report_id(false, 0U));
        AOAHID_CHECK(builder.feature_static_value(0x01U, 0x01U, 0, 1, 1U));
        AOAHID_CHECK(builder.feature_static_value(0x01U, 0x02U, 0, 1, 32U));
        AOAHID_CHECK(builder.end_collection());
        builder.finish();
        AOAHID_CHECK(builder.good());
        // The 32-bit Feature begins at bit one and therefore spans five
        // bytes, which HID 1.11 section 8.4 forbids just as it does for Input.
        AOAHID_CHECK(
            aoa::hid::validate_generated(descriptor.data(), builder.descriptor_size(), layout)
                .result == AOAHID_ERR_INTERNAL);
    }
}

} // namespace

void test_item_writer() {
    {
        std::array<std::uint8_t, 8> bytes{};
        aoa::hid::ItemWriter writer(bytes.data(), bytes.size());
        AOAHID_CHECK(writer.logical_minimum(128));
        AOAHID_CHECK(writer.size() == 3U);
        AOAHID_CHECK(bytes[0] == 0x16U && bytes[1] == 0x80U && bytes[2] == 0x00U);
    }
    {
        std::array<std::uint8_t, 8> bytes{};
        aoa::hid::ItemWriter writer(bytes.data(), bytes.size());
        AOAHID_CHECK(writer.logical_minimum(0));
        AOAHID_CHECK(writer.logical_maximum(65535, 0));
        AOAHID_CHECK(writer.size() == 5U);
        AOAHID_CHECK(bytes[0] == 0x15U && bytes[1] == 0x00U);
        AOAHID_CHECK(bytes[2] == 0x26U && bytes[3] == 0xFFU && bytes[4] == 0xFFU);
    }
    {
        std::array<std::uint8_t, 4> bytes{};
        aoa::hid::ItemWriter writer(bytes.data(), bytes.size());
        AOAHID_CHECK(writer.unit_exponent(-4));
        AOAHID_CHECK(writer.size() == 2U);
        AOAHID_CHECK(bytes[0] == 0x55U && bytes[1] == 0x0CU);
    }
    {
        std::array<std::uint8_t, 1U> bytes{0xA5U};
        aoa::hid::ItemWriter writer(bytes.data(), bytes.size());
        AOAHID_CHECK(!writer.usage(0x100U));
        AOAHID_CHECK(writer.size() == 0U);
        AOAHID_CHECK(bytes[0] == 0xA5U);
        AOAHID_CHECK(!writer.push());
        AOAHID_CHECK(writer.size() == 0U);
        AOAHID_CHECK(bytes[0] == 0xA5U);
    }
    AOAHID_CHECK(aoa::hid::value_fits(0, 7, 4U));
    AOAHID_CHECK(!aoa::hid::value_fits(-9, 7, 4U));
    // The generated-profile API carries signed int32 extrema; raw HID item
    // signedness is tested separately through validate_raw().
    AOAHID_CHECK(!aoa::hid::value_fits(0, -1, 32U));
    test_target_neutral_raw_requirements();
    test_local_state_and_delimiters();
    test_raw_arithmetic_and_failure_paths();
    test_required_control_items_and_collection_usage();
    test_strict_hid_encoding_rules();
    test_generated_collection_balance();
}
