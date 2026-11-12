// SPDX-License-Identifier: MIT
// Copyright (c) 2026 libaoahid contributors
/* Builds immutable HID descriptors and their matching report layouts; it has
 * no transport, device-state, or platform-classification responsibility. */
#pragma once

/*
 * This file builds a report descriptor and its input layout in the same pass.
 * It is pure construction code and never performs USB or platform I/O.
 */

#include "hid/item_writer.hpp"
#include "hid/report_layout.hpp"
#include "hid/validator.hpp"

#include <array>
#include <cstddef>
#include <cstdint>

namespace aoa::hid {

class DescriptorBuilder final {
  public:
    DescriptorBuilder(std::uint8_t* bytes, std::size_t capacity, ReportLayout* layout) noexcept;

    bool begin_application(std::uint16_t usage_page, std::uint16_t usage) noexcept;
    bool begin_logical(std::uint16_t usage_page, std::uint16_t usage) noexcept;
    bool begin_physical(std::uint16_t usage_page, std::uint16_t usage) noexcept;
    bool end_collection();
    bool set_report_id(bool enabled, std::uint8_t report_id) noexcept;
    // Makes the top-level End Collection write the battery report first.
    void set_battery(const BatteryField& field, ReportLayout* layout) noexcept;

    bool variable(std::uint16_t usage_page, std::uint16_t usage, std::int32_t logical_minimum,
                  std::int32_t logical_maximum, std::uint8_t bit_width, bool relative,
                  bool null_state, FieldSemantic semantic, std::uint16_t instance,
                  bool no_preferred = false, const aoahid_physical_properties* physical = nullptr);
    bool variable_range(std::uint16_t usage_page, std::uint16_t usage_minimum,
                        std::uint16_t usage_maximum, std::uint8_t bit_width,
                        FieldSemantic semantic);
    bool constant_padding(std::uint16_t bits);
    bool feature_static_value(std::uint16_t usage_page, std::uint16_t usage,
                              std::int32_t logical_minimum, std::int32_t logical_maximum,
                              std::uint8_t bit_width) noexcept;

    [[nodiscard]] bool good() const noexcept;
    [[nodiscard]] std::size_t descriptor_size() const noexcept;
    [[nodiscard]] std::size_t input_bits() const noexcept;
    [[nodiscard]] DescriptorRequirements requirements() const noexcept;
    void finish() noexcept;

  private:
    bool add_layout(FieldSemantic semantic, std::uint16_t instance, std::uint8_t bit_width,
                    std::int32_t logical_minimum, std::int32_t logical_maximum,
                    const aoahid_physical_properties* physical = nullptr,
                    bool has_null_state = false);
    void record_main(bool feature, std::uint64_t local_usage_count, std::uint64_t report_count,
                     std::uint64_t report_size) noexcept;
    void observe_usages(std::uint64_t count) noexcept;
    bool prepare_data_main() noexcept;
    bool emit_battery();

    ItemWriter writer_;
    ReportLayout* layout_{};
    BatteryField battery_{};
    ReportLayout* battery_layout_{};
    bool has_battery_{};
    std::size_t input_bits_{};
    std::uint8_t current_report_id_{};
    std::array<std::uint32_t, 256U> input_fields_{};
    std::array<std::uint32_t, 256U> feature_fields_{};
    std::array<std::uint64_t, 256U> input_report_bits_{};
    std::array<std::uint64_t, 256U> feature_report_bits_{};
    DescriptorRequirements requirements_{};
    std::size_t collection_depth_{};
    bool top_level_application_seen_{};
    bool report_id_decided_{};
    bool data_main_emitted_{};
    bool good_{true};
};

} // namespace aoa::hid
