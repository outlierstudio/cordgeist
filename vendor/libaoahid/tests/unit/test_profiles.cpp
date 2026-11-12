// SPDX-License-Identifier: MIT
// Copyright (c) 2026 libaoahid contributors
/* Verifies generated profile descriptors, layouts, and state obligations in
 * memory; it does not claim target-device acceptance. */
#include "aoahid.h"
#include "api/internal.hpp"
#include "hid/descriptor_builder.hpp"
#include "hid/usages.hpp"
#include "hid/validator.hpp"
#include "test.hpp"

#include <algorithm>
#include <array>
#include <cstdint>
#include <limits>
#include <string_view>
#include <vector>

namespace {
static_assert(sizeof(aoahid_usage_semantic) == sizeof(std::int32_t));

std::uint64_t extract(const std::uint8_t* report, const aoa::hid::FieldLayout& field) {
    std::uint64_t value = 0U;
    for (std::uint8_t bit = 0U; bit < field.bit_width; ++bit) {
        const std::size_t position = field.bit_offset + bit;
        value |= static_cast<std::uint64_t>((report[position / 8U] >> (position & 7U)) & 1U) << bit;
    }
    return value;
}

const aoa::hid::FieldLayout* field(const aoahid_spec* spec, const aoa::hid::FieldSemantic semantic,
                                   const std::uint16_t instance = 0U) {
    for (const auto& candidate : spec->layout.fields) {
        if (candidate.semantic == semantic && candidate.instance == instance)
            return &candidate;
    }
    return nullptr;
}

struct PinnedLinuxPenTools {
    bool invert{};
    bool pen{};
    bool rubber{};
};

void apply_pinned_linux_pen_report(const aoahid_spec* spec, const std::uint8_t* report,
                                   PinnedLinuxPenTools* tools) {
    std::vector<const aoa::hid::FieldLayout*> ordered;
    for (const auto& candidate : spec->layout.fields) {
        if (candidate.semantic == aoa::hid::FieldSemantic::eraser ||
            candidate.semantic == aoa::hid::FieldSemantic::in_range) {
            ordered.push_back(&candidate);
        }
    }
    std::sort(ordered.begin(), ordered.end(), [](const auto* left, const auto* right) {
        return left->bit_offset < right->bit_offset;
    });
    for (const auto* candidate : ordered) {
        const bool asserted = extract(report, *candidate) != 0U;
        if (candidate->semantic == aoa::hid::FieldSemantic::eraser) {
            tools->invert = asserted;
        } else if (!asserted) {
            tools->pen = false;
            tools->rubber = false;
        } else if (tools->invert) {
            tools->rubber = true;
        } else {
            tools->pen = true;
        }
    }
}

std::size_t count_item(const std::vector<std::uint8_t>& descriptor, const std::uint8_t prefix,
                       const std::uint8_t value) {
    std::size_t count = 0U;
    for (std::size_t index = 1U; index < descriptor.size(); ++index) {
        if (descriptor[index - 1U] == prefix && descriptor[index] == value)
            ++count;
    }
    return count;
}

bool has_sequence(const std::vector<std::uint8_t>& descriptor,
                  const std::initializer_list<std::uint8_t> sequence) {
    return std::search(descriptor.begin(), descriptor.end(), sequence.begin(), sequence.end()) !=
           descriptor.end();
}

aoahid_keyboard_options keyboard_options() {
    aoahid_keyboard_options options{};
    options.struct_size = static_cast<std::uint32_t>(sizeof(options));
    options.reserved = 0U;
    options.report_id = {0U, 0U, {0U, 0U, 0U}};
    options.usage_minimum = 0x04U;
    options.usage_maximum = 0x65U;
    return options;
}

// A profile's valid options must also work with a battery report added, and
// the input side of the descriptor must not change.
template <typename Options>
void check_battery_variant(Options options,
                           aoahid_result(AOAHID_CALL* create)(const Options*, aoahid_spec**)) {
    options.report_id = {1U, 1U, {0U, 0U, 0U}};
    aoahid_spec* plain = nullptr;
    AOAHID_CHECK(create(&options, &plain) == AOAHID_OK);
    options.enable_battery = 1U;
    options.battery.report_id = {1U, 2U, {0U, 0U, 0U}};
    options.battery.strength = {1, 100, 8U, {}};
    aoahid_spec* spec = nullptr;
    AOAHID_CHECK(create(&options, &spec) == AOAHID_OK);
    if (plain != nullptr && spec != nullptr) {
        // The input side is the plain descriptor without its closing End
        // Collection; the battery report is the 16 bytes before the new one.
        AOAHID_CHECK(spec->descriptor.size() == plain->descriptor.size() + 16U);
        AOAHID_CHECK(std::equal(plain->descriptor.begin(), plain->descriptor.end() - 1,
                                spec->descriptor.begin()));
        AOAHID_CHECK(spec->layout.wire_bytes == plain->layout.wire_bytes);
        AOAHID_CHECK(!plain->has_battery && spec->has_battery &&
                     spec->battery_layout.report_id == 2U && spec->battery_layout.wire_bytes == 2U);
        aoahid_capability_manifest manifest{};
        manifest.struct_size = static_cast<std::uint32_t>(sizeof(manifest));
        AOAHID_CHECK(aoahid_spec_manifest(spec, &manifest) == AOAHID_OK);
        AOAHID_CHECK(manifest.report_count == 2U && manifest.reports[0].report_id.value == 1U &&
                     manifest.reports[1].report_id.enabled == 1U &&
                     manifest.reports[1].report_id.value == 2U &&
                     manifest.reports[1].wire_length == 2U);
        AOAHID_CHECK(aoahid_spec_manifest(plain, &manifest) == AOAHID_OK);
        AOAHID_CHECK(manifest.report_count == 1U);
    }
    aoahid_spec_release(plain);
    aoahid_spec_release(spec);
}

void test_keyboard_nkro_concurrent_keys() {
    aoahid_keyboard_options options = keyboard_options();
    aoahid_spec* spec = nullptr;
    AOAHID_CHECK(aoahid_spec_create_keyboard(&options, &spec) == AOAHID_OK);
    if (spec == nullptr)
        return;
    aoahid_node node{};
    node.spec = spec;
    aoa::detail::KeyboardState state_value{};
    const std::size_t key_count =
        static_cast<std::size_t>(options.usage_maximum - options.usage_minimum) + 1U;
    state_value.pressed_bitmap.resize(key_count);
    state_value.key_transitions.resize(key_count);
    node.state = std::move(state_value);
    AOAHID_CHECK(aoahid_keyboard_key(&node, 0xE0U, 1U) == AOAHID_OK);
    node.dirty = false;
    AOAHID_CHECK(aoahid_keyboard_key(&node, 0xE0U, 1U) == AOAHID_OK);
    AOAHID_CHECK(!node.dirty);

    // Twenty simultaneously held non-modifier keys: well beyond the legacy
    // six-slot Array limit. Full NKRO gives every Usage its own bitmap bit,
    // so there is no ErrorRollOver overflow to hit.
    constexpr std::uint16_t k_held_keys = 20U;
    for (std::uint16_t usage = 0x04U; usage < 0x04U + k_held_keys; ++usage)
        AOAHID_CHECK(aoahid_keyboard_key(&node, usage, 1U) == AOAHID_OK);

    std::array<std::uint8_t, 64> report{};
    std::size_t length = 0U;
    AOAHID_CHECK(aoa::detail::serialize_node(&node, report.data(), report.size(), &length) ==
                 AOAHID_OK);
    AOAHID_CHECK(report[0] == 0x01U);
    for (std::uint16_t usage = 0x04U; usage < 0x04U + k_held_keys; ++usage) {
        const auto* key = field(spec, aoa::hid::FieldSemantic::key_bitmap,
                                static_cast<std::uint16_t>(usage - options.usage_minimum));
        AOAHID_CHECK(key != nullptr && extract(report.data(), *key) == 1U);
    }
    aoa::detail::transfer_complete(&node, AOAHID_OK, 0);

    AOAHID_CHECK(aoahid_keyboard_key(&node, 0x04U, 0U) == AOAHID_OK);
    report.fill(0U);
    AOAHID_CHECK(aoa::detail::serialize_node(&node, report.data(), report.size(), &length) ==
                 AOAHID_OK);
    const auto* released = field(spec, aoa::hid::FieldSemantic::key_bitmap, 0U);
    AOAHID_CHECK(released != nullptr && extract(report.data(), *released) == 0U);
    for (std::uint16_t usage = 0x05U; usage < 0x04U + k_held_keys; ++usage) {
        const auto* key = field(spec, aoa::hid::FieldSemantic::key_bitmap,
                                static_cast<std::uint16_t>(usage - options.usage_minimum));
        AOAHID_CHECK(key != nullptr && extract(report.data(), *key) == 1U);
    }
    AOAHID_CHECK(aoahid_keyboard_key(&node, 0x66U, 0U) == AOAHID_ERR_PARAM);
    aoahid_spec_release(spec);
}

void test_keyboard_bitmap_and_lifecycle_transitions() {
    aoahid_keyboard_options options = keyboard_options();
    options.usage_maximum = 0x0BU;
    aoahid_spec* spec = nullptr;
    AOAHID_CHECK(aoahid_spec_create_keyboard(&options, &spec) == AOAHID_OK);
    if (spec == nullptr)
        return;

    aoahid_node node{};
    node.spec = spec;
    aoa::detail::KeyboardState state_value{};
    const std::size_t key_count =
        static_cast<std::size_t>(options.usage_maximum - options.usage_minimum) + 1U;
    state_value.pressed_bitmap.resize(key_count);
    state_value.key_transitions.resize(key_count);
    node.state = std::move(state_value);

    AOAHID_CHECK(aoahid_keyboard_key(&node, 0x06U, 1U) == AOAHID_OK);
    AOAHID_CHECK(aoahid_keyboard_key(&node, 0x06U, 0U) == AOAHID_ERR_BUSY);
    std::array<std::uint8_t, 16U> report{};
    std::size_t length = 0U;
    AOAHID_CHECK(aoa::detail::serialize_node(&node, report.data(), report.size(), &length) ==
                 AOAHID_OK);
    const auto* key = field(spec, aoa::hid::FieldSemantic::key_bitmap, 2U);
    AOAHID_CHECK(key != nullptr && extract(report.data(), *key) == 1U);
    aoa::detail::transfer_complete(&node, AOAHID_OK, 0);

    AOAHID_CHECK(aoahid_keyboard_key(&node, 0x06U, 0U) == AOAHID_OK);
    AOAHID_CHECK(aoahid_keyboard_key(&node, 0x06U, 1U) == AOAHID_ERR_BUSY);
    report.fill(0xFFU);
    AOAHID_CHECK(aoa::detail::serialize_node(&node, report.data(), report.size(), &length) ==
                 AOAHID_OK);
    AOAHID_CHECK(key != nullptr && extract(report.data(), *key) == 0U);
    aoa::detail::transfer_complete(&node, AOAHID_ERR_IO, 1);
    AOAHID_CHECK(aoahid_keyboard_key(&node, 0x06U, 1U) == AOAHID_OK);
    AOAHID_CHECK(aoahid_keyboard_key(&node, 0x06U, 0U) == AOAHID_ERR_BUSY);
    aoa::detail::transfer_complete(&node, AOAHID_OK, 0);
    AOAHID_CHECK(aoahid_keyboard_key(&node, 0x06U, 0U) == AOAHID_OK);
    aoahid_spec_release(spec);
}

void test_mouse_delta_splitting_and_consumption() {
    aoahid_mouse_options options{};
    options.struct_size = static_cast<std::uint32_t>(sizeof(options));
    options.button_count = 1U;
    options.x = {-127, 127, 8U, {}};
    options.y = {-127, 127, 8U, {}};
    options.enable_wheel = 1U;
    options.wheel = {-31, 31, 8U, {}};
    options.enable_pan = 1U;
    options.pan = {-15, 15, 8U, {}};
    aoahid_spec* spec = nullptr;
    check_battery_variant(options, &aoahid_spec_create_mouse);
    AOAHID_CHECK(aoahid_spec_create_mouse(&options, &spec) == AOAHID_OK);
    if (spec == nullptr)
        return;

    aoahid_node node{};
    node.spec = spec;
    aoa::detail::MouseState state_value{};
    state_value.buttons.resize(1U);
    state_value.button_transitions.resize(1U);
    node.state = std::move(state_value);
    AOAHID_CHECK(aoahid_mouse_move(&node, 300, -300) == AOAHID_OK);
    AOAHID_CHECK(aoahid_mouse_scroll(&node, 95, -34) == AOAHID_OK);

    const auto* x = field(spec, aoa::hid::FieldSemantic::x);
    const auto* y = field(spec, aoa::hid::FieldSemantic::y);
    const auto* wheel = field(spec, aoa::hid::FieldSemantic::wheel);
    const auto* pan = field(spec, aoa::hid::FieldSemantic::pan);
    std::array<std::uint8_t, 16U> report{};
    std::size_t length = 0U;
    const std::array<std::int32_t, 4U> expected_x{127, 127, 46, 0};
    const std::array<std::int32_t, 4U> expected_y{-127, -127, -46, 0};
    const std::array<std::int32_t, 4U> expected_wheel{31, 31, 31, 2};
    const std::array<std::int32_t, 4U> expected_pan{-15, -15, -4, 0};
    for (std::size_t index = 0U; index < expected_x.size(); ++index) {
        report.fill(0U);
        AOAHID_CHECK(aoa::detail::serialize_node(&node, report.data(), report.size(), &length) ==
                     AOAHID_OK);
        AOAHID_CHECK(x != nullptr &&
                     extract(report.data(), *x) == static_cast<std::uint8_t>(expected_x[index]));
        AOAHID_CHECK(y != nullptr &&
                     extract(report.data(), *y) == static_cast<std::uint8_t>(expected_y[index]));
        AOAHID_CHECK(wheel != nullptr && extract(report.data(), *wheel) ==
                                             static_cast<std::uint8_t>(expected_wheel[index]));
        AOAHID_CHECK(pan != nullptr && extract(report.data(), *pan) ==
                                           static_cast<std::uint8_t>(expected_pan[index]));
        aoa::detail::transfer_complete(&node, AOAHID_OK, 0);
        AOAHID_CHECK(node.dirty == (index + 1U < expected_x.size()));
    }

    report.fill(0xFFU);
    AOAHID_CHECK(aoa::detail::serialize_node(&node, report.data(), report.size(), &length) ==
                 AOAHID_OK);
    AOAHID_CHECK(x != nullptr && extract(report.data(), *x) == 0U);
    AOAHID_CHECK(y != nullptr && extract(report.data(), *y) == 0U);
    AOAHID_CHECK(wheel != nullptr && extract(report.data(), *wheel) == 0U);
    AOAHID_CHECK(pan != nullptr && extract(report.data(), *pan) == 0U);
    AOAHID_CHECK(!node.dirty);
    AOAHID_CHECK(aoahid_mouse_button(&node, 1U, 1U) == AOAHID_OK);
    AOAHID_CHECK(aoahid_mouse_button(&node, 1U, 0U) == AOAHID_ERR_BUSY);
    aoa::detail::transfer_complete(&node, AOAHID_ERR_IO, 1);
    AOAHID_CHECK(aoahid_mouse_button(&node, 1U, 0U) == AOAHID_OK);
    aoahid_spec_release(spec);
}

// One pressed/transition slot per allowed Usage, as aoahid_node_open builds it.
aoa::detail::ConsumerState consumer_state_for(const aoahid_spec* spec) {
    const auto* config = std::get_if<aoa::detail::ConsumerConfig>(&spec->config);
    aoa::detail::ConsumerState state{};
    state.pressed.resize(config->allowed_usages.size());
    state.transitions.resize(config->allowed_usages.size());
    return state;
}

void test_usage_control_semantics() {
    const std::array<std::uint16_t, 6U> usages{0x00CDU, 0x00E9U, 0x00E2U,
                                               0x0201U, 0x00B0U, 0x00BBU};
    const std::array<aoahid_usage_semantic, 6U> semantics{
        AOAHID_USAGE_ONE_SHOT,        AOAHID_USAGE_RETRIGGER,     AOAHID_USAGE_ON_OFF_MAINTAINED,
        AOAHID_USAGE_SELECTOR_BITMAP, AOAHID_USAGE_ON_OFF_TOGGLE, AOAHID_USAGE_MOMENTARY};
    // These are deliberately test markers, not claims about a target mapping.
    const std::array<const char*, 6U> event_types{"TEST_EVENT_TYPE", "TEST_EVENT_TYPE",
                                                  "TEST_EVENT_TYPE", "TEST_EVENT_TYPE",
                                                  "TEST_EVENT_TYPE", "TEST_EVENT_TYPE"};
    const std::array<const char*, 6U> event_codes{"TEST_EVENT_CODE", "TEST_EVENT_CODE",
                                                  "TEST_EVENT_CODE", "TEST_EVENT_CODE",
                                                  "TEST_EVENT_CODE", "TEST_EVENT_CODE"};

    aoahid_toggle_options options{};
    options.struct_size = static_cast<std::uint32_t>(sizeof(options));
    options.report_id = {0U, 0U, {0U, 0U, 0U}};
    options.application_page = aoa::hid::usage::page_consumer;
    options.application_usage = aoa::hid::usage::consumer_control;
    options.field_page = aoa::hid::usage::page_consumer;
    options.allowed_usages = usages.data();
    options.allowed_usage_count = usages.size();
    options.usage_semantics = semantics.data();
    options.expected_linux_event_types = event_types.data();
    options.expected_linux_codes = event_codes.data();

    aoahid_spec* spec = nullptr;
    check_battery_variant(options, &aoahid_spec_create_toggle);
    AOAHID_CHECK(aoahid_spec_create_toggle(&options, &spec) == AOAHID_OK);
    if (spec != nullptr) {
        AOAHID_CHECK(count_item(spec->descriptor, 0x81U, 0x06U) == 2U);
        AOAHID_CHECK(count_item(spec->descriptor, 0x81U, 0x02U) == 3U);
        AOAHID_CHECK(count_item(spec->descriptor, 0x81U, 0x22U) == 1U);
        for (std::uint16_t index = 0U; index < usages.size(); ++index) {
            const auto* control = field(spec, aoa::hid::FieldSemantic::consumer_usage, index);
            AOAHID_CHECK(control != nullptr && control->bit_width == 1U &&
                         control->logical_minimum == 0 && control->logical_maximum == 1);
        }

        aoahid_node node{};
        node.spec = spec;
        node.state = consumer_state_for(spec);
        AOAHID_CHECK(aoahid_toggle_usage(&node, 0x00E9U, 1U) == AOAHID_OK);
        std::array<std::uint8_t, 8U> report{};
        std::size_t report_length = 0U;
        AOAHID_CHECK(aoa::detail::serialize_node(&node, report.data(), report.size(),
                                                 &report_length) == AOAHID_OK);
        AOAHID_CHECK(report_length == 1U);
        for (std::uint16_t index = 0U; index < usages.size(); ++index) {
            const auto* control = field(spec, aoa::hid::FieldSemantic::consumer_usage, index);
            AOAHID_CHECK(control != nullptr &&
                         extract(report.data(), *control) == (index == 1U ? 1U : 0U));
        }
        AOAHID_CHECK(aoahid_toggle_usage(&node, 0x00E9U, 0U) == AOAHID_ERR_BUSY);
        aoa::detail::transfer_complete(&node, AOAHID_OK, 0);
        AOAHID_CHECK(aoahid_toggle_usage(&node, 0x00E9U, 0U) == AOAHID_OK);
        report.fill(0xFFU);
        AOAHID_CHECK(aoa::detail::serialize_node(&node, report.data(), report.size(),
                                                 &report_length) == AOAHID_OK);
        AOAHID_CHECK(report[0] == 0U);
        AOAHID_CHECK(aoahid_toggle_usage(&node, 0x00E0U, 1U) == AOAHID_ERR_PARAM);

        // Several Usages can be held at once, and each is released by name.
        // Usage 0 is not in the allow-list, so there is no release-all call.
        aoa::detail::transfer_complete(&node, AOAHID_OK, 0);
        AOAHID_CHECK(aoahid_toggle_usage(&node, 0x00CDU, 1U) == AOAHID_OK);
        AOAHID_CHECK(aoahid_toggle_usage(&node, 0x00E9U, 1U) == AOAHID_OK);
        report.fill(0U);
        AOAHID_CHECK(aoa::detail::serialize_node(&node, report.data(), report.size(),
                                                 &report_length) == AOAHID_OK);
        for (std::uint16_t index = 0U; index < usages.size(); ++index) {
            const auto* control = field(spec, aoa::hid::FieldSemantic::consumer_usage, index);
            const bool held = usages[index] == 0x00CDU || usages[index] == 0x00E9U;
            AOAHID_CHECK(control != nullptr &&
                         extract(report.data(), *control) == (held ? 1U : 0U));
        }
        aoa::detail::transfer_complete(&node, AOAHID_OK, 0);
        AOAHID_CHECK(aoahid_toggle_usage(&node, 0x00CDU, 0U) == AOAHID_OK);
        report.fill(0U);
        AOAHID_CHECK(aoa::detail::serialize_node(&node, report.data(), report.size(),
                                                 &report_length) == AOAHID_OK);
        for (std::uint16_t index = 0U; index < usages.size(); ++index) {
            const auto* control = field(spec, aoa::hid::FieldSemantic::consumer_usage, index);
            AOAHID_CHECK(control != nullptr &&
                         extract(report.data(), *control) == (usages[index] == 0x00E9U ? 1U : 0U));
        }
        aoa::detail::transfer_complete(&node, AOAHID_OK, 0);
        AOAHID_CHECK(aoahid_toggle_usage(&node, 0U, 0U) == AOAHID_ERR_PARAM);

        // The state object enforces the same accepted 1->0 lifecycle for each
        // caller-declared HUT semantic. Semantic flags stay descriptor policy;
        // the caller never hand-crafts an impossible re-trigger sequence.
        for (std::size_t selected = 0U; selected < usages.size(); ++selected) {
            aoahid_node semantic_node{};
            semantic_node.spec = spec;
            semantic_node.state = consumer_state_for(spec);
            AOAHID_CHECK(aoahid_toggle_usage(&semantic_node, usages[selected], 1U) == AOAHID_OK);
            report.fill(0U);
            AOAHID_CHECK(aoa::detail::serialize_node(&semantic_node, report.data(), report.size(),
                                                     &report_length) == AOAHID_OK);
            for (std::size_t index = 0U; index < usages.size(); ++index) {
                const auto* control = field(spec, aoa::hid::FieldSemantic::consumer_usage,
                                            static_cast<std::uint16_t>(index));
                AOAHID_CHECK(control != nullptr &&
                             extract(report.data(), *control) == (index == selected ? 1U : 0U));
            }
            AOAHID_CHECK(aoahid_toggle_usage(&semantic_node, usages[selected], 0U) ==
                         AOAHID_ERR_BUSY);
            aoa::detail::transfer_complete(&semantic_node, AOAHID_OK, 0);
            AOAHID_CHECK(aoahid_toggle_usage(&semantic_node, usages[selected], 0U) == AOAHID_OK);
            report.fill(0xFFU);
            AOAHID_CHECK(aoa::detail::serialize_node(&semantic_node, report.data(), report.size(),
                                                     &report_length) == AOAHID_OK);
            AOAHID_CHECK(aoahid_toggle_usage(&semantic_node, usages[selected], 1U) ==
                         AOAHID_ERR_BUSY);
            aoa::detail::transfer_complete(&semantic_node, AOAHID_OK, 0);
            AOAHID_CHECK(aoahid_toggle_usage(&semantic_node, usages[selected], 1U) == AOAHID_OK);
        }
    }
    aoahid_spec_release(spec);

    const auto exercise_shared_control_state = [](aoahid_spec* control_spec,
                                                  const std::uint16_t usage) {
        if (control_spec == nullptr)
            return;
        aoahid_node control_node{};
        control_node.spec = control_spec;
        control_node.state = consumer_state_for(control_spec);
        AOAHID_CHECK(aoahid_toggle_usage(&control_node, usage, 1U) == AOAHID_OK);
        std::array<std::uint8_t, 16U> control_report{};
        std::size_t control_length = 0U;
        AOAHID_CHECK(aoa::detail::serialize_node(&control_node, control_report.data(),
                                                 control_report.size(),
                                                 &control_length) == AOAHID_OK);
        const auto* selected = field(control_spec, aoa::hid::FieldSemantic::consumer_usage, 0U);
        AOAHID_CHECK(selected != nullptr && extract(control_report.data(), *selected) == 1U);
        aoa::detail::transfer_complete(&control_node, AOAHID_OK, 0);
        AOAHID_CHECK(aoahid_toggle_usage(&control_node, usage, 0U) == AOAHID_OK);
        control_report.fill(0xFFU);
        AOAHID_CHECK(aoa::detail::serialize_node(&control_node, control_report.data(),
                                                 control_report.size(),
                                                 &control_length) == AOAHID_OK);
        AOAHID_CHECK(selected != nullptr && extract(control_report.data(), *selected) == 0U);
    };

    aoahid_toggle_options invalid = options;
    invalid.usage_semantics = nullptr;
    spec = nullptr;
    AOAHID_CHECK(aoahid_spec_create_toggle(&invalid, &spec) == AOAHID_ERR_UNSET_FIELD);

    std::array<std::uint16_t, 2U> duplicate_usages{0x00CDU, 0x00CDU};
    std::array<aoahid_usage_semantic, 2U> duplicate_semantics{AOAHID_USAGE_ONE_SHOT,
                                                              AOAHID_USAGE_ONE_SHOT};
    std::array<const char*, 2U> duplicate_types{"TEST_EVENT_TYPE", "TEST_EVENT_TYPE"};
    std::array<const char*, 2U> duplicate_codes{"TEST_EVENT_CODE", "TEST_EVENT_CODE"};
    invalid = {};
    invalid.struct_size = static_cast<std::uint32_t>(sizeof(invalid));
    invalid.application_page = aoa::hid::usage::page_consumer;
    invalid.application_usage = aoa::hid::usage::consumer_control;
    invalid.field_page = aoa::hid::usage::page_consumer;
    invalid.allowed_usages = duplicate_usages.data();
    invalid.allowed_usage_count = duplicate_usages.size();
    invalid.usage_semantics = duplicate_semantics.data();
    invalid.expected_linux_event_types = duplicate_types.data();
    invalid.expected_linux_codes = duplicate_codes.data();
    AOAHID_CHECK(aoahid_spec_create_toggle(&invalid, &spec) == AOAHID_ERR_PARAM);

    const std::array<std::uint16_t, 1U> volume_usage{0x00E0U};
    const std::array<aoahid_usage_semantic, 1U> volume_semantic{AOAHID_USAGE_LINEAR};
    const std::array<const char*, 1U> volume_type{"TEST_EVENT_TYPE"};
    const std::array<const char*, 1U> volume_code{"TEST_EVENT_CODE"};
    invalid.allowed_usages = volume_usage.data();
    invalid.allowed_usage_count = volume_usage.size();
    invalid.usage_semantics = volume_semantic.data();
    invalid.expected_linux_event_types = volume_type.data();
    invalid.expected_linux_codes = volume_code.data();
    AOAHID_CHECK(aoahid_spec_create_toggle(&invalid, &spec) == AOAHID_ERR_UNSUPPORTED);

    std::array<aoahid_usage_semantic, 1U> invalid_semantic{-1};
    invalid.usage_semantics = invalid_semantic.data();
    AOAHID_CHECK(aoahid_spec_create_toggle(&invalid, &spec) == AOAHID_ERR_PARAM);

    const std::array<std::uint16_t, 1U> collection_usage{0x0001U};
    invalid_semantic[0] = AOAHID_USAGE_SELECTOR_BITMAP;
    invalid.allowed_usages = collection_usage.data();
    AOAHID_CHECK(aoahid_spec_create_toggle(&invalid, &spec) == AOAHID_ERR_PARAM);

    const std::array<std::uint16_t, 2U> camera_usages{0x20U, 0x21U};
    std::array<aoahid_usage_semantic, 2U> camera_semantics{AOAHID_USAGE_ONE_SHOT,
                                                           AOAHID_USAGE_ONE_SHOT};
    const std::array<const char*, 2U> camera_types{"TEST_EVENT_TYPE", "TEST_EVENT_TYPE"};
    const std::array<const char*, 2U> camera_codes{"TEST_EVENT_CODE", "TEST_EVENT_CODE"};
    invalid.field_page = aoa::hid::usage::page_camera_control;
    invalid.allowed_usages = camera_usages.data();
    invalid.allowed_usage_count = camera_usages.size();
    invalid.usage_semantics = camera_semantics.data();
    invalid.expected_linux_event_types = camera_types.data();
    invalid.expected_linux_codes = camera_codes.data();
    AOAHID_CHECK(aoahid_spec_create_toggle(&invalid, &spec) == AOAHID_OK);
    exercise_shared_control_state(spec, camera_usages[0]);
    aoahid_spec_release(spec);
    spec = nullptr;
    camera_semantics[0] = AOAHID_USAGE_SELECTOR_BITMAP;
    AOAHID_CHECK(aoahid_spec_create_toggle(&invalid, &spec) == AOAHID_ERR_UNSUPPORTED);

    const std::array<std::uint16_t, 4U> system_usages{0x81U, 0x8AU, 0x97U, 0x98U};
    const std::array<aoahid_usage_semantic, 4U> system_semantics{
        AOAHID_USAGE_ONE_SHOT, AOAHID_USAGE_RETRIGGER, AOAHID_USAGE_MOMENTARY,
        AOAHID_USAGE_ON_OFF_TOGGLE};
    const std::array<const char*, 4U> system_types{"TEST_EVENT_TYPE", "TEST_EVENT_TYPE",
                                                   "TEST_EVENT_TYPE", "TEST_EVENT_TYPE"};
    const std::array<const char*, 4U> system_codes{"TEST_EVENT_CODE", "TEST_EVENT_CODE",
                                                   "TEST_EVENT_CODE", "TEST_EVENT_CODE"};
    invalid.application_page = aoa::hid::usage::page_generic_desktop;
    invalid.application_usage = aoa::hid::usage::system_control;
    invalid.field_page = aoa::hid::usage::page_generic_desktop;
    invalid.allowed_usages = system_usages.data();
    invalid.allowed_usage_count = system_usages.size();
    invalid.usage_semantics = system_semantics.data();
    invalid.expected_linux_event_types = system_types.data();
    invalid.expected_linux_codes = system_codes.data();
    AOAHID_CHECK(aoahid_spec_create_toggle(&invalid, &spec) == AOAHID_OK);
    exercise_shared_control_state(spec, system_usages[0]);
    aoahid_spec_release(spec);
    spec = nullptr;

    const std::array<std::uint16_t, 4U> phone_usages{0x20U, 0x21U, 0x24U, 0xB0U};
    const std::array<aoahid_usage_semantic, 4U> phone_semantics{
        AOAHID_USAGE_ON_OFF_MAINTAINED, AOAHID_USAGE_MOMENTARY, AOAHID_USAGE_ONE_SHOT,
        AOAHID_USAGE_SELECTOR_BITMAP};
    const std::array<const char*, 4U> phone_types{"TEST_EVENT_TYPE", "TEST_EVENT_TYPE",
                                                  "TEST_EVENT_TYPE", "TEST_EVENT_TYPE"};
    const std::array<const char*, 4U> phone_codes{"TEST_EVENT_CODE", "TEST_EVENT_CODE",
                                                  "TEST_EVENT_CODE", "TEST_EVENT_CODE"};
    invalid.application_page = aoa::hid::usage::page_telephony;
    invalid.application_usage = aoa::hid::usage::phone;
    invalid.field_page = aoa::hid::usage::page_telephony;
    invalid.allowed_usages = phone_usages.data();
    invalid.allowed_usage_count = phone_usages.size();
    invalid.usage_semantics = phone_semantics.data();
    invalid.expected_linux_event_types = phone_types.data();
    invalid.expected_linux_codes = phone_codes.data();
    AOAHID_CHECK(aoahid_spec_create_toggle(&invalid, &spec) == AOAHID_OK);
    exercise_shared_control_state(spec, phone_usages[0]);
    aoahid_spec_release(spec);
}

void test_hat_domain() {
    std::array<aoahid_gamepad_axis_option, 2> axes{};
    axes[0] = {AOAHID_AXIS_X, 0x01U, 0x30U, {-127, 127, 8U, {}}, 0, "ABS_X", "AXIS_X"};
    axes[1] = {AOAHID_AXIS_Y, 0x01U, 0x31U, {-127, 127, 8U, {}}, 0, "ABS_Y", "AXIS_Y"};
    aoahid_gamepad_options options{};
    options.struct_size = static_cast<std::uint32_t>(sizeof(options));
    options.report_id = {0U, 0U, {0U, 0U, 0U}};
    options.axes = axes.data();
    options.axis_count = axes.size();
    options.button_count = 1U;
    options.button_usage_minimum = 1U;
    options.dpad_representation = AOAHID_DPAD_HAT;
    options.hat_logical_minimum = 0;
    options.hat_logical_maximum = 15;
    options.hat_bit_width = 4U;
    aoahid_spec* spec = nullptr;
    AOAHID_CHECK(aoahid_spec_create_gamepad(&options, &spec) == AOAHID_ERR_PARAM);
    options.hat_logical_maximum = 7;
    check_battery_variant(options, &aoahid_spec_create_gamepad);
    AOAHID_CHECK(aoahid_spec_create_gamepad(&options, &spec) == AOAHID_OK);
    if (spec != nullptr) {
        aoahid_capability_manifest manifest{};
        manifest.struct_size = static_cast<std::uint32_t>(sizeof(manifest));
        AOAHID_CHECK(aoahid_spec_manifest(spec, &manifest) == AOAHID_OK);
        AOAHID_CHECK(manifest.android_status == AOAHID_ANDROID_CONDITIONAL);
        aoahid_node node{};
        node.spec = spec;
        aoa::detail::GamepadState state_value{};
        state_value.axes.resize(axes.size());
        state_value.buttons.resize(options.button_count);
        state_value.button_transitions.resize(options.button_count);
        node.state = std::move(state_value);
        std::array<std::uint8_t, 32U> report{};
        std::size_t length = 0U;
        AOAHID_CHECK(aoa::detail::serialize_node(&node, report.data(), report.size(), &length) ==
                     AOAHID_OK);
        const auto* hat = field(spec, aoa::hid::FieldSemantic::hat);
        AOAHID_CHECK(hat != nullptr && extract(report.data(), *hat) == 15U);
    }
    aoahid_spec_release(spec);

    axes[0].usage_page = 0x02U;
    axes[0].usage = 0xC4U;
    spec = nullptr;
    AOAHID_CHECK(aoahid_spec_create_gamepad(&options, &spec) == AOAHID_ERR_PARAM);
    axes[0].usage_page = 0x01U;
    axes[0].usage = 0x30U;

    axes[1].role = AOAHID_AXIS_X;
    axes[1].usage = 0x30U;
    AOAHID_CHECK(aoahid_spec_create_gamepad(&options, &spec) == AOAHID_ERR_PARAM);
    axes[1].role = AOAHID_AXIS_Y;
    axes[1].usage = 0x31U;

    axes[1].expected_linux_code = "";
    AOAHID_CHECK(aoahid_spec_create_gamepad(&options, &spec) == AOAHID_ERR_UNSET_FIELD);
}

void test_controller_dpad_and_extended_axes() {
    std::array<aoahid_gamepad_axis_option, 6U> axes{
        aoahid_gamepad_axis_option{
            AOAHID_AXIS_X, 0x01U, 0x30U, {-127, 127, 8U, {}}, 0, "ABS_X", "AXIS_X"},
        aoahid_gamepad_axis_option{
            AOAHID_AXIS_Y, 0x01U, 0x31U, {-127, 127, 8U, {}}, 0, "ABS_Y", "AXIS_Y"},
        aoahid_gamepad_axis_option{
            AOAHID_AXIS_DIAL, 0x01U, 0x37U, {-127, 127, 8U, {}}, 0, "TEST", "TEST"},
        aoahid_gamepad_axis_option{
            AOAHID_AXIS_WHEEL, 0x01U, 0x38U, {-127, 127, 8U, {}}, 0, "TEST", "TEST"},
        aoahid_gamepad_axis_option{
            AOAHID_AXIS_SIMULATION_RUDDER, 0x02U, 0xBAU, {-127, 127, 8U, {}}, 0, "TEST", "TEST"},
        aoahid_gamepad_axis_option{
            AOAHID_AXIS_SIMULATION_THROTTLE, 0x02U, 0xBBU, {0, 255, 8U, {}}, 0, "TEST", "TEST"}};
    aoahid_gamepad_options options{};
    options.struct_size = static_cast<std::uint32_t>(sizeof(options));
    options.axes = axes.data();
    options.axis_count = axes.size();
    options.button_count = 1U;
    options.button_usage_minimum = 1U;
    options.dpad_representation = AOAHID_DPAD_BUTTONS;

    aoahid_spec* spec = nullptr;
    AOAHID_CHECK(aoahid_spec_create_gamepad(&options, &spec) == AOAHID_OK);
    if (spec == nullptr)
        return;
    aoahid_capability_manifest raw_dpad_manifest{};
    raw_dpad_manifest.struct_size = static_cast<std::uint32_t>(sizeof(raw_dpad_manifest));
    AOAHID_CHECK(aoahid_spec_manifest(spec, &raw_dpad_manifest) == AOAHID_OK);
    AOAHID_CHECK(raw_dpad_manifest.android_status == AOAHID_ANDROID_CONDITIONAL);
    AOAHID_CHECK(count_item(spec->descriptor, 0x09U, 0x90U) == 1U);
    AOAHID_CHECK(count_item(spec->descriptor, 0x09U, 0x91U) == 1U);
    AOAHID_CHECK(count_item(spec->descriptor, 0x09U, 0x92U) == 1U);
    AOAHID_CHECK(count_item(spec->descriptor, 0x09U, 0x93U) == 1U);
    AOAHID_CHECK(count_item(spec->descriptor, 0x81U, 0x22U) == 4U);

    aoahid_node node{};
    node.spec = spec;
    aoa::detail::GamepadState gamepad{};
    gamepad.axes.resize(axes.size());
    gamepad.buttons.resize(options.button_count);
    gamepad.button_transitions.resize(options.button_count);
    node.state = std::move(gamepad);
    node.dirty = false;
    AOAHID_CHECK(aoahid_gamepad_dpad(&node, 1U, 0U, 1U, 0U) == AOAHID_OK);
    AOAHID_CHECK(node.dirty);
    std::array<std::uint8_t, 64U> report{};
    std::size_t length = 0U;
    AOAHID_CHECK(aoa::detail::serialize_node(&node, report.data(), report.size(), &length) ==
                 AOAHID_OK);
    const auto* up = field(spec, aoa::hid::FieldSemantic::buttons, 1U);
    const auto* down = field(spec, aoa::hid::FieldSemantic::buttons, 2U);
    const auto* right = field(spec, aoa::hid::FieldSemantic::buttons, 3U);
    const auto* left = field(spec, aoa::hid::FieldSemantic::buttons, 4U);
    AOAHID_CHECK(up != nullptr && extract(report.data(), *up) == 1U);
    AOAHID_CHECK(down != nullptr && extract(report.data(), *down) == 0U);
    AOAHID_CHECK(right != nullptr && extract(report.data(), *right) == 1U);
    AOAHID_CHECK(left != nullptr && extract(report.data(), *left) == 0U);
    AOAHID_CHECK(aoahid_gamepad_dpad(&node, 0U, 1U, 0U, 1U) == AOAHID_ERR_BUSY);
    node.dirty = false;
    AOAHID_CHECK(aoahid_gamepad_dpad(&node, 1U, 0U, 1U, 0U) == AOAHID_OK);
    AOAHID_CHECK(!node.dirty);
    aoa::detail::transfer_complete(&node, AOAHID_OK, 0);
    AOAHID_CHECK(aoahid_gamepad_dpad(&node, 0U, 1U, 0U, 1U) == AOAHID_OK);
    AOAHID_CHECK(node.dirty);
    node.dirty = false;
    AOAHID_CHECK(aoahid_gamepad_axis(&node, 0U, 0) == AOAHID_OK);
    AOAHID_CHECK(!node.dirty);
    aoahid_spec_release(spec);
    spec = nullptr;

    options.button_count = 4U;
    options.dpad_representation = AOAHID_DPAD_HAT;
    options.hat_logical_minimum = 0;
    options.hat_logical_maximum = 7;
    options.hat_bit_width = 4U;
    AOAHID_CHECK(aoahid_spec_create_gamepad(&options, &spec) == AOAHID_OK);
    if (spec != nullptr) {
        aoahid_capability_manifest four_button_manifest{};
        four_button_manifest.struct_size = static_cast<std::uint32_t>(sizeof(four_button_manifest));
        AOAHID_CHECK(aoahid_spec_manifest(spec, &four_button_manifest) == AOAHID_OK);
        AOAHID_CHECK(four_button_manifest.android_status == AOAHID_ANDROID_CONDITIONAL);
        aoahid_spec_release(spec);
        spec = nullptr;
    }
    options.button_count = 5U;
    AOAHID_CHECK(aoahid_spec_create_gamepad(&options, &spec) == AOAHID_OK);
    if (spec != nullptr) {
        aoahid_capability_manifest manifest{};
        manifest.struct_size = static_cast<std::uint32_t>(sizeof(manifest));
        AOAHID_CHECK(aoahid_spec_manifest(spec, &manifest) == AOAHID_OK);
        AOAHID_CHECK(manifest.android_status == AOAHID_ANDROID_PORTABLE_CANDIDATE);
        const auto* hat = field(spec, aoa::hid::FieldSemantic::hat);
        AOAHID_CHECK(hat != nullptr && hat->physical.enabled == 1U && hat->physical.minimum == 0 &&
                     hat->physical.maximum == 315 && hat->physical.unit_exponent == 0 &&
                     hat->physical.unit == 0x14U);
        aoahid_node hat_node{};
        hat_node.spec = spec;
        aoa::detail::GamepadState hat_state{};
        hat_state.axes.resize(axes.size());
        hat_state.buttons.resize(options.button_count);
        hat_state.button_transitions.resize(options.button_count);
        hat_node.state = std::move(hat_state);
        struct DirectionCase {
            std::uint32_t up;
            std::uint32_t down;
            std::uint32_t right;
            std::uint32_t left;
            std::uint32_t wire;
        };
        const std::array<DirectionCase, 9U> directions{
            DirectionCase{0U, 0U, 0U, 0U, 15U}, DirectionCase{1U, 0U, 0U, 0U, 0U},
            DirectionCase{1U, 0U, 1U, 0U, 1U},  DirectionCase{0U, 0U, 1U, 0U, 2U},
            DirectionCase{0U, 1U, 1U, 0U, 3U},  DirectionCase{0U, 1U, 0U, 0U, 4U},
            DirectionCase{0U, 1U, 0U, 1U, 5U},  DirectionCase{0U, 0U, 0U, 1U, 6U},
            DirectionCase{1U, 0U, 0U, 1U, 7U}};
        for (const auto& direction : directions) {
            AOAHID_CHECK(aoahid_gamepad_dpad(&hat_node, direction.up, direction.down,
                                             direction.right, direction.left) == AOAHID_OK);
            report.fill(0U);
            AOAHID_CHECK(aoa::detail::serialize_node(&hat_node, report.data(), report.size(),
                                                     &length) == AOAHID_OK);
            AOAHID_CHECK(hat != nullptr && extract(report.data(), *hat) == direction.wire);
            aoa::detail::transfer_complete(&hat_node, AOAHID_OK, 0);
            hat_node.dirty = false;
        }
        AOAHID_CHECK(aoahid_gamepad_dpad(&hat_node, 1U, 1U, 0U, 0U) == AOAHID_ERR_PARAM);
        AOAHID_CHECK(aoahid_gamepad_dpad(&hat_node, 0U, 0U, 1U, 1U) == AOAHID_ERR_PARAM);
        report.fill(0U);
        AOAHID_CHECK(aoa::detail::serialize_node(&hat_node, report.data(), report.size(),
                                                 &length) == AOAHID_OK);
        AOAHID_CHECK(hat != nullptr && extract(report.data(), *hat) == 7U);
        AOAHID_CHECK(!hat_node.dirty);
        aoahid_spec_release(spec);
        spec = nullptr;
    }

    axes[4].neutral_value = 1;
    AOAHID_CHECK(aoahid_spec_create_gamepad(&options, &spec) == AOAHID_ERR_PARAM);
    axes[4].neutral_value = 0;
    axes[4].value = {0, 127, 8U, {}};
    AOAHID_CHECK(aoahid_spec_create_gamepad(&options, &spec) == AOAHID_ERR_PARAM);
    axes[4].value = {-127, 127, 8U, {}};
    axes[5].neutral_value = 1;
    AOAHID_CHECK(aoahid_spec_create_gamepad(&options, &spec) == AOAHID_ERR_PARAM);
    axes[5].neutral_value = 0;
    axes[5].value = {-1, 255, 16U, {}};
    AOAHID_CHECK(aoahid_spec_create_gamepad(&options, &spec) == AOAHID_ERR_PARAM);
    axes[5].value = {0, 255, 8U, {}};

    options.dpad_representation = AOAHID_DPAD_NONE;
    options.hat_logical_minimum = 0;
    options.hat_logical_maximum = 0;
    options.hat_bit_width = 4U;
    AOAHID_CHECK(aoahid_spec_create_gamepad(&options, &spec) == AOAHID_ERR_PARAM);
}

void test_physical_properties_and_reset() {
    aoahid_mouse_options options{};
    options.struct_size = static_cast<std::uint32_t>(sizeof(options));
    options.button_count = 1U;
    options.x = {-127, 127, 8U, {1U, -10, 10, -2, 0x11U}};
    options.y = {-127, 127, 8U, {}};
    aoahid_spec* spec = nullptr;
    AOAHID_CHECK(aoahid_spec_create_mouse(&options, &spec) == AOAHID_OK);
    if (spec != nullptr) {
        const auto* x = field(spec, aoa::hid::FieldSemantic::x);
        const auto* y = field(spec, aoa::hid::FieldSemantic::y);
        AOAHID_CHECK(x != nullptr && x->physical.enabled == 1U && x->physical.minimum == -10 &&
                     x->physical.maximum == 10 && x->physical.unit_exponent == -2 &&
                     x->physical.unit == 0x11U);
        AOAHID_CHECK(y != nullptr && y->physical.enabled == 0U && y->physical.minimum == 0 &&
                     y->physical.maximum == 0 && y->physical.unit_exponent == 0 &&
                     y->physical.unit == 0U);
        AOAHID_CHECK(has_sequence(spec->descriptor,
                                  {0x35U, 0xF6U, 0x45U, 0x0AU, 0x55U, 0x0EU, 0x65U, 0x11U}));
        AOAHID_CHECK(has_sequence(spec->descriptor, {0x81U, 0x06U, 0x35U, 0x00U, 0x45U, 0x00U,
                                                     0x55U, 0x00U, 0x65U, 0x00U}));
    }
    aoahid_spec_release(spec);

    options.x.physical.maximum = -10;
    AOAHID_CHECK(aoahid_spec_create_mouse(&options, &spec) == AOAHID_ERR_PARAM);
    options.x.physical = {1U, -10, 10, 8, 0x11U};
    AOAHID_CHECK(aoahid_spec_create_mouse(&options, &spec) == AOAHID_ERR_PARAM);
    options.x.physical = {1U, -10, 10, -2, 0U};
    AOAHID_CHECK(aoahid_spec_create_mouse(&options, &spec) == AOAHID_ERR_PARAM);
    options.x.physical = {1U, -10, 10, -2, 0x05U};
    AOAHID_CHECK(aoahid_spec_create_mouse(&options, &spec) == AOAHID_ERR_PARAM);
    options.x.physical = {0U, 0, 1, 0, 0U};
    AOAHID_CHECK(aoahid_spec_create_mouse(&options, &spec) == AOAHID_ERR_PARAM);
}

aoahid_touchscreen_options touchscreen_options() {
    aoahid_touchscreen_options options{};
    options.struct_size = static_cast<std::uint32_t>(sizeof(options));
    options.report_id = {0U, 0U, {0U, 0U, 0U}};
    options.maximum_contacts = 3U;
    options.contacts_per_report = 2U;
    options.contact_id = {0, 15, 4U, {}};
    options.x = {0, 1000, 16U, {}};
    options.y = {0, 1000, 16U, {}};
    options.contact_count = {0, 3, 2U, {}};
    options.enable_pressure = 1U;
    options.pressure = {0, 255, 8U, {}};
    options.enable_width = 0U;
    options.width = {0, 0, 0U, {}};
    options.enable_height = 0U;
    options.height = {0, 0, 0U, {}};
    options.enable_scan_time = 1U;
    options.scan_time = {0, 65535, 16U, {}};
    options.scan_time_unit_100us = 1U;
    options.enable_contact_count_maximum_feature_declaration = 0U;
    options.enable_multi_packet_frames = 1U;
    return options;
}

aoahid_touchpad_options touchpad_options() {
    aoahid_touchpad_options options{};
    options.struct_size = static_cast<std::uint32_t>(sizeof(options));
    options.report_id = {0U, 0U, {0U, 0U, 0U}};
    options.maximum_contacts = 3U;
    options.contacts_per_report = 2U;
    options.contact_id = {0, 15, 4U, {}};
    options.x = {0, 1000, 16U, {}};
    options.y = {0, 1000, 16U, {}};
    options.contact_count = {0, 3, 2U, {}};
    options.enable_pressure = 1U;
    options.pressure = {0, 255, 8U, {}};
    options.enable_width = 0U;
    options.width = {0, 0, 0U, {}};
    options.enable_height = 0U;
    options.height = {0, 0, 0U, {}};
    options.enable_scan_time = 1U;
    options.scan_time = {0, 65535, 16U, {}};
    options.scan_time_unit_100us = 1U;
    options.enable_contact_count_maximum_feature_declaration = 0U;
    options.enable_multi_packet_frames = 1U;
    options.button_count = 0U;
    return options;
}

void test_field_span_padding() {
    // X ends mid-byte, so a 32-bit Y right behind it would touch five bytes.
    aoahid_mouse_options mouse{};
    mouse.struct_size = static_cast<std::uint32_t>(sizeof(mouse));
    mouse.button_count = 1U;
    mouse.x = {-1073741823, 1073741823, 31U, {}};
    mouse.y = {-2147483647, 2147483647, 32U, {}};
    aoahid_spec* spec = nullptr;
    AOAHID_CHECK(aoahid_spec_create_mouse(&mouse, &spec) == AOAHID_OK);
    if (spec != nullptr) {
        const auto* x = field(spec, aoa::hid::FieldSemantic::x);
        const auto* y = field(spec, aoa::hid::FieldSemantic::y);
        AOAHID_CHECK(x != nullptr && x->bit_offset == 8U);
        AOAHID_CHECK(y != nullptr && y->bit_offset == 40U);
    }
    aoahid_spec_release(spec);
    spec = nullptr;

    // Fields that already fit keep their packed positions.
    mouse.x = {-127, 127, 8U, {}};
    mouse.y = {-2047, 2047, 12U, {}};
    AOAHID_CHECK(aoahid_spec_create_mouse(&mouse, &spec) == AOAHID_OK);
    if (spec != nullptr) {
        const auto* y = field(spec, aoa::hid::FieldSemantic::y);
        AOAHID_CHECK(y != nullptr && y->bit_offset == 16U);
    }
    aoahid_spec_release(spec);
    spec = nullptr;

    // A 4-bit Contact Identifier followed by a 31-bit X.
    aoahid_touchscreen_options touch{};
    touch.struct_size = static_cast<std::uint32_t>(sizeof(touch));
    touch.maximum_contacts = 1U;
    touch.contacts_per_report = 1U;
    touch.contact_id = {0, 15, 4U, {}};
    touch.x = {0, 2147483646, 31U, {}};
    touch.y = {0, 2559, 12U, {}};
    touch.contact_count = {0, 1, 1U, {}};
    AOAHID_CHECK(aoahid_spec_create_touchscreen(&touch, &spec) == AOAHID_OK);
    if (spec != nullptr) {
        const auto* x = field(spec, aoa::hid::FieldSemantic::x);
        AOAHID_CHECK(x != nullptr && x->bit_offset % 8U == 0U);
    }
    aoahid_spec_release(spec);
}

void test_touch_units_feature_and_manifest_status() {
    aoahid_touchscreen_options options = touchscreen_options();
    options.enable_contact_count_maximum_feature_declaration = 1U;
    aoahid_spec* spec = nullptr;
    check_battery_variant(options, &aoahid_spec_create_touchscreen);
    AOAHID_CHECK(aoahid_spec_create_touchscreen(&options, &spec) == AOAHID_OK);
    if (spec != nullptr) {
        AOAHID_CHECK(count_item(spec->descriptor, 0xB1U, 0x03U) == 1U);
        aoahid_spec_release(spec);
    }

    options = touchscreen_options();
    options.x.physical = {1U, 0, 100, -2, 0x11U};
    options.enable_width = 1U;
    options.width = {0, 100, 8U, {1U, 0, 10, -2, 0x11U}};
    spec = nullptr;
    AOAHID_CHECK(aoahid_spec_create_touchscreen(&options, &spec) == AOAHID_OK);
    aoahid_spec_release(spec);
    options.width.physical.unit_exponent = -3;
    AOAHID_CHECK(aoahid_spec_create_touchscreen(&options, &spec) == AOAHID_ERR_PARAM);

    options = touchscreen_options();
    options.y.physical = {1U, 0, 100, -2, 0x11U};
    options.enable_height = 1U;
    options.height = {0, 100, 8U, {1U, 0, 10, -2, 0x12U}};
    AOAHID_CHECK(aoahid_spec_create_touchscreen(&options, &spec) == AOAHID_ERR_PARAM);

    options = touchscreen_options();
    options.scan_time.physical = {1U, 0, 65535, -4, 0x1001U};
    AOAHID_CHECK(aoahid_spec_create_touchscreen(&options, &spec) == AOAHID_ERR_PARAM);
}

void test_touch_azimuth() {
    aoahid_touchscreen_options options = touchscreen_options();
    options.enable_azimuth = 1U;
    options.azimuth = {0, 36000, 16U, {1U, 0, 360, 0, 0x14U}};
    aoahid_spec* spec = nullptr;
    AOAHID_CHECK(aoahid_spec_create_touchscreen(&options, &spec) == AOAHID_OK);
    if (spec == nullptr)
        return;

    const auto* azimuth = field(spec, aoa::hid::FieldSemantic::azimuth, 0U);
    AOAHID_CHECK(azimuth != nullptr && azimuth->logical_minimum == 0 &&
                 azimuth->logical_maximum == 36000);
    aoahid_node node{};
    node.spec = spec;
    node.state = aoa::detail::TouchState{};
    const aoa::detail::TouchContact contact{1U, 100, 200, 10, 0, 0, 35999};
    aoahid_touchscreen_extra extra{contact.pressure, contact.width, contact.height,
                                   contact.azimuth};
    AOAHID_CHECK(aoahid_touchscreen_contact(&node, contact.contact_id, 1U, contact.x, contact.y,
                                            &extra) == AOAHID_OK);
    std::array<std::uint8_t, 128U> report{};
    std::size_t length = 0U;
    AOAHID_CHECK(aoa::detail::serialize_node(&node, report.data(), report.size(), &length) ==
                 AOAHID_OK);
    AOAHID_CHECK(azimuth != nullptr && extract(report.data(), *azimuth) == 35999U);
    node.dirty = false;
    AOAHID_CHECK(aoahid_touchscreen_contact(&node, contact.contact_id, 1U, contact.x, contact.y,
                                            &extra) == AOAHID_OK);
    AOAHID_CHECK(!node.dirty);
    aoahid_touchscreen_extra invalid_extra = extra;
    invalid_extra.azimuth = 36000;
    AOAHID_CHECK(aoahid_touchscreen_contact(&node, contact.contact_id, 1U, contact.x, contact.y,
                                            &invalid_extra) == AOAHID_ERR_PARAM);
    aoahid_spec_release(spec);

    options.azimuth.logical_minimum = 1;
    AOAHID_CHECK(aoahid_spec_create_touchscreen(&options, &spec) == AOAHID_ERR_PARAM);
    options.enable_azimuth = 0U;
    AOAHID_CHECK(aoahid_spec_create_touchscreen(&options, &spec) == AOAHID_ERR_PARAM);
    options.azimuth = {};
    AOAHID_CHECK(aoahid_spec_create_touchscreen(&options, &spec) == AOAHID_OK);
    if (spec != nullptr) {
        aoahid_node disabled_node{};
        disabled_node.spec = spec;
        disabled_node.state = aoa::detail::TouchState{};
        aoahid_touchscreen_extra disabled_extra{10, 0, 0, 1};
        AOAHID_CHECK(aoahid_touchscreen_contact(&disabled_node, 1U, 1U, 100, 200,
                                                &disabled_extra) == AOAHID_ERR_PARAM);
    }
    aoahid_spec_release(spec);
}

void test_touch_packets() {
    aoahid_touchscreen_options options = touchscreen_options();
    aoahid_spec* spec = nullptr;
    AOAHID_CHECK(aoahid_spec_create_touchscreen(&options, &spec) == AOAHID_OK);
    if (spec == nullptr)
        return;
    aoahid_node node{};
    node.spec = spec;
    node.state = aoa::detail::TouchState{};
    for (std::uint32_t id = 0U; id < 3U; ++id) {
        const aoa::detail::TouchContact contact{id,
                                                static_cast<std::int32_t>(100U + id),
                                                static_cast<std::int32_t>(200U + id),
                                                id == 0U ? 0 : 10,
                                                0,
                                                0,
                                                0};
        aoahid_touchscreen_extra extra{contact.pressure, contact.width, contact.height,
                                       contact.azimuth};
        AOAHID_CHECK(aoahid_touchscreen_contact(&node, contact.contact_id, 1U, contact.x, contact.y,
                                                &extra) == AOAHID_OK);
    }
    std::array<std::uint8_t, 128> report{};
    std::size_t length = 0U;
    AOAHID_CHECK(aoa::detail::serialize_node(&node, report.data(), report.size(), &length) ==
                 AOAHID_OK);
    const auto* count = field(spec, aoa::hid::FieldSemantic::contact_count);
    const auto* pressure = field(spec, aoa::hid::FieldSemantic::pressure, 0U);
    const auto* scan_time = field(spec, aoa::hid::FieldSemantic::scan_time);
    AOAHID_CHECK(count != nullptr && extract(report.data(), *count) == 3U);
    AOAHID_CHECK(pressure != nullptr && extract(report.data(), *pressure) == 1U);
    AOAHID_CHECK(scan_time != nullptr && extract(report.data(), *scan_time) == 0U);
    node.transfer_inflight.store(true);
    aoa::detail::transfer_complete(&node, AOAHID_OK, 0);
    const aoa::detail::TouchContact changed{0U, 300, 400, 10, 0, 0, 0};
    aoahid_touchscreen_extra changed_extra{changed.pressure, changed.width, changed.height,
                                           changed.azimuth};
    AOAHID_CHECK(aoahid_touchscreen_contact(&node, changed.contact_id, 1U, changed.x, changed.y,
                                            &changed_extra) == AOAHID_ERR_BUSY);
    AOAHID_CHECK(aoa::detail::serialize_node(&node, report.data(), report.size(), &length) ==
                 AOAHID_OK);
    AOAHID_CHECK(count != nullptr && extract(report.data(), *count) == 0U);
    AOAHID_CHECK(scan_time != nullptr && extract(report.data(), *scan_time) == 0U);
    aoahid_spec_release(spec);
}

void test_touch_lift_frame_and_idle_suppression() {
    aoahid_touchscreen_options options = touchscreen_options();
    aoahid_spec* spec = nullptr;
    AOAHID_CHECK(aoahid_spec_create_touchscreen(&options, &spec) == AOAHID_OK);
    if (spec == nullptr)
        return;

    aoahid_node node{};
    node.spec = spec;
    node.state = aoa::detail::TouchState{};
    const aoa::detail::TouchContact contact{7U, 100, 200, 0, 0, 0, 0};
    AOAHID_CHECK(aoahid_touchscreen_contact(&node, contact.contact_id, 1U, contact.x, contact.y,
                                            nullptr) == AOAHID_OK);
    AOAHID_CHECK(aoahid_touchscreen_contact(&node, 7U, 0U, 300, 400, nullptr) == AOAHID_ERR_BUSY);

    const auto* count = field(spec, aoa::hid::FieldSemantic::contact_count);
    const auto* tip = field(spec, aoa::hid::FieldSemantic::tip);
    const auto* identifier = field(spec, aoa::hid::FieldSemantic::contact_id);
    const auto* x = field(spec, aoa::hid::FieldSemantic::x);
    const auto* y = field(spec, aoa::hid::FieldSemantic::y);
    const auto* pressure = field(spec, aoa::hid::FieldSemantic::pressure);
    const auto* scan_time = field(spec, aoa::hid::FieldSemantic::scan_time);
    std::array<std::uint8_t, 128U> report{};
    std::size_t length = 0U;
    AOAHID_CHECK(aoa::detail::serialize_node(&node, report.data(), report.size(), &length) ==
                 AOAHID_OK);
    AOAHID_CHECK(count != nullptr && extract(report.data(), *count) == 1U);
    AOAHID_CHECK(tip != nullptr && extract(report.data(), *tip) == 1U);
    AOAHID_CHECK(identifier != nullptr && extract(report.data(), *identifier) == 7U);
    AOAHID_CHECK(x != nullptr && extract(report.data(), *x) == 100U);
    AOAHID_CHECK(y != nullptr && extract(report.data(), *y) == 200U);
    AOAHID_CHECK(pressure != nullptr && extract(report.data(), *pressure) == 1U);
    AOAHID_CHECK(scan_time != nullptr && extract(report.data(), *scan_time) == 0U);
    aoa::detail::transfer_complete(&node, AOAHID_OK, 0);
    AOAHID_CHECK(!node.dirty);

    AOAHID_CHECK(aoahid_touchscreen_contact(&node, 7U, 0U, 300, 400, nullptr) == AOAHID_OK);
    report.fill(0U);
    AOAHID_CHECK(aoa::detail::serialize_node(&node, report.data(), report.size(), &length) ==
                 AOAHID_OK);
    // The explicit Up report keeps the same slot and final coordinates while
    // clearing Tip; it is still counted until its successful completion.
    AOAHID_CHECK(count != nullptr && extract(report.data(), *count) == 1U);
    AOAHID_CHECK(tip != nullptr && extract(report.data(), *tip) == 0U);
    AOAHID_CHECK(identifier != nullptr && extract(report.data(), *identifier) == 7U);
    AOAHID_CHECK(x != nullptr && extract(report.data(), *x) == 300U);
    AOAHID_CHECK(y != nullptr && extract(report.data(), *y) == 400U);
    AOAHID_CHECK(pressure != nullptr && extract(report.data(), *pressure) == 0U);
    aoa::detail::transfer_complete(&node, AOAHID_OK, 0);

    const auto* state_value = std::get_if<aoa::detail::TouchState>(&node.state);
    AOAHID_CHECK(state_value != nullptr);
    if (state_value != nullptr) {
        AOAHID_CHECK(std::all_of(state_value->contacts.begin(), state_value->contacts.end(),
                                 [](const aoa::detail::ContactState& entry) {
                                     return entry.phase == aoa::detail::ContactPhase::none;
                                 }));
    }
    // Idle touch state is clean, so the public submit path suppresses a
    // zero-count report instead of sending one and reusing the previous count.
    AOAHID_CHECK(!node.dirty);
    AOAHID_CHECK(aoahid_touchscreen_contact(&node, 7U, 0U, 300, 400, nullptr) == AOAHID_ERR_PARAM);
    const aoa::detail::TouchContact next_contact{8U, 500, 600, 10, 0, 0, 0};
    aoahid_touchscreen_extra next_extra{next_contact.pressure, next_contact.width,
                                        next_contact.height, next_contact.azimuth};
    AOAHID_CHECK(aoahid_touchscreen_contact(&node, next_contact.contact_id, 1U, next_contact.x,
                                            next_contact.y, &next_extra) == AOAHID_OK);
    report.fill(0U);
    AOAHID_CHECK(aoa::detail::serialize_node(&node, report.data(), report.size(), &length) ==
                 AOAHID_OK);
    AOAHID_CHECK(scan_time != nullptr && extract(report.data(), *scan_time) == 0U);
    aoahid_spec_release(spec);
}

void test_touchpad_button_only_and_mid_frame_guard() {
    aoahid_touchpad_options options = touchpad_options();
    options.button_count = 1U;
    aoahid_spec* spec = nullptr;
    check_battery_variant(options, &aoahid_spec_create_touchpad);
    AOAHID_CHECK(aoahid_spec_create_touchpad(&options, &spec) == AOAHID_OK);
    if (spec == nullptr)
        return;

    aoahid_node node{};
    node.spec = spec;
    aoa::detail::TouchState state_value{};
    state_value.buttons.resize(1U);
    state_value.button_transitions.resize(1U);
    node.state = std::move(state_value);

    AOAHID_CHECK(aoahid_touchpad_button(&node, 1U, 1U) == AOAHID_OK);
    std::array<std::uint8_t, 128U> report{};
    std::size_t length = 0U;
    AOAHID_CHECK(aoa::detail::serialize_node(&node, report.data(), report.size(), &length) ==
                 AOAHID_OK);
    const auto* count = field(spec, aoa::hid::FieldSemantic::contact_count);
    const auto* button = field(spec, aoa::hid::FieldSemantic::buttons);
    AOAHID_CHECK(count != nullptr && extract(report.data(), *count) == 0U);
    AOAHID_CHECK(button != nullptr && extract(report.data(), *button) == 1U);
    AOAHID_CHECK(aoahid_touchpad_button(&node, 1U, 0U) == AOAHID_ERR_BUSY);
    aoa::detail::transfer_complete(&node, AOAHID_OK, 0);

    auto* touch = std::get_if<aoa::detail::TouchState>(&node.state);
    AOAHID_CHECK(touch != nullptr);
    if (touch != nullptr) {
        touch->packet_cursor = 1U;
        AOAHID_CHECK(aoahid_touchpad_button(&node, 1U, 0U) == AOAHID_ERR_BUSY);
        AOAHID_CHECK(touch->buttons[0] == 1U);
        touch->packet_cursor = 0U;
    }

    AOAHID_CHECK(aoahid_touchpad_button(&node, 1U, 0U) == AOAHID_OK);
    report.fill(0U);
    AOAHID_CHECK(aoa::detail::serialize_node(&node, report.data(), report.size(), &length) ==
                 AOAHID_OK);
    AOAHID_CHECK(count != nullptr && extract(report.data(), *count) == 0U);
    AOAHID_CHECK(button != nullptr && extract(report.data(), *button) == 0U);
    aoahid_spec_release(spec);
}

void test_pen_away_coordinates() {
    const std::array<std::uint16_t, 1> barrels{0x44U};
    aoahid_pen_options options{};
    options.struct_size = static_cast<std::uint32_t>(sizeof(options));
    options.report_id = {0U, 0U, {0U, 0U, 0U}};
    options.x = {0, 1000, 16U, {}};
    options.y = {0, 1000, 16U, {}};
    options.enable_pressure = 1U;
    options.pressure = {0, 255, 8U, {}};
    options.barrel_usages = barrels.data();
    options.barrel_usage_count = barrels.size();
    options.enable_hover = 1U;
    aoahid_spec* spec = nullptr;
    check_battery_variant(options, &aoahid_spec_create_pen);
    AOAHID_CHECK(aoahid_spec_create_pen(&options, &spec) == AOAHID_OK);
    if (spec == nullptr)
        return;
    const auto* x = field(spec, aoa::hid::FieldSemantic::x);
    const auto* y = field(spec, aoa::hid::FieldSemantic::y);
    AOAHID_CHECK(x != nullptr && y != nullptr);
    std::array<std::uint8_t, 128> report{};
    std::size_t length = 0U;
    aoahid_node node{};
    node.spec = spec;
    node.state = aoa::detail::PenState{};
    aoahid_pen_sample sample{};
    sample.in_range = 1U;
    sample.x = 100;
    sample.y = 200;
    AOAHID_CHECK(aoahid_pen_update(&node, &sample) == AOAHID_OK);
    node.transfer_inflight.store(true);
    aoa::detail::transfer_complete(&node, AOAHID_OK, 0);
    // The away report carries the sample's own X/Y, not the last position.
    sample.in_range = 0U;
    sample.x = 500;
    sample.y = 600;
    AOAHID_CHECK(aoahid_pen_update(&node, &sample) == AOAHID_OK);
    report.fill(0U);
    AOAHID_CHECK(aoa::detail::serialize_node(&node, report.data(), report.size(), &length) ==
                 AOAHID_OK);
    const auto* range = field(spec, aoa::hid::FieldSemantic::in_range);
    AOAHID_CHECK(range != nullptr && extract(report.data(), *range) == 0U);
    AOAHID_CHECK(x != nullptr && extract(report.data(), *x) == 500U);
    AOAHID_CHECK(y != nullptr && extract(report.data(), *y) == 600U);
    aoahid_spec_release(spec);
}

void test_pen_switch() {
    const std::array<std::uint16_t, 1> barrels{0x44U};
    aoahid_pen_options options{};
    options.struct_size = static_cast<std::uint32_t>(sizeof(options));
    options.report_id = {0U, 0U, {0U, 0U, 0U}};
    options.x = {0, 1000, 16U, {}};
    options.y = {0, 1000, 16U, {}};
    options.enable_pressure = 1U;
    options.pressure = {0, 255, 8U, {}};
    options.enable_tilt = 0U;
    options.enable_twist_target_specific = 0U;
    options.barrel_usages = barrels.data();
    options.barrel_usage_count = barrels.size();
    options.enable_eraser = 1U;
    options.enable_hover = 1U;
    aoahid_spec* spec = nullptr;
    AOAHID_CHECK(aoahid_spec_create_pen(&options, &spec) == AOAHID_OK);
    if (spec == nullptr)
        return;
    aoahid_capability_manifest manifest{};
    manifest.struct_size = static_cast<std::uint32_t>(sizeof(manifest));
    AOAHID_CHECK(aoahid_spec_manifest(spec, &manifest) == AOAHID_OK);
    AOAHID_CHECK(manifest.android_status == AOAHID_ANDROID_PORTABLE_CANDIDATE);
    aoahid_node node{};
    node.spec = spec;
    aoa::detail::PenState state_value{};
    state_value.emitted.in_range = 1U;
    state_value.emitted.barrel_buttons = 1U;
    state_value.emitted.x = 100;
    state_value.emitted.y = 200;
    node.state = state_value;
    aoahid_pen_sample sample{};
    sample.in_range = 1U;
    sample.eraser = 1U;
    sample.barrel_buttons = 1U;
    sample.x = 100;
    sample.y = 200;
    AOAHID_CHECK(aoahid_pen_update(&node, &sample) == AOAHID_OK);
    sample.in_range = 0U;
    AOAHID_CHECK(aoahid_pen_update(&node, &sample) == AOAHID_ERR_BUSY);
    std::array<std::uint8_t, 128> report{};
    std::size_t length = 0U;
    AOAHID_CHECK(aoa::detail::serialize_node(&node, report.data(), report.size(), &length) ==
                 AOAHID_OK);
    const auto* range = field(spec, aoa::hid::FieldSemantic::in_range);
    const auto* invert = field(spec, aoa::hid::FieldSemantic::eraser);
    const auto* barrel = field(spec, aoa::hid::FieldSemantic::buttons);
    const auto* tip = field(spec, aoa::hid::FieldSemantic::tip);
    AOAHID_CHECK(invert != nullptr && tip != nullptr && range != nullptr &&
                 invert->bit_offset < tip->bit_offset && tip->bit_offset < range->bit_offset);
    AOAHID_CHECK(range != nullptr && extract(report.data(), *range) == 0U);
    AOAHID_CHECK(invert != nullptr && extract(report.data(), *invert) == 0U);
    AOAHID_CHECK(barrel != nullptr && extract(report.data(), *barrel) == 0U);
    // Model the order-sensitive tool selection in Android common commit
    // 35556bed836f8dc07ac55f69c8d17dce3e7f0e25. Departure releases both tools;
    // Invert must then be processed before In Range on eraser re-entry.
    PinnedLinuxPenTools tools{false, true, false};
    apply_pinned_linux_pen_report(spec, report.data(), &tools);
    AOAHID_CHECK(!tools.pen && !tools.rubber);
    node.transfer_inflight.store(true);
    aoa::detail::transfer_complete(&node, AOAHID_OK, 0);
    AOAHID_CHECK(aoa::detail::serialize_node(&node, report.data(), report.size(), &length) ==
                 AOAHID_OK);
    AOAHID_CHECK(range != nullptr && extract(report.data(), *range) == 1U);
    AOAHID_CHECK(invert != nullptr && extract(report.data(), *invert) == 1U);
    apply_pinned_linux_pen_report(spec, report.data(), &tools);
    AOAHID_CHECK(!tools.pen && tools.rubber);

    // Commit the eraser-side arrival, then leave range without making the
    // caller clear the remembered tool selector. The wire report must still
    // clear Invert so current Linux tool state can release BTN_TOOL_RUBBER.
    node.transfer_inflight.store(true);
    aoa::detail::transfer_complete(&node, AOAHID_OK, 0);
    sample.in_range = 0U;
    AOAHID_CHECK(aoahid_pen_update(&node, &sample) == AOAHID_OK);
    report.fill(0U);
    AOAHID_CHECK(aoa::detail::serialize_node(&node, report.data(), report.size(), &length) ==
                 AOAHID_OK);
    const auto* pressure = field(spec, aoa::hid::FieldSemantic::pressure);
    AOAHID_CHECK(range != nullptr && extract(report.data(), *range) == 0U);
    AOAHID_CHECK(tip != nullptr && extract(report.data(), *tip) == 0U);
    AOAHID_CHECK(invert != nullptr && extract(report.data(), *invert) == 0U);
    AOAHID_CHECK(barrel != nullptr && extract(report.data(), *barrel) == 0U);
    AOAHID_CHECK(pressure != nullptr && extract(report.data(), *pressure) == 0U);

    // Re-enter on the remembered eraser side, then switch back to the pen side.
    node.transfer_inflight.store(true);
    aoa::detail::transfer_complete(&node, AOAHID_OK, 0);
    sample.in_range = 1U;
    sample.eraser = 1U;
    AOAHID_CHECK(aoahid_pen_update(&node, &sample) == AOAHID_OK);
    report.fill(0U);
    AOAHID_CHECK(aoa::detail::serialize_node(&node, report.data(), report.size(), &length) ==
                 AOAHID_OK);
    AOAHID_CHECK(range != nullptr && extract(report.data(), *range) == 1U);
    AOAHID_CHECK(invert != nullptr && extract(report.data(), *invert) == 1U);
    node.transfer_inflight.store(true);
    aoa::detail::transfer_complete(&node, AOAHID_OK, 0);
    sample.eraser = 0U;
    AOAHID_CHECK(aoahid_pen_update(&node, &sample) == AOAHID_OK);
    report.fill(0U);
    AOAHID_CHECK(aoa::detail::serialize_node(&node, report.data(), report.size(), &length) ==
                 AOAHID_OK);
    AOAHID_CHECK(range != nullptr && extract(report.data(), *range) == 0U);
    AOAHID_CHECK(invert != nullptr && extract(report.data(), *invert) == 0U);
    AOAHID_CHECK(barrel != nullptr && extract(report.data(), *barrel) == 0U);
    aoahid_spec_release(spec);

    aoahid_digitizer_options digitizer{};
    digitizer.struct_size = static_cast<std::uint32_t>(sizeof(digitizer));
    digitizer.report_id = options.report_id;
    digitizer.x = options.x;
    digitizer.y = options.y;
    digitizer.enable_pressure = options.enable_pressure;
    digitizer.pressure = options.pressure;
    digitizer.barrel_usages = options.barrel_usages;
    digitizer.barrel_usage_count = options.barrel_usage_count;
    digitizer.enable_eraser = options.enable_eraser;
    digitizer.enable_hover = options.enable_hover;
    spec = nullptr;
    check_battery_variant(digitizer, &aoahid_spec_create_digitizer);
    AOAHID_CHECK(aoahid_spec_create_digitizer(&digitizer, &spec) == AOAHID_OK);
    if (spec != nullptr) {
        manifest = {};
        manifest.struct_size = static_cast<std::uint32_t>(sizeof(manifest));
        AOAHID_CHECK(aoahid_spec_manifest(spec, &manifest) == AOAHID_OK);
        AOAHID_CHECK(manifest.profile_kind == AOAHID_PROFILE_DIGITIZER);
        AOAHID_CHECK(manifest.android_status == AOAHID_ANDROID_CONDITIONAL);
        aoahid_node digitizer_node{};
        digitizer_node.spec = spec;
        digitizer_node.state = aoa::detail::PenState{};
        digitizer_node.transfer_inflight.store(true);
        aoa::detail::transfer_complete(&digitizer_node, AOAHID_OK, 0);
        aoahid_digitizer_sample digitizer_sample{};
        digitizer_sample.in_range = 1U;
        digitizer_sample.x = 10;
        digitizer_sample.y = 20;
        AOAHID_CHECK(aoahid_digitizer_update(&digitizer_node, &digitizer_sample) == AOAHID_OK);
        digitizer_sample.in_range = 0U;
        AOAHID_CHECK(aoahid_digitizer_update(&digitizer_node, &digitizer_sample) !=
                     AOAHID_ERR_PARAM);
        aoahid_spec_release(spec);
    }
}

void test_raw_report_id_order() {
    const std::array<std::uint8_t, 17> valid{0x05U, 0x01U, 0x09U, 0x06U, 0xA1U, 0x01U,
                                             0x85U, 0x01U, 0x15U, 0x00U, 0x25U, 0x01U,
                                             0x75U, 0x01U, 0x95U, 0x01U, 0x81U};
    // Complete the Input item data and collection in a separate exact array.
    const std::array<std::uint8_t, 21> accepted{0x05U, 0x01U, 0x09U, 0x06U, 0xA1U, 0x01U, 0x85U,
                                                0x01U, 0x15U, 0x00U, 0x25U, 0x01U, 0x75U, 0x01U,
                                                0x95U, 0x01U, 0x09U, 0x01U, 0x81U, 0x02U, 0xC0U};
    (void)valid;
    const aoahid_raw_report report{{1U, 1U, {}}, 2U};
    AOAHID_CHECK(aoa::hid::validate_raw(accepted.data(), accepted.size(), &report, 1U).result ==
                 AOAHID_OK);
    const std::array<std::uint8_t, 19> rejected{0x05U, 0x01U, 0x09U, 0x06U, 0xA1U, 0x01U, 0x15U,
                                                0x00U, 0x25U, 0x01U, 0x75U, 0x01U, 0x95U, 0x01U,
                                                0x81U, 0x02U, 0x85U, 0x01U, 0xC0U};
    AOAHID_CHECK(aoa::hid::validate_raw(rejected.data(), rejected.size(), &report, 1U).result !=
                 AOAHID_OK);

    const aoahid_raw_report no_id_report{{0U, 0U, {}}, 1U};
    const std::array<std::uint8_t, 17> output_item{0x05U, 0x01U, 0x09U, 0x06U, 0xA1U, 0x01U,
                                                   0x15U, 0x00U, 0x25U, 0x01U, 0x75U, 0x01U,
                                                   0x95U, 0x01U, 0x91U, 0x02U, 0xC0U};
    AOAHID_CHECK(
        aoa::hid::validate_raw(output_item.data(), output_item.size(), &no_id_report, 1U).result ==
        AOAHID_ERR_UNSUPPORTED);

    const std::array<std::uint8_t, 19> restored_id_zero{
        0x05U, 0x01U, 0x09U, 0x06U, 0xA1U, 0x01U, 0xA4U, 0x85U, 0x01U, 0x75U,
        0x01U, 0x95U, 0x01U, 0x81U, 0x02U, 0xB4U, 0x81U, 0x02U, 0xC0U};
    AOAHID_CHECK(
        aoa::hid::validate_raw(restored_id_zero.data(), restored_id_zero.size(), &report, 1U)
            .result == AOAHID_ERR_PARAM);

    const std::array<std::uint8_t, 16> excessive_count{0x05U, 0x01U, 0x09U, 0x06U, 0xA1U, 0x01U,
                                                       0x75U, 0x01U, 0x97U, 0xFFU, 0xFFU, 0xFFU,
                                                       0xFFU, 0x81U, 0x02U, 0xC0U};
    AOAHID_CHECK(
        aoa::hid::validate_raw(excessive_count.data(), excessive_count.size(), &no_id_report, 1U)
            .result == AOAHID_ERR_PARAM);

    const std::array<std::uint8_t, 19> range_exceeds_size{
        0x05U, 0x01U, 0x09U, 0x06U, 0xA1U, 0x01U, 0x85U, 0x01U, 0x15U, 0x00U,
        0x25U, 0x03U, 0x75U, 0x01U, 0x95U, 0x01U, 0x81U, 0x02U, 0xC0U};
    AOAHID_CHECK(
        aoa::hid::validate_raw(range_exceeds_size.data(), range_exceeds_size.size(), &report, 1U)
            .result == AOAHID_ERR_PARAM);
}

void test_raw_report_table_report_id_rules() {
    const std::array<std::uint8_t, 21> descriptor{0x05U, 0x01U, 0x09U, 0x06U, 0xA1U, 0x01U, 0x85U,
                                                  0x01U, 0x15U, 0x00U, 0x25U, 0x01U, 0x75U, 0x01U,
                                                  0x95U, 0x01U, 0x09U, 0x01U, 0x81U, 0x02U, 0xC0U};
    const auto result_for = [&](const aoahid_raw_report& report) {
        return aoa::hid::validate_raw(descriptor.data(), descriptor.size(), &report, 1U).result;
    };
    AOAHID_CHECK(result_for({{1U, 1U, {}}, 2U}) == AOAHID_OK);
    // The flag is exactly zero or one.
    AOAHID_CHECK(result_for({{2U, 1U, {}}, 2U}) == AOAHID_ERR_PARAM);
    // A declared ID is 1 through 255, and a disabled ID is zero.
    AOAHID_CHECK(result_for({{1U, 0U, {}}, 2U}) == AOAHID_ERR_PARAM);
    AOAHID_CHECK(result_for({{0U, 1U, {}}, 2U}) == AOAHID_ERR_PARAM);
    // The reserved bytes are zero.
    AOAHID_CHECK(result_for({{1U, 1U, {1U, 0U, 0U}}, 2U}) == AOAHID_ERR_PARAM);
    AOAHID_CHECK(result_for({{1U, 1U, {0U, 0U, 1U}}, 2U}) == AOAHID_ERR_PARAM);
    // The wire length is not zero.
    AOAHID_CHECK(result_for({{1U, 1U, {}}, 0U}) == AOAHID_ERR_PARAM);

    // The same ID twice is one report identity listed twice.
    const std::array<aoahid_raw_report, 2> duplicate{aoahid_raw_report{{1U, 1U, {}}, 2U},
                                                     aoahid_raw_report{{1U, 1U, {}}, 2U}};
    AOAHID_CHECK(aoa::hid::validate_raw(descriptor.data(), descriptor.size(), duplicate.data(),
                                        duplicate.size())
                     .result == AOAHID_ERR_PARAM);

    // The raw spec reports the table back through the manifest unchanged.
    const aoahid_raw_report report{{1U, 1U, {}}, 2U};
    const aoahid_raw_options options{sizeof(aoahid_raw_options),
                                     0U,
                                     descriptor.data(),
                                     descriptor.size(),
                                     &report,
                                     1U,
                                     1U,
                                     0U,
                                     0U};
    aoahid_spec* spec = nullptr;
    AOAHID_CHECK(aoahid_spec_create_raw(&options, &spec) == AOAHID_OK);
    if (spec != nullptr) {
        aoahid_capability_manifest manifest{};
        manifest.struct_size = static_cast<std::uint32_t>(sizeof(manifest));
        AOAHID_CHECK(aoahid_spec_manifest(spec, &manifest) == AOAHID_OK);
        AOAHID_CHECK(manifest.report_count == 1U);
        if (manifest.report_count == 1U) {
            AOAHID_CHECK(manifest.reports[0].report_id.enabled == 1U);
            AOAHID_CHECK(manifest.reports[0].report_id.value == 1U);
            AOAHID_CHECK(manifest.reports[0].wire_length == 2U);
        }
        aoahid_spec_release(spec);
    }
}

void test_raw_factory_bounds_before_allocation() {
    const std::array<std::uint8_t, 1U> descriptor{0xC0U};
    const aoahid_raw_report report{};
    aoahid_raw_options options{};
    options.struct_size = static_cast<std::uint32_t>(sizeof(options));
    options.descriptor = descriptor.data();
    options.descriptor_length = descriptor.size();
    options.reports = &report;
    options.report_count = std::numeric_limits<std::size_t>::max();
    options.acknowledges_no_android_support = 1U;
    aoahid_spec* spec = reinterpret_cast<aoahid_spec*>(std::uintptr_t{1U});
    AOAHID_CHECK(aoahid_spec_create_raw(&options, &spec) == AOAHID_ERR_OVERFLOW);
    AOAHID_CHECK(spec == nullptr);

    options.reports = nullptr;
    AOAHID_CHECK(aoahid_spec_create_raw(&options, &spec) == AOAHID_ERR_UNSET_FIELD);
    AOAHID_CHECK(spec == nullptr);
}

void test_send_time_report_validation() {
    aoahid_keyboard_options options = keyboard_options();
    options.report_id = {1U, 5U, {0U, 0U, 0U}};
    aoahid_spec* spec = nullptr;
    AOAHID_CHECK(aoahid_spec_create_keyboard(&options, &spec) == AOAHID_OK);
    if (spec == nullptr)
        return;

    aoahid_node node{};
    node.spec = spec;
    aoa::detail::KeyboardState state_value{};
    const std::size_t key_count =
        static_cast<std::size_t>(options.usage_maximum - options.usage_minimum) + 1U;
    state_value.pressed_bitmap.resize(key_count);
    state_value.key_transitions.resize(key_count);
    node.state = std::move(state_value);
    AOAHID_CHECK(aoahid_keyboard_key(&node, 0x04U, 1U) == AOAHID_OK);
    std::array<std::uint8_t, 64U> report{};
    std::size_t length = 0U;
    AOAHID_CHECK(aoa::detail::serialize_node(&node, report.data(), report.size(), &length) ==
                 AOAHID_OK);
    AOAHID_CHECK(aoa::detail::validate_serialized_report(spec, report.data(), length) == AOAHID_OK);
    AOAHID_CHECK(aoa::detail::validate_serialized_report(spec, report.data(), length - 1U) ==
                 AOAHID_ERR_INTERNAL);

    report[0] = 4U;
    AOAHID_CHECK(aoa::detail::validate_serialized_report(spec, report.data(), length) ==
                 AOAHID_ERR_INTERNAL);
    report[0] = 5U;
    const auto* padding = field(spec, aoa::hid::FieldSemantic::constant_padding);
    AOAHID_CHECK(padding != nullptr);
    if (padding != nullptr) {
        report[padding->bit_offset / 8U] = static_cast<std::uint8_t>(
            report[padding->bit_offset / 8U] | (1U << (padding->bit_offset & 7U)));
        AOAHID_CHECK(aoa::detail::validate_serialized_report(spec, report.data(), length) ==
                     AOAHID_ERR_INTERNAL);
    }
    aoahid_spec_release(spec);
}
} // namespace

void test_battery_builder() {
    using aoa::hid::BatteryField;
    using aoa::hid::DescriptorBuilder;
    using aoa::hid::FieldSemantic;
    using aoa::hid::ReportLayout;

    const BatteryField battery{2U, 1, 100, 8U, {}};
    const auto emit = [&](DescriptorBuilder& builder) {
        return builder.begin_application(0x01U, 0x06U) && builder.set_report_id(true, 1U) &&
               builder.variable_range(0x07U, 0xE0U, 0xE7U, 1U, FieldSemantic::modifier) &&
               builder.end_collection();
    };
    ReportLayout input;
    ReportLayout battery_layout;
    DescriptorBuilder counting(nullptr, 65535U, &input);
    counting.set_battery(battery, &battery_layout);
    AOAHID_CHECK(emit(counting));
    counting.finish();
    AOAHID_CHECK(counting.good());
    std::vector<std::uint8_t> bytes(counting.descriptor_size());
    DescriptorBuilder emitting(bytes.data(), bytes.size(), nullptr);
    emitting.set_battery(battery, nullptr);
    AOAHID_CHECK(emit(emitting));
    emitting.finish();
    AOAHID_CHECK(emitting.good() && emitting.descriptor_size() == bytes.size());

    // Input layout is untouched by the battery report.
    AOAHID_CHECK(input.has_report_id && input.report_id == 1U && input.wire_bytes == 2U);
    AOAHID_CHECK(battery_layout.has_report_id && battery_layout.report_id == 2U &&
                 battery_layout.wire_bytes == 2U && battery_layout.fields.size() == 1U &&
                 battery_layout.fields[0].semantic == FieldSemantic::battery_strength &&
                 battery_layout.fields[0].bit_offset == 8U &&
                 battery_layout.fields[0].logical_minimum == 1 &&
                 battery_layout.fields[0].logical_maximum == 100);

    const std::vector<std::uint8_t> tail{0x85U, 0x02U, 0x05U, 0x06U, 0x09U, 0x20U,
                                         0x15U, 0x01U, 0x25U, 0x64U, 0x75U, 0x08U,
                                         0x95U, 0x01U, 0x81U, 0x02U, 0xC0U};
    AOAHID_CHECK(bytes.size() > tail.size() &&
                 std::equal(tail.begin(), tail.end(),
                            bytes.end() - static_cast<std::ptrdiff_t>(tail.size())));

    AOAHID_CHECK(
        aoa::hid::validate_generated(bytes.data(), bytes.size(), input, &battery_layout).result ==
        AOAHID_OK);
    // The same bytes checked against a wrong battery layout are refused.
    ReportLayout wrong = battery_layout;
    wrong.report_id = 3U;
    AOAHID_CHECK(aoa::hid::validate_generated(bytes.data(), bytes.size(), input, &wrong).result ==
                 AOAHID_ERR_INTERNAL);
    // Without a battery layout a second Report ID is still refused.
    AOAHID_CHECK(aoa::hid::validate_generated(bytes.data(), bytes.size(), input).result ==
                 AOAHID_ERR_INTERNAL);

    // A 7-bit field is padded to a whole byte.
    const BatteryField narrow{2U, 0, 100, 7U, {}};
    ReportLayout narrow_input;
    ReportLayout narrow_layout;
    DescriptorBuilder narrow_builder(nullptr, 65535U, &narrow_input);
    narrow_builder.set_battery(narrow, &narrow_layout);
    AOAHID_CHECK(emit(narrow_builder));
    narrow_builder.finish();
    AOAHID_CHECK(narrow_builder.good() && narrow_layout.wire_bytes == 2U &&
                 narrow_layout.fields.size() == 2U && !narrow_layout.fields[0].has_null_state &&
                 narrow_layout.fields[1].semantic == FieldSemantic::constant_padding);
}

void test_battery_options() {
    aoahid_keyboard_options keyboard = keyboard_options();
    check_battery_variant(keyboard, &aoahid_spec_create_keyboard);

    keyboard.report_id = {1U, 1U, {0U, 0U, 0U}};
    keyboard.enable_battery = 1U;
    keyboard.battery.report_id = {1U, 2U, {0U, 0U, 0U}};
    keyboard.battery.strength = {1, 100, 8U, {}};
    const auto rejects = [&](const char* field) {
        aoahid_spec* refused = nullptr;
        const bool param = aoahid_spec_create_keyboard(&keyboard, &refused) == AOAHID_ERR_PARAM;
        const std::string_view named = aoahid_last_error()->field;
        aoahid_spec_release(refused);
        return param && named == field;
    };
    const aoahid_keyboard_options good = keyboard;

    keyboard.enable_battery = 2U;
    AOAHID_CHECK(rejects("keyboard.enable_battery"));
    keyboard = good;
    keyboard.enable_battery = 0U; // battery left filled in
    AOAHID_CHECK(rejects("keyboard.battery"));
    keyboard = good;
    keyboard.report_id = {0U, 0U, {0U, 0U, 0U}}; // input report has no ID
    AOAHID_CHECK(rejects("keyboard.battery.report_id"));
    keyboard = good;
    keyboard.battery.report_id = {0U, 0U, {0U, 0U, 0U}};
    AOAHID_CHECK(rejects("keyboard.battery.report_id"));
    keyboard = good;
    keyboard.battery.report_id.value = 1U; // same as the input report
    AOAHID_CHECK(rejects("keyboard.battery.report_id"));
    keyboard = good;
    keyboard.battery.strength = {100, 1, 8U, {}};
    AOAHID_CHECK(rejects("keyboard.battery.strength"));
    keyboard = good;
    keyboard.battery.strength = {-1, 100, 8U, {}};
    AOAHID_CHECK(rejects("keyboard.battery.strength"));
    keyboard = good;
    keyboard.battery.strength = {1, 300, 8U, {}}; // does not fit the width
    AOAHID_CHECK(rejects("keyboard.battery.strength"));
}

void test_battery_state() {
    aoahid_keyboard_options keyboard = keyboard_options();
    keyboard.report_id = {1U, 1U, {0U, 0U, 0U}};
    aoahid_spec* plain = nullptr;
    AOAHID_CHECK(aoahid_spec_create_keyboard(&keyboard, &plain) == AOAHID_OK);
    keyboard.enable_battery = 1U;
    keyboard.battery.report_id = {1U, 2U, {0U, 0U, 0U}};
    keyboard.battery.strength = {0, 100, 8U, {}};
    aoahid_spec* spec = nullptr;
    AOAHID_CHECK(aoahid_spec_create_keyboard(&keyboard, &spec) == AOAHID_OK);
    if (plain == nullptr || spec == nullptr) {
        aoahid_spec_release(plain);
        aoahid_spec_release(spec);
        return;
    }

    aoahid_node without{};
    without.spec = plain;
    without.state = aoa::detail::KeyboardState{};
    AOAHID_CHECK(aoahid_battery_update(&without, 50) == AOAHID_ERR_PARAM);

    aoahid_node node{};
    node.spec = spec;
    node.state = aoa::detail::KeyboardState{};
    std::array<std::uint8_t, 8U> report{};
    std::size_t length = 0U;

    // Nothing was ever set, so there is nothing to send.
    AOAHID_CHECK(!node.battery_dirty);
    // The first value is a change even when it is 0, the initial storage.
    AOAHID_CHECK(aoahid_battery_update(&node, 0) == AOAHID_OK && node.battery_dirty);
    AOAHID_CHECK(aoa::detail::serialize_battery(&node, report.data(), report.size(), &length) ==
                 AOAHID_OK);
    AOAHID_CHECK(length == 2U && report[0] == 2U && report[1] == 0U);

    AOAHID_CHECK(aoahid_battery_update(&node, 80) == AOAHID_OK);
    AOAHID_CHECK(node.battery_dirty && !node.dirty);
    AOAHID_CHECK(aoa::detail::serialize_battery(&node, report.data(), report.size(), &length) ==
                 AOAHID_OK);
    AOAHID_CHECK(length == 2U && report[0] == 2U && report[1] == 80U);
    AOAHID_CHECK(aoa::detail::validate_serialized_report(spec, spec->battery_layout, report.data(),
                                                         length) == AOAHID_OK);
    report[1] = 101U;
    AOAHID_CHECK(aoa::detail::validate_serialized_report(spec, spec->battery_layout, report.data(),
                                                         length) == AOAHID_ERR_INTERNAL);

    // The same value again asks for nothing.
    node.battery_dirty = false;
    AOAHID_CHECK(aoahid_battery_update(&node, 80) == AOAHID_OK && !node.battery_dirty);
    AOAHID_CHECK(aoahid_battery_update(&node, 79) == AOAHID_OK && node.battery_dirty);

    AOAHID_CHECK(aoahid_battery_update(&node, -1) == AOAHID_ERR_PARAM);
    AOAHID_CHECK(aoahid_battery_update(&node, 101) == AOAHID_ERR_PARAM);
    AOAHID_CHECK(aoahid_battery_update(nullptr, 50) == AOAHID_ERR_PARAM);

    node.transfer_inflight.store(true);
    AOAHID_CHECK(aoahid_battery_update(&node, 10) == AOAHID_ERR_BUSY);
    node.transfer_inflight.store(false);

    aoahid_spec_release(plain);
    aoahid_spec_release(spec);
}

// Every options error names the structure the caller filled in, not the
// implementation two profiles share.
void test_error_fields_name_their_profile() {
    const auto field_starts_with = [](const std::string_view prefix) {
        const char* field = aoahid_last_error()->field;
        return field != nullptr && std::string_view(field).substr(0, prefix.size()) == prefix;
    };
    aoahid_spec* spec = nullptr;
    aoahid_touchscreen_options touchscreen = touchscreen_options();
    touchscreen.x = {};
    AOAHID_CHECK(aoahid_spec_create_touchscreen(&touchscreen, &spec) != AOAHID_OK);
    AOAHID_CHECK(field_starts_with("touchscreen"));
    aoahid_touchpad_options touchpad = touchpad_options();
    touchpad.x = {};
    AOAHID_CHECK(aoahid_spec_create_touchpad(&touchpad, &spec) != AOAHID_OK);
    AOAHID_CHECK(field_starts_with("touchpad"));
    aoahid_pen_options pen{};
    pen.struct_size = static_cast<std::uint32_t>(sizeof(pen));
    AOAHID_CHECK(aoahid_spec_create_pen(&pen, &spec) != AOAHID_OK);
    AOAHID_CHECK(field_starts_with("pen"));
    aoahid_digitizer_options digitizer{};
    digitizer.struct_size = static_cast<std::uint32_t>(sizeof(digitizer));
    AOAHID_CHECK(aoahid_spec_create_digitizer(&digitizer, &spec) != AOAHID_OK);
    AOAHID_CHECK(field_starts_with("digitizer"));
}

void test_profiles() {
    aoahid_keyboard_options zero{};
    auto* spec = reinterpret_cast<aoahid_spec*>(static_cast<std::uintptr_t>(1U));
    AOAHID_CHECK(aoahid_spec_create_keyboard(&zero, &spec) != AOAHID_OK);
    AOAHID_CHECK(spec == nullptr);
    AOAHID_CHECK(aoahid_spec_create_keyboard(&zero, nullptr) == AOAHID_ERR_PARAM);

    aoahid_keyboard_options invalid_keyboard = keyboard_options();
    invalid_keyboard.report_id.reserved8[0] = 1U;
    AOAHID_CHECK(aoahid_spec_create_keyboard(&invalid_keyboard, &spec) == AOAHID_ERR_PARAM);
    invalid_keyboard = keyboard_options();
    invalid_keyboard.usage_minimum = 1U;
    AOAHID_CHECK(aoahid_spec_create_keyboard(&invalid_keyboard, &spec) == AOAHID_ERR_PARAM);
    invalid_keyboard = keyboard_options();
    invalid_keyboard.usage_maximum =
        static_cast<std::uint16_t>(invalid_keyboard.usage_minimum - 1U);
    AOAHID_CHECK(aoahid_spec_create_keyboard(&invalid_keyboard, &spec) == AOAHID_ERR_PARAM);
    invalid_keyboard = keyboard_options();
    invalid_keyboard.usage_maximum = 0xE0U;
    AOAHID_CHECK(aoahid_spec_create_keyboard(&invalid_keyboard, &spec) == AOAHID_ERR_PARAM);

    test_keyboard_nkro_concurrent_keys();
    test_keyboard_bitmap_and_lifecycle_transitions();
    test_mouse_delta_splitting_and_consumption();
    test_usage_control_semantics();
    test_hat_domain();
    test_controller_dpad_and_extended_axes();
    test_physical_properties_and_reset();
    test_field_span_padding();
    test_touch_units_feature_and_manifest_status();
    test_touch_azimuth();
    test_touch_packets();
    test_touch_lift_frame_and_idle_suppression();
    test_touchpad_button_only_and_mid_frame_guard();
    test_pen_away_coordinates();
    test_pen_switch();
    test_raw_report_id_order();
    test_raw_report_table_report_id_rules();
    test_raw_factory_bounds_before_allocation();
    test_send_time_report_validation();
    test_battery_builder();
    test_battery_options();
    test_battery_state();
    test_error_fields_name_their_profile();

    aoahid_touchscreen_options invalid_touch = touchscreen_options();
    invalid_touch.contact_id = {1, 15, 4U, {}};
    AOAHID_CHECK(aoahid_spec_create_touchscreen(&invalid_touch, &spec) == AOAHID_ERR_PARAM);

    aoahid_touchpad_options invalid_touchpad = touchpad_options();
    invalid_touchpad.button_count = 65536U;
    AOAHID_CHECK(aoahid_spec_create_touchpad(&invalid_touchpad, &spec) == AOAHID_ERR_OVERFLOW);

    aoahid_touchpad_options buttonless = touchpad_options();
    buttonless.button_count = 0U;
    AOAHID_CHECK(aoahid_spec_create_touchpad(&buttonless, &spec) == AOAHID_OK);
    if (spec != nullptr) {
        aoahid_capability_manifest manifest{};
        manifest.struct_size = static_cast<std::uint32_t>(sizeof(manifest));
        AOAHID_CHECK(aoahid_spec_manifest(spec, &manifest) == AOAHID_OK);
        AOAHID_CHECK(manifest.profile_kind == AOAHID_PROFILE_TOUCHPAD);
        AOAHID_CHECK(manifest.android_status == AOAHID_ANDROID_CONDITIONAL);
    }
    aoahid_spec_release(spec);
    spec = nullptr;

    // The profile numbers are part of the ABI.
    AOAHID_CHECK(AOAHID_PROFILE_KEYBOARD == 1 && AOAHID_PROFILE_MOUSE == 2 &&
                 AOAHID_PROFILE_TOUCHSCREEN == 3 && AOAHID_PROFILE_TOUCHPAD == 4 &&
                 AOAHID_PROFILE_PEN == 5 && AOAHID_PROFILE_DIGITIZER == 6 &&
                 AOAHID_PROFILE_GAMEPAD == 7 && AOAHID_PROFILE_TOGGLE == 8 &&
                 AOAHID_PROFILE_RAW == 9);
}
