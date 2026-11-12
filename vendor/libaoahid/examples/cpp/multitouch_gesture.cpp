// SPDX-License-Identifier: MIT
// Copyright (c) 2026 libaoahid contributors
// The first phone found presents a two-finger touchscreen and spreads two fingers apart.
#include <aoahid.hpp>

#include <chrono>
#include <cstdio>
#include <thread>

namespace {
constexpr std::uint32_t k_control_timeout_ms = 500;
constexpr std::uint32_t k_drain_timeout_ms = 1000;
} // namespace

int main() {
    auto context_config = aoahid::context_options();
    context_config.event_mode = AOAHID_EVENT_CALLER_POLL;
    aoahid_context* context = nullptr;
    if (aoahid::context_create(&context_config, &context) != AOAHID_OK) {
        std::fprintf(stderr, "context_create: %s\n", aoahid::last_error()->reason);
        return 1;
    }

    aoahid_discovery* found_raw = nullptr;
    if (aoahid::discover(context, k_control_timeout_ms, &found_raw) != AOAHID_OK) {
        std::fprintf(stderr, "discover: %s\n", aoahid::last_error()->reason);
        (void)aoahid::context_destroy(context, k_control_timeout_ms, k_drain_timeout_ms);
        return 1;
    }
    const aoahid::DiscoveryPtr found{found_raw};
    const aoahid_device_info* phone =
        aoahid::discovery_count(found.get()) == 0 ? nullptr : aoahid::discovery_get(found.get(), 0);
    // Open sends no AOA request, so the version check is the caller's.
    if (phone == nullptr || phone->protocol_version < 2) {
        std::fprintf(stderr, "No phone with AOA 2 found.\n");
        (void)aoahid::context_destroy(context, k_control_timeout_ms, k_drain_timeout_ms);
        return 1;
    }

    auto device_config = aoahid::device_options();
    device_config.descriptor_fragment_bytes = 64; // AOA fixes no fragment size.
    device_config.transfer_pool_slots = 8;
    device_config.maximum_report_bytes = 4088;
    device_config.host_control_buffer_policy_bytes = 4088;
    device_config.validate_reports = 1;
    aoahid_device* device = nullptr;
    if (const aoahid_result opened = aoahid::device_open(context, phone, &device_config, &device);
        opened != AOAHID_OK) {
        std::fprintf(stderr, "device_open: %s\n", aoahid::result_name(opened));
        (void)aoahid::context_destroy(context, k_control_timeout_ms, k_drain_timeout_ms);
        return 1;
    }

    // The coordinates of a 1080 x 2400 screen, two contacts.
    auto touch = aoahid::touchscreen_options();
    touch.maximum_contacts = 2;
    touch.contacts_per_report = 2;
    touch.x = aoahid_integer_field_make(0, 1080, 16);
    touch.y = aoahid_integer_field_make(0, 2400, 16);
    touch.contact_count = aoahid_integer_field_make(0, 2, 2);
    aoahid_spec* spec_raw = nullptr;
    const aoahid_result created = aoahid::spec_create_touchscreen(&touch, &spec_raw);
    const aoahid::SpecPtr spec{spec_raw};

    const auto node_config = aoahid::node_options();
    aoahid_node* node = nullptr;
    if (created != AOAHID_OK || aoahid::node_open(device, spec.get(), &node_config,
                                                  k_control_timeout_ms, &node) != AOAHID_OK) {
        std::fprintf(stderr, "setup failed: %s\n", aoahid::last_error()->reason);
        (void)aoahid::device_close(device, k_control_timeout_ms, k_drain_timeout_ms);
        (void)aoahid::context_destroy(context, k_control_timeout_ms, k_drain_timeout_ms);
        return 1;
    }

    for (int step = 0; step <= 20; ++step) {
        const std::int32_t spread = step * 15;
        (void)aoahid::touchscreen_contact(node, 0, 1, 540 - spread, 1200, nullptr);
        (void)aoahid::touchscreen_contact(node, 1, 1, 540 + spread, 1200, nullptr);
        (void)aoahid::node_submit_blocking(node, 100);
        std::this_thread::sleep_for(std::chrono::milliseconds(8));
    }
    (void)aoahid::touchscreen_contact(node, 0, 0, 240, 1200, nullptr);
    (void)aoahid::touchscreen_contact(node, 1, 0, 840, 1200, nullptr);
    (void)aoahid::node_submit_blocking(node, 100);

    // Closing the Device closes the Node too.
    const aoahid_result closed =
        aoahid::device_close(device, k_control_timeout_ms, k_drain_timeout_ms);
    const aoahid_result destroyed =
        aoahid::context_destroy(context, k_control_timeout_ms, k_drain_timeout_ms);
    return closed == AOAHID_OK && destroyed == AOAHID_OK ? 0 : 1;
}
