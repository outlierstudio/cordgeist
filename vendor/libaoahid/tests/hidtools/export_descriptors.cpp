// SPDX-License-Identifier: MIT
// Copyright (c) 2026 libaoahid contributors
/* Exports representative generated descriptors for independent hid-tools
 * parsing; it makes no hardware-support claim. */
#include "aoahid.h"
#include "hid/usages.hpp"

#include <array>
#include <cstdint>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <limits>
#include <map>
#include <string>
#include <system_error>
#include <utility>
#include <vector>

namespace {

std::filesystem::path golden_directory;

struct manifest_record {
    std::string key;
    std::int32_t android_status{};
    std::uint32_t input_supported{};
    std::uint32_t output_supported{};
    std::uint32_t feature_transport_supported{};
    std::size_t descriptor_length{};
    std::string reports;
};

std::vector<manifest_record> manifest_records;

struct hid_globals {
    std::uint32_t report_size{};
    std::uint32_t report_count{};
    std::uint8_t report_id{};
    bool has_report_id{};
};

bool descriptor_layout_matches_manifest(const char* name, const aoahid_spec* spec,
                                        const std::uint8_t* bytes, const std::size_t length,
                                        aoahid_capability_manifest* const out_manifest) {
    std::map<std::pair<bool, std::uint8_t>, std::uint64_t> input_bits;
    std::vector<hid_globals> stack;
    hid_globals globals{};
    std::size_t offset = 0U;
    while (offset < length) {
        const std::uint8_t prefix = bytes[offset];
        if (prefix == 0xFEU) {
            if (offset + 3U > length)
                return false;
            const std::size_t data_size = bytes[offset + 1U];
            if (data_size > length - offset - 3U)
                return false;
            offset += data_size + 3U;
            continue;
        }
        const std::uint8_t encoded_size = prefix & 0x03U;
        const std::size_t item_size = encoded_size == 3U ? 4U : encoded_size;
        if (item_size > length - offset - 1U)
            return false;
        std::uint32_t value = 0U;
        for (std::size_t byte_index = 0U; byte_index < item_size; ++byte_index) {
            value |= static_cast<std::uint32_t>(bytes[offset + 1U + byte_index])
                     << (byte_index * 8U);
        }
        const std::uint8_t item_type = (prefix >> 2U) & 0x03U;
        const std::uint8_t item_tag = prefix >> 4U;
        if (item_type == 1U) {
            if (item_tag == 7U)
                globals.report_size = value;
            if (item_tag == 8U) {
                globals.report_id = static_cast<std::uint8_t>(value);
                globals.has_report_id = true;
            }
            if (item_tag == 9U)
                globals.report_count = value;
            if (item_tag == 10U)
                stack.push_back(globals);
            if (item_tag == 11U) {
                if (stack.empty())
                    return false;
                globals = stack.back();
                stack.pop_back();
            }
        } else if (item_type == 0U && item_tag == 8U) {
            const std::uint64_t bits = static_cast<std::uint64_t>(globals.report_size) *
                                       static_cast<std::uint64_t>(globals.report_count);
            auto& total = input_bits[{globals.has_report_id, globals.report_id}];
            if (bits > std::numeric_limits<std::uint64_t>::max() - total)
                return false;
            total += bits;
        }
        offset += item_size + 1U;
    }
    if (!stack.empty() || input_bits.empty())
        return false;

    aoahid_capability_manifest manifest{};
    manifest.struct_size = static_cast<std::uint32_t>(sizeof(manifest));
    if (aoahid_spec_manifest(spec, &manifest) != AOAHID_OK || manifest.input_supported != 1U ||
        manifest.reports == nullptr || manifest.report_count != input_bits.size() ||
        manifest.descriptor_length != length) {
        std::fprintf(stderr, "%s manifest shape disagrees with descriptor\n", name);
        return false;
    }
    auto remaining = input_bits;
    for (std::size_t report_index = 0U; report_index < manifest.report_count; ++report_index) {
        const aoahid_report_capability& report = manifest.reports[report_index];
        const auto key = std::make_pair(report.report_id.enabled != 0U, report.report_id.value);
        const auto found = remaining.find(key);
        if (found == remaining.end())
            return false;
        const std::uint64_t payload_bytes = (found->second + 7U) / 8U;
        const std::uint64_t wire_bytes = payload_bytes + (key.first ? 1U : 0U);
        if (wire_bytes != report.wire_length)
            return false;
        remaining.erase(found);
    }
    if (!remaining.empty())
        return false;
    if (out_manifest != nullptr)
        *out_manifest = manifest;
    return true;
}

bool emit_descriptor(const std::filesystem::path& directory, const char* name,
                     const aoahid_result creation_result, aoahid_spec*& spec) {
    if (creation_result != AOAHID_OK || spec == nullptr) {
        const aoahid_error_detail* detail = aoahid_last_error();
        std::fprintf(stderr, "%s creation failed: result=%s field=%s reason=%s\n", name,
                     aoahid_result_name(creation_result),
                     detail != nullptr && detail->field != nullptr ? detail->field : "",
                     detail != nullptr && detail->reason != nullptr ? detail->reason : "");
        aoahid_spec_release(spec);
        return false;
    }

    const std::uint8_t* bytes = nullptr;
    std::size_t length = 0U;
    const aoahid_result descriptor_result = aoahid_spec_descriptor(spec, &bytes, &length);
    if (descriptor_result != AOAHID_OK || bytes == nullptr || length == 0U) {
        std::fprintf(stderr, "%s returned no descriptor\n", name);
        aoahid_spec_release(spec);
        return false;
    }
    aoahid_capability_manifest manifest{};
    if (!descriptor_layout_matches_manifest(name, spec, bytes, length, &manifest)) {
        std::fprintf(stderr, "%s report layout property failed\n", name);
        aoahid_spec_release(spec);
        return false;
    }

    std::string reports;
    for (std::size_t index = 0U; index < manifest.report_count; ++index) {
        if (!reports.empty())
            reports.push_back(',');
        const aoahid_report_capability& report = manifest.reports[index];
        reports += report.report_id.enabled != 0U ? "id:" : "idless:";
        reports += std::to_string(report.report_id.value);
        reports.push_back(':');
        reports += std::to_string(report.wire_length);
    }
    manifest_records.push_back(manifest_record{
        name, static_cast<std::int32_t>(manifest.android_status), manifest.input_supported,
        manifest.output_supported, manifest.feature_transport_supported, manifest.descriptor_length,
        std::move(reports)});

    const std::filesystem::path output = directory / (std::string{name} + ".bin");
    std::ofstream stream(output, std::ios::binary | std::ios::trunc);
    stream.write(reinterpret_cast<const char*>(bytes), static_cast<std::streamsize>(length));
    stream.close();
    if (!stream) {
        std::fprintf(stderr, "failed to write %s\n", output.string().c_str());
        aoahid_spec_release(spec);
        return false;
    }
    if (!golden_directory.empty()) {
        constexpr char digits[] = "0123456789abcdef";
        std::string actual;
        actual.reserve((length * 2U) + 1U);
        for (std::size_t index = 0U; index < length; ++index) {
            actual.push_back(digits[bytes[index] >> 4U]);
            actual.push_back(digits[bytes[index] & 0x0FU]);
        }
        actual.push_back('\n');
        const std::filesystem::path expected_path = golden_directory / (std::string{name} + ".hex");
        std::ifstream expected_stream(expected_path, std::ios::binary);
        const std::string expected{std::istreambuf_iterator<char>{expected_stream},
                                   std::istreambuf_iterator<char>{}};
        if (!expected_stream || expected != actual) {
            std::fprintf(stderr, "golden descriptor mismatch or missing file: %s\n",
                         expected_path.string().c_str());
            aoahid_spec_release(spec);
            return false;
        }
    }
    aoahid_spec_release(spec);
    return true;
}

aoahid_keyboard_options keyboard_options() noexcept {
    aoahid_keyboard_options options{};
    options.struct_size = static_cast<std::uint32_t>(sizeof(options));
    options.usage_minimum = 0x04U;
    options.usage_maximum = 0x65U;
    return options;
}

aoahid_touchscreen_options fixed_touchscreen_options() noexcept {
    aoahid_touchscreen_options options{};
    options.struct_size = static_cast<std::uint32_t>(sizeof(options));
    options.maximum_contacts = 3U;
    options.contacts_per_report = 3U;
    options.contact_id = {0, 15, 4U, {}};
    options.x = {0, 32767, 16U, {}};
    options.y = {0, 32767, 16U, {}};
    options.contact_count = {0, 3, 2U, {}};
    options.enable_pressure = 1U;
    options.pressure = {0, 255, 8U, {}};
    options.enable_width = 1U;
    options.width = {0, 255, 8U, {}};
    options.enable_height = 1U;
    options.height = {0, 255, 8U, {}};
    options.enable_azimuth = 1U;
    options.azimuth = {0, 36000, 16U, {1U, 0, 360, 0, 0x14U}};
    options.enable_scan_time = 1U;
    options.scan_time = {0, 65535, 16U, {}};
    options.scan_time_unit_100us = 1U;
    options.enable_contact_count_maximum_feature_declaration = 0U;
    options.enable_multi_packet_frames = 0U;
    return options;
}

} // namespace

int main(const int argc, char** argv) {
    if ((argc < 2 || argc > 4) || argv[1] == nullptr || argv[1][0] == '\0') {
        std::fputs("usage: aoahid_export_descriptors OUTPUT_DIRECTORY [GOLDEN_DIRECTORY] "
                   "[MANIFEST_CATALOG]\n",
                   stderr);
        return 2;
    }
    if (argc >= 3) {
        if (argv[2] == nullptr || argv[2][0] == '\0')
            return 2;
        if (std::string{argv[2]} != "-")
            golden_directory = std::filesystem::path{argv[2]};
    }
    const std::filesystem::path output_directory{argv[1]};
    std::error_code directory_error;
    std::filesystem::create_directories(output_directory, directory_error);
    if (directory_error) {
        std::fprintf(stderr, "failed to create descriptor directory: %s\n",
                     directory_error.message().c_str());
        return 1;
    }

    bool ok = true;
    aoahid_spec* spec = nullptr;

    aoahid_keyboard_options keyboard = keyboard_options();
    keyboard.report_id = {1U, 7U, {0U, 0U, 0U}};
    ok = emit_descriptor(output_directory, "keyboard",
                         aoahid_spec_create_keyboard(&keyboard, &spec), spec) &&
         ok;
    spec = nullptr;

    keyboard.enable_battery = 1U;
    keyboard.battery.report_id = {1U, 8U, {0U, 0U, 0U}};
    keyboard.battery.strength = {1, 100, 8U, {}};
    ok = emit_descriptor(output_directory, "keyboard-battery",
                         aoahid_spec_create_keyboard(&keyboard, &spec), spec) &&
         ok;
    spec = nullptr;

    aoahid_mouse_options mouse{};
    mouse.struct_size = static_cast<std::uint32_t>(sizeof(mouse));
    mouse.button_count = 5U;
    mouse.x = {-32767, 32767, 16U, {}};
    mouse.y = {-32767, 32767, 16U, {}};
    mouse.enable_wheel = 1U;
    mouse.wheel = {-127, 127, 8U, {}};
    mouse.enable_pan = 1U;
    mouse.pan = {-127, 127, 8U, {}};
    ok = emit_descriptor(output_directory, "mouse-wheel-pan",
                         aoahid_spec_create_mouse(&mouse, &spec), spec) &&
         ok;
    spec = nullptr;

    // These audited vectors are shared with tests/unit/test_profiles.cpp.  They
    // exercise each supported one-control Main-item semantic without claiming
    // an operating-system mapping.
    static constexpr std::array<std::uint16_t, 6U> consumer_usages{0x00CDU, 0x00E9U, 0x00E2U,
                                                                   0x0201U, 0x00B0U, 0x00BBU};
    static constexpr std::array<aoahid_usage_semantic, 6U> consumer_semantics{
        AOAHID_USAGE_ONE_SHOT,        AOAHID_USAGE_RETRIGGER,     AOAHID_USAGE_ON_OFF_MAINTAINED,
        AOAHID_USAGE_SELECTOR_BITMAP, AOAHID_USAGE_ON_OFF_TOGGLE, AOAHID_USAGE_MOMENTARY};
    static constexpr std::array<const char*, 6U> consumer_linux{
        "TEST_EVENT_CODE", "TEST_EVENT_CODE", "TEST_EVENT_CODE",
        "TEST_EVENT_CODE", "TEST_EVENT_CODE", "TEST_EVENT_CODE"};
    aoahid_toggle_options controls{};
    controls.struct_size = static_cast<std::uint32_t>(sizeof(controls));
    controls.application_page = aoa::hid::usage::page_consumer;
    controls.application_usage = aoa::hid::usage::consumer_control;
    controls.field_page = aoa::hid::usage::page_consumer;
    controls.allowed_usages = consumer_usages.data();
    controls.allowed_usage_count = consumer_usages.size();
    controls.usage_semantics = consumer_semantics.data();
    controls.expected_linux_event_types = consumer_linux.data();
    controls.expected_linux_codes = consumer_linux.data();
    ok = emit_descriptor(output_directory, "consumer-controls",
                         aoahid_spec_create_toggle(&controls, &spec), spec) &&
         ok;
    spec = nullptr;

    static constexpr std::array<std::uint16_t, 4U> system_usages{0x81U, 0x8AU, 0x97U, 0x98U};
    static constexpr std::array<aoahid_usage_semantic, 4U> system_semantics{
        AOAHID_USAGE_ONE_SHOT, AOAHID_USAGE_RETRIGGER, AOAHID_USAGE_MOMENTARY,
        AOAHID_USAGE_ON_OFF_TOGGLE};
    static constexpr std::array<const char*, 4U> system_linux{"TEST_EVENT_CODE", "TEST_EVENT_CODE",
                                                              "TEST_EVENT_CODE", "TEST_EVENT_CODE"};
    controls.application_page = aoa::hid::usage::page_generic_desktop;
    controls.application_usage = aoa::hid::usage::system_control;
    controls.field_page = aoa::hid::usage::page_generic_desktop;
    controls.allowed_usages = system_usages.data();
    controls.allowed_usage_count = system_usages.size();
    controls.usage_semantics = system_semantics.data();
    controls.expected_linux_event_types = system_linux.data();
    controls.expected_linux_codes = system_linux.data();
    ok = emit_descriptor(output_directory, "system-controls",
                         aoahid_spec_create_toggle(&controls, &spec), spec) &&
         ok;
    spec = nullptr;

    static constexpr std::array<std::uint16_t, 2U> camera_usages{0x20U, 0x21U};
    static constexpr std::array<aoahid_usage_semantic, 2U> camera_semantics{AOAHID_USAGE_ONE_SHOT,
                                                                            AOAHID_USAGE_ONE_SHOT};
    static constexpr std::array<const char*, 2U> camera_linux{"TEST_EVENT_CODE", "TEST_EVENT_CODE"};
    controls.application_page = aoa::hid::usage::page_consumer;
    controls.application_usage = aoa::hid::usage::consumer_control;
    controls.field_page = aoa::hid::usage::page_camera_control;
    controls.allowed_usages = camera_usages.data();
    controls.allowed_usage_count = camera_usages.size();
    controls.usage_semantics = camera_semantics.data();
    controls.expected_linux_event_types = camera_linux.data();
    controls.expected_linux_codes = camera_linux.data();
    ok = emit_descriptor(output_directory, "camera-keys",
                         aoahid_spec_create_toggle(&controls, &spec), spec) &&
         ok;
    spec = nullptr;

    static constexpr std::array<std::uint16_t, 4U> telephony_usages{0x20U, 0x21U, 0x24U, 0xB0U};
    static constexpr std::array<aoahid_usage_semantic, 4U> telephony_semantics{
        AOAHID_USAGE_ON_OFF_MAINTAINED, AOAHID_USAGE_MOMENTARY, AOAHID_USAGE_ONE_SHOT,
        AOAHID_USAGE_SELECTOR_BITMAP};
    static constexpr std::array<const char*, 4U> telephony_linux{
        "TEST_EVENT_CODE", "TEST_EVENT_CODE", "TEST_EVENT_CODE", "TEST_EVENT_CODE"};
    controls.application_page = aoa::hid::usage::page_telephony;
    controls.application_usage = aoa::hid::usage::phone;
    controls.field_page = aoa::hid::usage::page_telephony;
    controls.allowed_usages = telephony_usages.data();
    controls.allowed_usage_count = telephony_usages.size();
    controls.usage_semantics = telephony_semantics.data();
    controls.expected_linux_event_types = telephony_linux.data();
    controls.expected_linux_codes = telephony_linux.data();
    ok = emit_descriptor(output_directory, "telephony-keys",
                         aoahid_spec_create_toggle(&controls, &spec), spec) &&
         ok;
    spec = nullptr;

    static constexpr std::array<aoahid_gamepad_axis_option, 2U> axes{
        aoahid_gamepad_axis_option{AOAHID_AXIS_X,
                                   aoa::hid::usage::page_generic_desktop,
                                   aoa::hid::usage::x,
                                   {-32767, 32767, 16U, {}},
                                   0,
                                   "ABS_X",
                                   "AXIS_X"},
        aoahid_gamepad_axis_option{AOAHID_AXIS_Y,
                                   aoa::hid::usage::page_generic_desktop,
                                   aoa::hid::usage::y,
                                   {-32767, 32767, 16U, {}},
                                   0,
                                   "ABS_Y",
                                   "AXIS_Y"}};
    aoahid_gamepad_options controller{};
    controller.struct_size = static_cast<std::uint32_t>(sizeof(controller));
    controller.axes = axes.data();
    controller.axis_count = axes.size();
    controller.button_count = 12U;
    controller.button_usage_minimum = 1U;
    controller.dpad_representation = AOAHID_DPAD_HAT;
    controller.hat_logical_minimum = 0;
    controller.hat_logical_maximum = 7;
    controller.hat_bit_width = 4U;
    ok = emit_descriptor(output_directory, "gamepad",
                         aoahid_spec_create_gamepad(&controller, &spec), spec) &&
         ok;
    spec = nullptr;

    aoahid_touchscreen_options touch = fixed_touchscreen_options();
    ok = emit_descriptor(output_directory, "touchscreen-fixed-mt",
                         aoahid_spec_create_touchscreen(&touch, &spec), spec) &&
         ok;
    spec = nullptr;

    aoahid_touchpad_options touchpad{};
    touchpad.struct_size = static_cast<std::uint32_t>(sizeof(touchpad));
    touchpad.report_id = touch.report_id;
    touchpad.maximum_contacts = touch.maximum_contacts;
    touchpad.contacts_per_report = touch.contacts_per_report;
    touchpad.contact_id = touch.contact_id;
    touchpad.x = touch.x;
    touchpad.y = touch.y;
    touchpad.contact_count = touch.contact_count;
    touchpad.enable_pressure = touch.enable_pressure;
    touchpad.pressure = touch.pressure;
    touchpad.enable_width = touch.enable_width;
    touchpad.width = touch.width;
    touchpad.enable_height = touch.enable_height;
    touchpad.height = touch.height;
    touchpad.enable_azimuth = touch.enable_azimuth;
    touchpad.azimuth = touch.azimuth;
    touchpad.enable_scan_time = touch.enable_scan_time;
    touchpad.scan_time = touch.scan_time;
    touchpad.scan_time_unit_100us = touch.scan_time_unit_100us;
    touchpad.enable_contact_count_maximum_feature_declaration =
        touch.enable_contact_count_maximum_feature_declaration;
    touchpad.enable_multi_packet_frames = touch.enable_multi_packet_frames;
    touchpad.button_count = 2U;
    ok = emit_descriptor(output_directory, "touchpad",
                         aoahid_spec_create_touchpad(&touchpad, &spec), spec) &&
         ok;
    spec = nullptr;

    static constexpr std::array<std::uint16_t, 2U> barrel_usages{
        aoa::hid::usage::barrel_switch, aoa::hid::usage::secondary_barrel_switch};
    aoahid_pen_options pen{};
    pen.struct_size = static_cast<std::uint32_t>(sizeof(pen));
    pen.x = {0, 32767, 16U, {}};
    pen.y = {0, 32767, 16U, {}};
    pen.enable_pressure = 1U;
    pen.pressure = {0, 4095, 12U, {}};
    pen.enable_tilt = 1U;
    pen.tilt_x = {-90, 90, 8U, {}};
    pen.tilt_y = {-90, 90, 8U, {}};
    pen.enable_twist_target_specific = 0U;
    pen.barrel_usages = barrel_usages.data();
    pen.barrel_usage_count = barrel_usages.size();
    pen.enable_eraser = 1U;
    pen.enable_hover = 1U;
    ok = emit_descriptor(output_directory, "pen", aoahid_spec_create_pen(&pen, &spec), spec) && ok;
    spec = nullptr;
    aoahid_digitizer_options digitizer{};
    digitizer.struct_size = static_cast<std::uint32_t>(sizeof(digitizer));
    digitizer.x = pen.x;
    digitizer.y = pen.y;
    digitizer.enable_pressure = pen.enable_pressure;
    digitizer.pressure = pen.pressure;
    digitizer.enable_tilt = pen.enable_tilt;
    digitizer.tilt_x = pen.tilt_x;
    digitizer.tilt_y = pen.tilt_y;
    digitizer.enable_twist_target_specific = pen.enable_twist_target_specific;
    digitizer.barrel_usages = pen.barrel_usages;
    digitizer.barrel_usage_count = pen.barrel_usage_count;
    digitizer.enable_eraser = pen.enable_eraser;
    digitizer.enable_hover = pen.enable_hover;
    ok = emit_descriptor(output_directory, "digitizer",
                         aoahid_spec_create_digitizer(&digitizer, &spec), spec) &&
         ok;
    spec = nullptr;

    static constexpr std::array<std::uint8_t, 21U> raw_descriptor{
        0x05U, 0x01U, 0x09U, 0x06U, 0xA1U, 0x01U, 0x85U, 0x01U, 0x15U, 0x00U, 0x25U,
        0x01U, 0x75U, 0x01U, 0x95U, 0x01U, 0x09U, 0x01U, 0x81U, 0x02U, 0xC0U};
    static constexpr aoahid_raw_report raw_report{{1U, 1U, {}}, 2U};
    const aoahid_raw_options raw{static_cast<std::uint32_t>(sizeof(aoahid_raw_options)),
                                 0U,
                                 raw_descriptor.data(),
                                 raw_descriptor.size(),
                                 &raw_report,
                                 1U,
                                 1U,
                                 0U,
                                 0U};
    ok = emit_descriptor(output_directory, "raw-validated", aoahid_spec_create_raw(&raw, &spec),
                         spec) &&
         ok;

    if (argc == 4) {
        if (argv[3] == nullptr || argv[3][0] == '\0')
            return 2;
        std::ofstream catalog(argv[3], std::ios::binary | std::ios::trunc);
        catalog << "key\tandroid_status\tinput\toutput\tfeature_transport\tdescriptor_"
                   "length\treports\n";
        for (const manifest_record& record : manifest_records) {
            catalog << record.key << '\t' << record.android_status << '\t' << record.input_supported
                    << '\t' << record.output_supported << '\t' << record.feature_transport_supported
                    << '\t' << record.descriptor_length << '\t' << record.reports << '\n';
        }
        catalog.close();
        if (!catalog) {
            std::fprintf(stderr, "failed to write manifest catalog: %s\n", argv[3]);
            ok = false;
        }
    }

    return ok ? 0 : 1;
}
