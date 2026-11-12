// SPDX-License-Identifier: MIT
// Copyright (c) 2026 libaoahid contributors
/*
 * One bounded libFuzzer process exercises the three pure paths required by the
 * CI design: descriptor construction, report serialization, and raw-descriptor
 * validation. It has no USB side effects and asserts only memory safety.
 */

#include "aoahid.h"
#include "api/internal.hpp"
#include "hid/descriptor_builder.hpp"
#include "hid/usages.hpp"
#include "hid/validator.hpp"

#include <array>
#include <cstddef>
#include <cstdint>
#include <utility>

namespace {

void fuzz_builder(const std::uint8_t* data, const std::size_t size) {
    std::array<std::uint8_t, 512U> descriptor{};
    aoa::hid::ReportLayout layout{};
    aoa::hid::DescriptorBuilder builder(descriptor.data(), descriptor.size(), &layout);
    const std::uint8_t selector = size == 0U ? 0U : data[0];
    static_cast<void>(
        builder.begin_application(aoa::hid::usage::page_generic_desktop, aoa::hid::usage::mouse));
    static_cast<void>(builder.set_report_id((selector & 1U) != 0U,
                                            static_cast<std::uint8_t>(1U + selector % 254U)));
    static_cast<void>(
        builder.begin_physical(aoa::hid::usage::page_generic_desktop, aoa::hid::usage::pointer));
    static_cast<void>(builder.variable_range(aoa::hid::usage::page_button, 1U,
                                             static_cast<std::uint16_t>(1U + selector % 32U), 1U,
                                             aoa::hid::FieldSemantic::buttons));
    static_cast<void>(builder.constant_padding(static_cast<std::uint16_t>(selector % 8U)));
    static_cast<void>(builder.variable(aoa::hid::usage::page_generic_desktop, aoa::hid::usage::x,
                                       -32767, 32767, 16U, (selector & 2U) != 0U, false,
                                       aoa::hid::FieldSemantic::x, 0U));
    static_cast<void>(builder.variable(aoa::hid::usage::page_generic_desktop, aoa::hid::usage::y,
                                       -32767, 32767, 16U, (selector & 4U) != 0U, false,
                                       aoa::hid::FieldSemantic::y, 0U));
    static_cast<void>(builder.end_collection());
    static_cast<void>(builder.end_collection());
    builder.finish();
    if (builder.good()) {
        static_cast<void>(
            aoa::hid::validate_generated(descriptor.data(), builder.descriptor_size(), layout));
    }
}

void fuzz_serializer(const std::uint8_t* data, const std::size_t size) {
    aoahid_mouse_options options{};
    options.struct_size = static_cast<std::uint32_t>(sizeof(options));
    options.button_count = 8U;
    options.x = {-32767, 32767, 16U, {}};
    options.y = {-32767, 32767, 16U, {}};
    options.enable_wheel = 1U;
    options.wheel = {-127, 127, 8U, {}};
    options.enable_pan = 1U;
    options.pan = {-127, 127, 8U, {}};
    aoahid_spec* spec = nullptr;
    if (aoahid_spec_create_mouse(&options, &spec) != AOAHID_OK || spec == nullptr) {
        return;
    }

    aoa::detail::MouseState state{};
    state.buttons.resize(options.button_count);
    for (std::size_t index = 0U; index < state.buttons.size() && index < size; ++index) {
        state.buttons[index] = static_cast<std::uint8_t>(data[index] & 1U);
    }
    if (size > 8U)
        state.dx = static_cast<std::int8_t>(data[8U]);
    if (size > 9U)
        state.dy = static_cast<std::int8_t>(data[9U]);
    if (size > 10U)
        state.wheel = static_cast<std::int8_t>(data[10U]);
    if (size > 11U)
        state.pan = static_cast<std::int8_t>(data[11U]);
    aoahid_node node{};
    node.spec = spec;
    node.state = std::move(state);
    std::array<std::uint8_t, 256U> report{};
    std::size_t report_length = 0U;
    if (aoa::detail::serialize_node(&node, report.data(), report.size(), &report_length) ==
        AOAHID_OK) {
        static_cast<void>(
            aoa::detail::validate_serialized_report(spec, report.data(), report_length));
    }
    aoahid_spec_release(spec);
}

void fuzz_raw_validator(const std::uint8_t* data, const std::size_t size) {
    if (data == nullptr || size == 0U) {
        return;
    }

    constexpr std::size_t maximum_reports = 4U;
    std::array<aoahid_raw_report, maximum_reports> reports{};
    const std::size_t report_count = 1U + static_cast<std::size_t>(data[0] % maximum_reports);
    std::size_t cursor = 1U;
    for (std::size_t index = 0U; index < report_count; ++index) {
        const std::uint8_t report_id = cursor < size ? data[cursor++] : 0U;
        const std::uint8_t flags = cursor < size ? data[cursor++] : 0U;
        const std::uint8_t length = cursor < size ? data[cursor++] : 0U;
        reports[index].report_id.enabled = flags & 1U;
        reports[index].report_id.value =
            reports[index].report_id.enabled == 1U
                ? static_cast<std::uint8_t>(report_id == 0U ? 1U : report_id)
                : 0U;
        reports[index].wire_length = static_cast<std::uint32_t>(length) + 1U;
    }
    const std::uint8_t* descriptor = cursor < size ? data + cursor : data;
    const std::size_t descriptor_length = cursor < size ? size - cursor : size;
    static_cast<void>(
        aoa::hid::validate_raw(descriptor, descriptor_length, reports.data(), report_count));
}

} // namespace

extern "C" int LLVMFuzzerTestOneInput(const std::uint8_t* data, const std::size_t size) {
    if (data == nullptr || size == 0U) {
        return 0;
    }
    fuzz_builder(data, size);
    fuzz_serializer(data, size);
    fuzz_raw_validator(data, size);
    return 0;
}
