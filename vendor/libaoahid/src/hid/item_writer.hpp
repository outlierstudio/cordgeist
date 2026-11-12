// SPDX-License-Identifier: MIT
// Copyright (c) 2026 libaoahid contributors
/* Encodes bounded HID short items into caller-owned storage; it neither
 * allocates report objects nor interprets operating-system behavior. */
#pragma once

/*
 * This file emits HID 1.11 short items into caller-owned storage. A counting
 * pass permits exact allocation without imposing a descriptor-size ceiling.
 */

#include <cstddef>
#include <cstdint>

namespace aoa::hid {

class ItemWriter final {
  public:
    ItemWriter(std::uint8_t* bytes, std::size_t capacity) noexcept;

    bool usage_page(std::uint32_t value) noexcept;
    bool usage(std::uint32_t value) noexcept;
    bool usage_minimum(std::uint32_t value) noexcept;
    bool usage_maximum(std::uint32_t value) noexcept;
    bool logical_minimum(std::int32_t value) noexcept;
    bool logical_maximum(std::int32_t value, std::int32_t declared_minimum) noexcept;
    bool physical_minimum(std::int32_t value) noexcept;
    bool physical_maximum(std::int32_t value, std::int32_t declared_minimum) noexcept;
    bool unit_exponent(std::int32_t value) noexcept;
    bool unit(std::uint32_t value) noexcept;
    bool report_size(std::uint32_t value) noexcept;
    bool report_id(std::uint32_t value) noexcept;
    bool report_count(std::uint32_t value) noexcept;
    bool push() noexcept;
    bool pop() noexcept;
    bool input(std::uint32_t flags) noexcept;
    bool feature(std::uint32_t flags) noexcept;
    bool collection(std::uint32_t type) noexcept;
    bool end_collection() noexcept;

    [[nodiscard]] bool good() const noexcept;
    [[nodiscard]] std::size_t size() const noexcept;

  private:
    struct EncodedItem {
        std::uint8_t base;
        std::uint32_t value;
        std::size_t bytes;
    };

    bool unsigned_item(std::uint8_t base, std::uint32_t value) noexcept;
    bool signed_item(std::uint8_t base, std::int32_t value) noexcept;
    bool emit(EncodedItem item) noexcept;

    std::uint8_t* bytes_{};
    std::size_t capacity_{};
    std::size_t position_{};
    bool good_{true};
};

} // namespace aoa::hid
