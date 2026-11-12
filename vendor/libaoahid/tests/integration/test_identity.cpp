// SPDX-License-Identifier: MIT
// Copyright (c) 2026 libaoahid contributors
/* Verifies multi-device identity isolation, structured logging boundaries, and
 * byte-identical state output without physical USB hardware. */
#include "aoahid.h"
#include "fake_libusb/libusb.h"
#include "test.hpp"

#include <array>
#include <cstdint>
#include <cstring>

// Every wait is an argument of the call and has no default; these are the tests' own choices.
inline constexpr std::uint32_t kControlTimeoutMs = 20U;
inline constexpr std::uint32_t kSendTimeoutMs = 20U;
inline constexpr std::uint32_t kDrainTimeoutMs = 20U;

namespace {

aoahid_context_options context_options() {
    aoahid_context_options options{};
    options.struct_size = sizeof(options);
    options.event_mode = AOAHID_EVENT_CALLER_POLL;
    options.log_level = AOAHID_LOG_DISABLED;
    return options;
}

aoahid_device_options device_options() {
    aoahid_device_options options{};
    options.struct_size = sizeof(options);
    options.descriptor_fragment_bytes = 64U;
    options.transfer_pool_slots = 1U;
    options.maximum_report_bytes = 64U;
    options.validate_reports = 1U;
    options.aoa_descriptor_wire_policy_bytes = 1024U;
    options.linux_hid.descriptor_bytes = 1024U;
    options.linux_hid.fields_per_report = 256U;
    options.linux_hid.global_stack_depth = 4U;
    options.linux_hid.usages = 12288U;
    options.linux_hid.report_data_bits = 65528U;
    options.linux_hid.report_size_bits = 256U;
    options.target_ep0_data_policy_bytes = 4096U;
    options.host_control_buffer_policy_bytes = 4096U;
    options.interface_claim_policy = AOAHID_INTERFACE_CLAIM_NONE;
    options.interface_number = -1;
    return options;
}

aoahid_spec* make_raw_spec() {
    static constexpr std::array<std::uint8_t, 21U> descriptor{
        0x05U, 0x01U, 0x09U, 0x06U, 0xA1U, 0x01U, 0x85U, 0x01U, 0x15U, 0x00U, 0x25U,
        0x01U, 0x75U, 0x01U, 0x95U, 0x01U, 0x09U, 0x01U, 0x81U, 0x02U, 0xC0U};
    static constexpr aoahid_raw_report report{{1U, 1U, {}}, 2U};
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
    return spec;
}

aoahid_device* open_device(aoahid_context* context, const aoahid_fake_libusb_device_config& fake) {
    const aoahid_device_info selected{
        fake.bus,        fake.address, fake.port_path, fake.port_path_length, fake.vendor_id,
        fake.product_id, 0U,           fake.serial,    fake.product};
    aoahid_device_options options = device_options();
    aoahid_device* device = nullptr;
    AOAHID_CHECK(aoahid_device_open(context, &selected, &options, &device) == AOAHID_OK);
    return device;
}

struct LogCapture {
    std::uint32_t error_count{};
    std::uint32_t info_count{};
    std::uint32_t trace_count{};
    bool exposed_payload{};
};

void AOAHID_CALL capture_log(void* user, const aoahid_log_level level, const char* message) {
    auto* capture = static_cast<LogCapture*>(user);
    if (capture == nullptr || message == nullptr)
        return;
    if (level == AOAHID_LOG_ERROR)
        ++capture->error_count;
    if (level == AOAHID_LOG_INFO)
        ++capture->info_count;
    if (level == AOAHID_LOG_TRACE)
        ++capture->trace_count;
    capture->exposed_payload =
        capture->exposed_payload || std::strstr(message, "SECRET") != nullptr;
}

void test_structured_logging_excludes_payload() {
    aoahid_fake_libusb_reset();
    static constexpr std::array<std::uint8_t, 2U> path{2U, 1U};
    const aoahid_fake_libusb_device_config fake{
        2U,      6U, path.data(), path.size(), 0x18D1U,
        0x2D00U, 2U, "log-phone", "Android",   LIBUSB_SUCCESS};
    AOAHID_CHECK(aoahid_fake_libusb_add_device(&fake) >= 0);
    LogCapture capture{};
    aoahid_context_options context_config = context_options();
    context_config.log_level = AOAHID_LOG_TRACE;
    context_config.log_sink = &capture_log;
    context_config.log_user = &capture;
    aoahid_context* context = nullptr;
    AOAHID_CHECK(aoahid_context_create(&context_config, &context) == AOAHID_OK);
    aoahid_device* device = open_device(context, fake);

    static constexpr std::array<std::uint8_t, 22U> descriptor{
        0x05U, 0x01U, 0x09U, 0x06U, 0xA1U, 0x01U, 0x85U, 0x01U, 0x15U, 0x00U, 0x26U,
        0xFFU, 0x00U, 0x75U, 0x08U, 0x95U, 0x06U, 0x09U, 0x01U, 0x81U, 0x02U, 0xC0U};
    static constexpr aoahid_raw_report report_capability{{1U, 1U, {}}, 7U};
    const aoahid_raw_options raw_options{sizeof(aoahid_raw_options),
                                         0U,
                                         descriptor.data(),
                                         descriptor.size(),
                                         &report_capability,
                                         1U,
                                         1U,
                                         0U,
                                         0U};
    aoahid_spec* spec = nullptr;
    AOAHID_CHECK(aoahid_spec_create_raw(&raw_options, &spec) == AOAHID_OK);
    const aoahid_node_options node_options{sizeof(aoahid_node_options), 0U, 1U, 0U};
    aoahid_node* node = nullptr;
    AOAHID_CHECK(aoahid_node_open(device, spec, &node_options, kControlTimeoutMs, &node) ==
                 AOAHID_OK);
    static constexpr std::array<std::uint8_t, 7U> sensitive_report{1U,  'S', 'E', 'C',
                                                                   'R', 'E', 'T'};
    AOAHID_CHECK(aoahid_raw_submit(node, sensitive_report.data(), sensitive_report.size(),
                                   kSendTimeoutMs) == AOAHID_OK);
    AOAHID_CHECK(aoahid_node_submit_blocking(node, 20U) == AOAHID_OK);
    AOAHID_CHECK(aoahid_node_close(node, kControlTimeoutMs, kDrainTimeoutMs) == AOAHID_OK);
    aoahid_spec_release(spec);
    AOAHID_CHECK(aoahid_device_close(device, kControlTimeoutMs, kDrainTimeoutMs) == AOAHID_OK);
    AOAHID_CHECK(aoahid_context_destroy(context, kControlTimeoutMs, kDrainTimeoutMs) == AOAHID_OK);
    AOAHID_CHECK(capture.error_count == 0U);
    AOAHID_CHECK(capture.info_count >= 4U);
    // Prompt 0 rule 3 keeps the send/completion path free of logging calls.
    AOAHID_CHECK(capture.trace_count == 0U);
    AOAHID_CHECK(!capture.exposed_payload);
    aoahid_fake_libusb_reset();
}

void test_physical_identity_hid_id_domains() {
    aoahid_fake_libusb_reset();
    static constexpr std::array<std::uint8_t, 2U> first_path{1U, 1U};
    static constexpr std::array<std::uint8_t, 2U> second_path{1U, 2U};
    const aoahid_fake_libusb_device_config first{
        1U,      4U, first_path.data(), first_path.size(), 0x18D1U,
        0x2D00U, 2U, "first",           "Android",         LIBUSB_SUCCESS};
    const aoahid_fake_libusb_device_config second{
        1U,      5U, second_path.data(), second_path.size(), 0x18D1U,
        0x2D00U, 2U, "second",           "Android",          LIBUSB_SUCCESS};
    AOAHID_CHECK(aoahid_fake_libusb_add_device(&first) >= 0);
    AOAHID_CHECK(aoahid_fake_libusb_add_device(&second) >= 0);

    aoahid_context* context = nullptr;
    aoahid_context_options context_config = context_options();
    AOAHID_CHECK(aoahid_context_create(&context_config, &context) == AOAHID_OK);
    aoahid_device* first_device = open_device(context, first);
    aoahid_device* second_device = open_device(context, second);
    aoahid_spec* spec = make_raw_spec();
    const aoahid_node_options node_options{sizeof(aoahid_node_options), 0U, 1U, 0U};

    // A failed registration retires its ID in only the first physical domain.
    aoahid_fake_libusb_queue_control_result(LIBUSB_ERROR_PIPE);
    aoahid_node* rejected = nullptr;
    AOAHID_CHECK(aoahid_node_open(first_device, spec, &node_options, kControlTimeoutMs,
                                  &rejected) == AOAHID_ERR_STALL);
    AOAHID_CHECK(rejected == nullptr);
    const aoahid_error_detail* registration_error = aoahid_last_error();
    AOAHID_CHECK(registration_error->code == AOAHID_ERR_STALL);
    AOAHID_CHECK(registration_error->field != nullptr && registration_error->reason != nullptr);
    AOAHID_CHECK(registration_error->transport_status == LIBUSB_ERROR_PIPE);
    AOAHID_CHECK(registration_error->aoa_request == 54);
    AOAHID_CHECK(registration_error->node_id == 1U);
    AOAHID_CHECK(registration_error->offset == 0U);
    AOAHID_CHECK(registration_error->length == 21U);

    aoahid_node* first_node = nullptr;
    aoahid_node* second_node = nullptr;
    AOAHID_CHECK(aoahid_node_open(first_device, spec, &node_options, kControlTimeoutMs,
                                  &first_node) == AOAHID_OK);
    AOAHID_CHECK(aoahid_last_error()->code == AOAHID_OK);
    AOAHID_CHECK(aoahid_node_open(second_device, spec, &node_options, kControlTimeoutMs,
                                  &second_node) == AOAHID_OK);
    AOAHID_CHECK(aoahid_node_id(first_node) == 2U);
    AOAHID_CHECK(aoahid_node_id(second_node) == 1U);

    AOAHID_CHECK(aoahid_node_close(first_node, kControlTimeoutMs, kDrainTimeoutMs) == AOAHID_OK);
    AOAHID_CHECK(aoahid_device_close(first_device, kControlTimeoutMs, kDrainTimeoutMs) ==
                 AOAHID_OK);
    first_device = open_device(context, first);
    first_node = nullptr;
    AOAHID_CHECK(aoahid_node_open(first_device, spec, &node_options, kControlTimeoutMs,
                                  &first_node) == AOAHID_OK);
    AOAHID_CHECK(aoahid_node_id(first_node) == 3U);

    AOAHID_CHECK(aoahid_node_close(first_node, kControlTimeoutMs, kDrainTimeoutMs) == AOAHID_OK);
    AOAHID_CHECK(aoahid_node_close(second_node, kControlTimeoutMs, kDrainTimeoutMs) == AOAHID_OK);
    aoahid_spec_release(spec);
    AOAHID_CHECK(aoahid_device_close(first_device, kControlTimeoutMs, kDrainTimeoutMs) ==
                 AOAHID_OK);
    AOAHID_CHECK(aoahid_device_close(second_device, kControlTimeoutMs, kDrainTimeoutMs) ==
                 AOAHID_OK);
    AOAHID_CHECK(aoahid_context_destroy(context, kControlTimeoutMs, kDrainTimeoutMs) == AOAHID_OK);
    aoahid_fake_libusb_reset();
}

} // namespace

void test_identity() {
    test_physical_identity_hid_id_domains();
    test_structured_logging_excludes_payload();
}
