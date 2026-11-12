// SPDX-License-Identifier: MIT
// Copyright (c) 2026 libaoahid contributors
/*
 * This file emits descriptor bytes and immutable field offsets together so a
 * serializer can never silently drift away from the descriptor it serves.
 */

#include "hid/descriptor_builder.hpp"

#include "hid/usages.hpp"

#include <algorithm>
#include <limits>

namespace aoa::hid {

namespace {
constexpr std::uint32_t kCollectionPhysical = 0x00U;
constexpr std::uint32_t kCollectionApplication = 0x01U;
constexpr std::uint32_t kCollectionLogical = 0x02U;
constexpr std::uint32_t kInputConstant = 0x01U;
constexpr std::uint32_t kInputVariable = 0x02U;
constexpr std::uint32_t kInputRelative = 0x04U;
constexpr std::uint32_t kInputNoPreferred = 0x20U;
constexpr std::uint32_t kInputNullState = 0x40U;
} // namespace

DescriptorBuilder::DescriptorBuilder(std::uint8_t* bytes, const std::size_t capacity,
                                     ReportLayout* layout) noexcept
    : writer_(bytes, capacity), layout_(layout) {}

bool DescriptorBuilder::begin_application(const std::uint16_t usage_page,
                                          const std::uint16_t usage) noexcept {
    if (!good_ || (collection_depth_ == 0U && top_level_application_seen_)) {
        good_ = false;
        return false;
    }
    const bool is_top_level = collection_depth_ == 0U;
    observe_usages(1U);
    good_ = good_ && writer_.usage_page(usage_page) && writer_.usage(usage) &&
            writer_.collection(kCollectionApplication);
    if (good_) {
        top_level_application_seen_ = top_level_application_seen_ || is_top_level;
        ++collection_depth_;
    }
    return good_;
}

bool DescriptorBuilder::begin_logical(const std::uint16_t usage_page,
                                      const std::uint16_t usage) noexcept {
    if (!good_ || collection_depth_ == 0U) {
        good_ = false;
        return false;
    }
    observe_usages(1U);
    good_ = good_ && writer_.usage_page(usage_page) && writer_.usage(usage) &&
            writer_.collection(kCollectionLogical);
    if (good_) {
        ++collection_depth_;
    }
    return good_;
}

bool DescriptorBuilder::begin_physical(const std::uint16_t usage_page,
                                       const std::uint16_t usage) noexcept {
    if (!good_ || collection_depth_ == 0U) {
        good_ = false;
        return false;
    }
    observe_usages(1U);
    good_ = good_ && writer_.usage_page(usage_page) && writer_.usage(usage) &&
            writer_.collection(kCollectionPhysical);
    if (good_) {
        ++collection_depth_;
    }
    return good_;
}

bool DescriptorBuilder::end_collection() {
    if (!good_ || collection_depth_ == 0U) {
        good_ = false;
        return false;
    }
    if (collection_depth_ == 1U && has_battery_ && !emit_battery()) {
        return false;
    }
    good_ = writer_.end_collection();
    if (good_) {
        --collection_depth_;
    }
    return good_;
}

bool DescriptorBuilder::set_report_id(const bool enabled, const std::uint8_t report_id) noexcept {
    if (!good_ || report_id_decided_ || data_main_emitted_) {
        good_ = false;
        return false;
    }
    report_id_decided_ = true;
    if (!enabled) {
        current_report_id_ = 0U;
        if (layout_ != nullptr) {
            layout_->has_report_id = false;
            layout_->report_id = 0U;
        }
        input_bits_ = 0U;
        return true;
    }
    if (report_id == 0U) {
        good_ = false;
        return false;
    }
    good_ = good_ && writer_.report_id(report_id);
    current_report_id_ = report_id;
    if (layout_ != nullptr) {
        layout_->has_report_id = true;
        layout_->report_id = report_id;
    }
    input_bits_ = 8U;
    return good_;
}

void DescriptorBuilder::set_battery(const BatteryField& field, ReportLayout* layout) noexcept {
    battery_ = field;
    battery_layout_ = layout;
    has_battery_ = true;
}

// Switches to the battery report: its own Report ID, its own layout, its own
// bit count. Nothing may follow it but the End Collection.
bool DescriptorBuilder::emit_battery() {
    if (!good_ || current_report_id_ == 0U || battery_.report_id == 0U ||
        battery_.report_id == current_report_id_) {
        good_ = false;
        return false;
    }
    if (layout_ != nullptr) {
        layout_->wire_bytes = (input_bits_ + 7U) / 8U;
    }
    good_ = writer_.report_id(battery_.report_id);
    current_report_id_ = battery_.report_id;
    layout_ = battery_layout_;
    if (layout_ != nullptr) {
        layout_->has_report_id = true;
        layout_->report_id = battery_.report_id;
    }
    input_bits_ = 8U;
    has_battery_ = false;
    if (!variable(usage::page_generic_device_controls, usage::battery_strength,
                  battery_.logical_minimum, battery_.logical_maximum, battery_.bit_width, false,
                  false, FieldSemantic::battery_strength, 0U, false, &battery_.physical)) {
        return false;
    }
    const std::size_t padding = (8U - (input_bits_ & 7U)) & 7U;
    return padding == 0U || constant_padding(static_cast<std::uint16_t>(padding));
}

bool DescriptorBuilder::prepare_data_main() noexcept {
    if (!good_ || !report_id_decided_) {
        good_ = false;
        return false;
    }
    data_main_emitted_ = true;
    return true;
}

bool DescriptorBuilder::add_layout(const FieldSemantic semantic, const std::uint16_t instance,
                                   const std::uint8_t bit_width, const std::int32_t logical_minimum,
                                   const std::int32_t logical_maximum,
                                   const aoahid_physical_properties* physical,
                                   const bool has_null_state) {
    if (bit_width == 0U || bit_width > 32U ||
        input_bits_ > std::numeric_limits<std::size_t>::max() - bit_width) {
        good_ = false;
        return false;
    }
    const aoahid_physical_properties physical_value =
        physical == nullptr ? aoahid_physical_properties{} : *physical;
    if (layout_ != nullptr) {
        layout_->fields.push_back(FieldLayout{
            semantic, instance, input_bits_, bit_width, logical_minimum < 0 || logical_maximum < 0,
            has_null_state, logical_minimum, logical_maximum, physical_value});
    }
    input_bits_ += bit_width;
    return true;
}

void DescriptorBuilder::observe_usages(const std::uint64_t count) noexcept {
    requirements_.maximum_usages = std::max(requirements_.maximum_usages, count);
}

void DescriptorBuilder::record_main(const bool feature, const std::uint64_t local_usage_count,
                                    const std::uint64_t report_count,
                                    const std::uint64_t report_size) noexcept {
    observe_usages(std::max(local_usage_count, report_count));
    if (report_size > std::numeric_limits<std::uint32_t>::max()) {
        good_ = false;
        return;
    }
    requirements_.maximum_report_size_bits =
        std::max(requirements_.maximum_report_size_bits, static_cast<std::uint32_t>(report_size));
    if (report_size != 0U &&
        report_count > std::numeric_limits<std::uint64_t>::max() / report_size) {
        good_ = false;
        return;
    }
    auto& report_bits = feature ? feature_report_bits_ : input_report_bits_;
    std::uint64_t& bits = report_bits[current_report_id_];
    const std::uint64_t added_bits = report_size * report_count;
    if (bits > std::numeric_limits<std::uint64_t>::max() - added_bits) {
        good_ = false;
        return;
    }
    bits += added_bits;
    requirements_.maximum_report_data_bits = std::max(requirements_.maximum_report_data_bits, bits);
    if (local_usage_count == 0U) {
        return;
    }
    auto& fields = feature ? feature_fields_ : input_fields_;
    std::uint32_t& count = fields[current_report_id_];
    if (count == std::numeric_limits<std::uint32_t>::max()) {
        good_ = false;
        return;
    }
    ++count;
    requirements_.maximum_fields_per_report =
        std::max(requirements_.maximum_fields_per_report, count);
}

bool DescriptorBuilder::variable(const std::uint16_t usage_page, const std::uint16_t usage,
                                 const std::int32_t logical_minimum,
                                 const std::int32_t logical_maximum, const std::uint8_t bit_width,
                                 const bool relative, const bool null_state,
                                 const FieldSemantic semantic, const std::uint16_t instance,
                                 const bool no_preferred,
                                 const aoahid_physical_properties* physical) {
    if (!prepare_data_main()) {
        return false;
    }
    // HID 1.11 section 8.4 lets a field touch at most four bytes. A field that
    // would reach a fifth starts on the next byte boundary instead.
    const std::size_t lead_bits = input_bits_ & 7U;
    if (lead_bits != 0U && lead_bits + bit_width > 32U &&
        !constant_padding(static_cast<std::uint16_t>(8U - lead_bits))) {
        return false;
    }
    std::uint32_t flags = kInputVariable;
    if (relative) {
        flags |= kInputRelative;
    }
    if (no_preferred) {
        flags |= kInputNoPreferred;
    }
    if (null_state) {
        flags |= kInputNullState;
    }
    bool has_physical = false;
    if (physical != nullptr) {
        const bool canonical_disabled = physical->enabled == 0U && physical->minimum == 0 &&
                                        physical->maximum == 0 && physical->unit_exponent == 0 &&
                                        physical->unit == 0U;
        const bool valid_enabled = physical->enabled == 1U &&
                                   physical->maximum > physical->minimum &&
                                   physical->unit_exponent >= -8 && physical->unit_exponent <= 7 &&
                                   physical->unit != 0U && unit_is_valid(physical->unit);
        if (!canonical_disabled && !valid_enabled) {
            good_ = false;
            return false;
        }
        has_physical = valid_enabled;
    }
    good_ = good_ && writer_.usage_page(usage_page) && writer_.usage(usage) &&
            writer_.logical_minimum(logical_minimum) &&
            writer_.logical_maximum(logical_maximum, logical_minimum);
    if (good_ && has_physical) {
        good_ = writer_.physical_minimum(physical->minimum) &&
                writer_.physical_maximum(physical->maximum, physical->minimum) &&
                writer_.unit_exponent(physical->unit_exponent) && writer_.unit(physical->unit);
    }
    good_ =
        good_ && writer_.report_size(bit_width) && writer_.report_count(1U) && writer_.input(flags);
    if (good_) {
        record_main(false, 1U, 1U, bit_width);
    }
    if (good_ && has_physical) {
        good_ = writer_.physical_minimum(0) && writer_.physical_maximum(0, 0) &&
                writer_.unit_exponent(0) && writer_.unit(0U);
    }
    good_ = good_ && add_layout(semantic, instance, bit_width, logical_minimum, logical_maximum,
                                physical, null_state);
    return good_;
}

bool DescriptorBuilder::variable_range(const std::uint16_t usage_page,
                                       const std::uint16_t usage_minimum,
                                       const std::uint16_t usage_maximum,
                                       const std::uint8_t bit_width, const FieldSemantic semantic) {
    if (!prepare_data_main()) {
        return false;
    }
    if (usage_maximum < usage_minimum) {
        good_ = false;
        return false;
    }
    const std::uint32_t count = static_cast<std::uint32_t>(usage_maximum - usage_minimum) + 1U;
    good_ = good_ && writer_.usage_page(usage_page) && writer_.usage_minimum(usage_minimum) &&
            writer_.usage_maximum(usage_maximum) && writer_.logical_minimum(0) &&
            writer_.logical_maximum(1, 0) && writer_.report_size(bit_width) &&
            writer_.report_count(count) && writer_.input(kInputVariable);
    if (good_) {
        record_main(false, count, count, bit_width);
    }
    for (std::uint32_t index = 0; good_ && index < count; ++index) {
        good_ = add_layout(semantic, static_cast<std::uint16_t>(index), bit_width, 0, 1);
    }
    return good_;
}

bool DescriptorBuilder::constant_padding(const std::uint16_t bits) {
    if (bits == 0U) {
        return true;
    }
    if (!prepare_data_main()) {
        return false;
    }
    good_ = good_ && writer_.report_size(bits) && writer_.report_count(1U) &&
            writer_.input(kInputConstant);
    if (good_) {
        record_main(false, 0U, 1U, bits);
    }
    good_ = good_ &&
            add_layout(FieldSemantic::constant_padding, 0U, static_cast<std::uint8_t>(bits), 0, 0);
    return good_;
}

bool DescriptorBuilder::feature_static_value(const std::uint16_t usage_page,
                                             const std::uint16_t usage,
                                             const std::int32_t logical_minimum,
                                             const std::int32_t logical_maximum,
                                             const std::uint8_t bit_width) noexcept {
    if (!prepare_data_main()) {
        return false;
    }
    good_ = good_ && writer_.usage_page(usage_page) && writer_.usage(usage) &&
            writer_.logical_minimum(logical_minimum) &&
            writer_.logical_maximum(logical_maximum, logical_minimum) &&
            writer_.report_size(bit_width) && writer_.report_count(1U) &&
            writer_.feature(kInputConstant | kInputVariable);
    if (good_) {
        record_main(true, 1U, 1U, bit_width);
    }
    return good_;
}

bool DescriptorBuilder::good() const noexcept { return good_ && writer_.good(); }
std::size_t DescriptorBuilder::descriptor_size() const noexcept { return writer_.size(); }
std::size_t DescriptorBuilder::input_bits() const noexcept { return input_bits_; }
DescriptorRequirements DescriptorBuilder::requirements() const noexcept { return requirements_; }
void DescriptorBuilder::finish() noexcept {
    if (collection_depth_ != 0U) {
        good_ = false;
    }
    if (layout_ != nullptr) {
        layout_->wire_bytes = (input_bits_ + 7U) / 8U;
    }
}

} // namespace aoa::hid
