// SPDX-License-Identifier: MIT
// Copyright (c) 2026 libaoahid contributors
/*
 * This file implements compact HID 1.11 short-item encoding. It does not know
 * about Android, profiles, report state, or USB transport.
 */

#include "hid/item_writer.hpp"

#include <limits>

namespace aoa::hid {

namespace {
constexpr std::uint8_t kInput = 0x80U;
constexpr std::uint8_t kCollection = 0xA0U;
constexpr std::uint8_t kFeature = 0xB0U;
constexpr std::uint8_t kEndCollection = 0xC0U;
constexpr std::uint8_t kUsagePage = 0x04U;
constexpr std::uint8_t kLogicalMinimum = 0x14U;
constexpr std::uint8_t kLogicalMaximum = 0x24U;
constexpr std::uint8_t kPhysicalMinimum = 0x34U;
constexpr std::uint8_t kPhysicalMaximum = 0x44U;
constexpr std::uint8_t kUnitExponent = 0x54U;
constexpr std::uint8_t kUnit = 0x64U;
constexpr std::uint8_t kReportSize = 0x74U;
constexpr std::uint8_t kReportId = 0x84U;
constexpr std::uint8_t kReportCount = 0x94U;
constexpr std::uint8_t kPush = 0xA4U;
constexpr std::uint8_t kPop = 0xB4U;
constexpr std::uint8_t kUsage = 0x08U;
constexpr std::uint8_t kUsageMinimum = 0x18U;
constexpr std::uint8_t kUsageMaximum = 0x28U;
} // namespace

ItemWriter::ItemWriter(std::uint8_t* bytes, const std::size_t capacity) noexcept
    : bytes_(bytes), capacity_(capacity) {}

bool ItemWriter::emit(const EncodedItem item) noexcept {
    if (!good_) {
        return false;
    }
    const std::uint8_t size_code = item.bytes == 4U ? 3U : static_cast<std::uint8_t>(item.bytes);
    if (position_ > capacity_ || item.bytes + 1U > capacity_ - position_) {
        good_ = false;
        return false;
    }
    if (bytes_ != nullptr) {
        bytes_[position_] = static_cast<std::uint8_t>(item.base | size_code);
        for (std::size_t index = 0; index < item.bytes; ++index) {
            bytes_[position_ + 1U + index] =
                static_cast<std::uint8_t>((item.value >> (index * 8U)) & 0xFFU);
        }
    }
    position_ += item.bytes + 1U;
    return true;
}

bool ItemWriter::unsigned_item(const std::uint8_t base, const std::uint32_t value) noexcept {
    if (value <= std::numeric_limits<std::uint8_t>::max()) {
        return emit(EncodedItem{base, value, 1U});
    }
    if (value <= std::numeric_limits<std::uint16_t>::max()) {
        return emit(EncodedItem{base, value, 2U});
    }
    return emit(EncodedItem{base, value, 4U});
}

bool ItemWriter::signed_item(const std::uint8_t base, const std::int32_t value) noexcept {
    if (value >= std::numeric_limits<std::int8_t>::min() &&
        value <= std::numeric_limits<std::int8_t>::max()) {
        return emit(EncodedItem{base, static_cast<std::uint32_t>(value), 1U});
    }
    if (value >= std::numeric_limits<std::int16_t>::min() &&
        value <= std::numeric_limits<std::int16_t>::max()) {
        return emit(EncodedItem{base, static_cast<std::uint32_t>(value), 2U});
    }
    return emit(EncodedItem{base, static_cast<std::uint32_t>(value), 4U});
}

bool ItemWriter::usage_page(const std::uint32_t value) noexcept {
    return unsigned_item(kUsagePage, value);
}
bool ItemWriter::usage(const std::uint32_t value) noexcept { return unsigned_item(kUsage, value); }
bool ItemWriter::usage_minimum(const std::uint32_t value) noexcept {
    return unsigned_item(kUsageMinimum, value);
}
bool ItemWriter::usage_maximum(const std::uint32_t value) noexcept {
    return unsigned_item(kUsageMaximum, value);
}
bool ItemWriter::logical_minimum(const std::int32_t value) noexcept {
    // HID 1.11 defines the Minimum items as signed data, even when positive.
    return signed_item(kLogicalMinimum, value);
}
bool ItemWriter::logical_maximum(const std::int32_t value,
                                 const std::int32_t declared_minimum) noexcept {
    // HID 1.11 sections 5.8 and 6.2.2.7 make a field unsigned when both
    // extents are non-negative. Therefore 65535 with minimum zero is a
    // two-byte value, not a sign-extended four-byte value.
    return declared_minimum >= 0 && value >= 0
               ? unsigned_item(kLogicalMaximum, static_cast<std::uint32_t>(value))
               : signed_item(kLogicalMaximum, value);
}
bool ItemWriter::physical_minimum(const std::int32_t value) noexcept {
    return signed_item(kPhysicalMinimum, value);
}
bool ItemWriter::physical_maximum(const std::int32_t value,
                                  const std::int32_t declared_minimum) noexcept {
    return declared_minimum >= 0 && value >= 0
               ? unsigned_item(kPhysicalMaximum, static_cast<std::uint32_t>(value))
               : signed_item(kPhysicalMaximum, value);
}
bool ItemWriter::unit_exponent(const std::int32_t value) noexcept {
    // HID 1.11 section 6.2.2.7 encodes Unit Exponent as a signed four-bit
    // nibble. Linux accepts legacy full-byte sign extension too, but emitting
    // that compatibility form would make the descriptor non-canonical.
    if (value < -8 || value > 7) {
        good_ = false;
        return false;
    }
    return emit(EncodedItem{kUnitExponent, static_cast<std::uint32_t>(value) & 0x0FU, 1U});
}
bool ItemWriter::unit(const std::uint32_t value) noexcept { return unsigned_item(kUnit, value); }
bool ItemWriter::report_size(const std::uint32_t value) noexcept {
    return unsigned_item(kReportSize, value);
}
bool ItemWriter::report_id(const std::uint32_t value) noexcept {
    return unsigned_item(kReportId, value);
}
bool ItemWriter::report_count(const std::uint32_t value) noexcept {
    return unsigned_item(kReportCount, value);
}
bool ItemWriter::push() noexcept { return emit(EncodedItem{kPush, 0U, 0U}); }
bool ItemWriter::pop() noexcept { return emit(EncodedItem{kPop, 0U, 0U}); }
bool ItemWriter::input(const std::uint32_t flags) noexcept { return unsigned_item(kInput, flags); }
bool ItemWriter::feature(const std::uint32_t flags) noexcept {
    return unsigned_item(kFeature, flags);
}
bool ItemWriter::collection(const std::uint32_t type) noexcept {
    return unsigned_item(kCollection, type);
}
bool ItemWriter::end_collection() noexcept { return emit(EncodedItem{kEndCollection, 0U, 0U}); }
bool ItemWriter::good() const noexcept { return good_; }
std::size_t ItemWriter::size() const noexcept { return position_; }

} // namespace aoa::hid
