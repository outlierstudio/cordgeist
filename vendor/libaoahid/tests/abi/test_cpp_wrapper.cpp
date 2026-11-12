// SPDX-License-Identifier: MIT
// Copyright (c) 2026 libaoahid contributors
/* Checks that the C++ header forwards to the C ABI under namespace aoahid. */
#include "aoahid.hpp"

#include <cstdint>

// Every wait is an argument of the call and has no default; these are the tests' own choices.
inline constexpr std::uint32_t kControlTimeoutMs = 20U;
inline constexpr std::uint32_t kSendTimeoutMs = 20U;
inline constexpr std::uint32_t kDrainTimeoutMs = 20U;

int main() {
    if (aoahid::version() != aoahid_version() || aoahid::result_name(AOAHID_OK) == nullptr ||
        aoahid::keyboard_key(nullptr, 0U, 0U) != AOAHID_ERR_PARAM ||
        aoahid::mouse_move(nullptr, 0, 0) != AOAHID_ERR_PARAM ||
        aoahid::node_submit(nullptr, kSendTimeoutMs) != AOAHID_ERR_PARAM ||
        aoahid::context_create(nullptr, nullptr) != AOAHID_ERR_PARAM ||
        aoahid::last_error() != aoahid_last_error()) {
        return 1;
    }

    const aoahid_device_options device = aoahid::device_options();
    if (device.struct_size != sizeof(device) || device.interface_number != -1 ||
        aoahid::channel_options().read_mode != AOAHID_CHANNEL_READ_REQUEST) {
        return 1;
    }

    const aoahid_keyboard_options keyboard = aoahid::keyboard_options();
    aoahid_spec* raw_spec = nullptr;
    if (aoahid::spec_create_keyboard(&keyboard, &raw_spec) != AOAHID_OK) {
        return 1;
    }
    const aoahid::SpecPtr spec{raw_spec};
    aoahid_capability_manifest manifest = aoahid::capability_manifest();
    if (aoahid::spec_manifest(spec.get(), &manifest) != AOAHID_OK ||
        manifest.profile_kind != AOAHID_PROFILE_KEYBOARD) {
        return 1;
    }
    const aoahid::DiscoveryPtr empty{};
    return 0;
}
