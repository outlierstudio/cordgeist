#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
# Copyright (c) 2026 libaoahid contributors
"""Compare the compiler-observed public C ABI with the golden schema.

The AOAHID_VERSION_* entries in GOLDEN_CONSTANTS track the version recorded
in include/aoahid.h and are the only part of this file that changes across
releases; tools/release/sync_version.py keeps them in sync.
"""

from __future__ import annotations

import ctypes as c
import subprocess
import sys
from pathlib import Path


I32 = c.c_int32
U8 = c.c_uint8
U16 = c.c_uint16
U32 = c.c_uint32
U64 = c.c_uint64
SIZE = c.c_size_t
CHAR_PTR = c.c_char_p
VOID_PTR = c.c_void_p
U8_PTR = c.POINTER(U8)
U16_PTR = c.POINTER(U16)
I32_PTR = c.POINTER(I32)
CHAR_PTR_PTR = c.POINTER(CHAR_PTR)
LOG_SINK = c.CFUNCTYPE(None, VOID_PTR, I32, CHAR_PTR)


class ErrorDetail(c.Structure):
    _fields_ = [
        ("code", I32),
        ("field", CHAR_PTR),
        ("reason", CHAR_PTR),
        ("transport_status", I32),
        ("aoa_request", I32),
        ("node_id", U16),
        ("report_id", U16),
        ("offset", U32),
        ("length", U32),
    ]


class ContextOptions(c.Structure):
    _fields_ = [
        ("struct_size", U32),
        ("reserved", U32),
        ("event_mode", I32),
        ("log_level", I32),
        ("log_sink", LOG_SINK),
        ("log_user", VOID_PTR),
    ]


class DeviceInfo(c.Structure):
    _fields_ = [
        ("bus_number", U8),
        ("device_address", U8),
        ("port_path", U8_PTR),
        ("port_path_length", SIZE),
        ("vendor_id", U16),
        ("product_id", U16),
        ("protocol_version", U16),
        ("serial", CHAR_PTR),
        ("product", CHAR_PTR),
    ]


class AoaStrings(c.Structure):
    _fields_ = [
        ("manufacturer", CHAR_PTR),
        ("model", CHAR_PTR),
        ("description", CHAR_PTR),
        ("version", CHAR_PTR),
        ("uri", CHAR_PTR),
        ("serial", CHAR_PTR),
    ]


class LinuxHidPolicy(c.Structure):
    _fields_ = [
        ("descriptor_bytes", U32),
        ("fields_per_report", U32),
        ("global_stack_depth", U32),
        ("report_size_bits", U32),
        ("usages", U64),
        ("report_data_bits", U64),
    ]


class DeviceOptions(c.Structure):
    _fields_ = [
        ("struct_size", U32),
        ("reserved", U32),
        ("descriptor_fragment_bytes", U32),
        ("transfer_pool_slots", U32),
        ("maximum_report_bytes", U32),
        ("aoa_descriptor_wire_policy_bytes", U32),
        ("linux_hid", LinuxHidPolicy),
        ("target_ep0_data_policy_bytes", U32),
        ("host_control_buffer_policy_bytes", U32),
        ("interface_claim_policy", I32),
        ("interface_number", I32),
        ("validate_reports", U32),
    ]


class AccessoryOptions(c.Structure):
    _fields_ = [
        ("struct_size", U32),
        ("reserved", U32),
        ("strings", AoaStrings),
    ]


class ChannelOptions(c.Structure):
    _fields_ = [
        ("struct_size", U32),
        ("reserved", U32),
        ("interface_class", U8),
        ("interface_subclass", U8),
        ("interface_protocol", U8),
        ("reserved8", U8),
        ("in_transfers", U32),
        ("out_transfers", U32),
        ("transfer_bytes", U32),
        ("zero_length_termination", U32),
        ("read_mode", I32),
    ]


class NodeOptions(c.Structure):
    _fields_ = [
        ("struct_size", U32),
        ("reserved", U32),
        ("has_reserved_slots", U32),
        ("reserved_slots", U32),
    ]


class PhysicalProperties(c.Structure):
    _fields_ = [
        ("enabled", U32),
        ("minimum", I32),
        ("maximum", I32),
        ("unit_exponent", I32),
        ("unit", U32),
    ]


class IntegerField(c.Structure):
    _fields_ = [
        ("logical_minimum", I32),
        ("logical_maximum", I32),
        ("bit_width", U32),
        ("physical", PhysicalProperties),
    ]


class ReportIdOption(c.Structure):
    _fields_ = [("enabled", U32), ("value", U8), ("reserved8", U8 * 3)]


class BatteryReport(c.Structure):
    _fields_ = [
        ("report_id", ReportIdOption),
        ("strength", IntegerField),
    ]


class KeyboardOptions(c.Structure):
    _fields_ = [
        ("struct_size", U32),
        ("reserved", U32),
        ("report_id", ReportIdOption),
        ("usage_minimum", U16),
        ("usage_maximum", U16),
        ("enable_battery", U32),
        ("battery", BatteryReport),
    ]


class MouseOptions(c.Structure):
    _fields_ = [
        ("struct_size", U32),
        ("reserved", U32),
        ("report_id", ReportIdOption),
        ("button_count", U32),
        ("x", IntegerField),
        ("y", IntegerField),
        ("enable_wheel", U32),
        ("wheel", IntegerField),
        ("enable_pan", U32),
        ("pan", IntegerField),
        ("enable_battery", U32),
        ("battery", BatteryReport),
    ]


class ToggleOptions(c.Structure):
    _fields_ = [
        ("struct_size", U32),
        ("reserved", U32),
        ("report_id", ReportIdOption),
        ("application_page", U16),
        ("application_usage", U16),
        ("field_page", U16),
        ("reserved16", U16),
        ("allowed_usages", U16_PTR),
        ("allowed_usage_count", SIZE),
        ("usage_semantics", I32_PTR),
        ("expected_linux_event_types", CHAR_PTR_PTR),
        ("expected_linux_codes", CHAR_PTR_PTR),
        ("enable_battery", U32),
        ("battery", BatteryReport),
    ]


class GamepadAxisOption(c.Structure):
    _fields_ = [
        ("role", I32),
        ("usage_page", U16),
        ("usage", U16),
        ("value", IntegerField),
        ("neutral_value", I32),
        ("expected_linux_code", CHAR_PTR),
        ("expected_android_axis", CHAR_PTR),
    ]


class GamepadOptions(c.Structure):
    _fields_ = [
        ("struct_size", U32),
        ("reserved", U32),
        ("report_id", ReportIdOption),
        ("axes", c.POINTER(GamepadAxisOption)),
        ("axis_count", SIZE),
        ("button_count", U32),
        ("button_usage_minimum", U16),
        ("dpad_representation", I32),
        ("hat_logical_minimum", I32),
        ("hat_logical_maximum", I32),
        ("hat_bit_width", U32),
        ("enable_battery", U32),
        ("battery", BatteryReport),
    ]


class TouchscreenOptions(c.Structure):
    _fields_ = [
        ("struct_size", U32),
        ("reserved", U32),
        ("report_id", ReportIdOption),
        ("maximum_contacts", U32),
        ("contacts_per_report", U32),
        ("contact_id", IntegerField),
        ("x", IntegerField),
        ("y", IntegerField),
        ("contact_count", IntegerField),
        ("enable_pressure", U32),
        ("pressure", IntegerField),
        ("enable_width", U32),
        ("width", IntegerField),
        ("enable_height", U32),
        ("height", IntegerField),
        ("enable_azimuth", U32),
        ("azimuth", IntegerField),
        ("enable_scan_time", U32),
        ("scan_time", IntegerField),
        ("scan_time_unit_100us", U32),
        ("enable_contact_count_maximum_feature_declaration", U32),
        ("enable_multi_packet_frames", U32),
        ("enable_battery", U32),
        ("battery", BatteryReport),
    ]


class TouchpadOptions(c.Structure):
    _fields_ = [
        ("struct_size", U32),
        ("reserved", U32),
        ("report_id", ReportIdOption),
        ("maximum_contacts", U32),
        ("contacts_per_report", U32),
        ("contact_id", IntegerField),
        ("x", IntegerField),
        ("y", IntegerField),
        ("contact_count", IntegerField),
        ("enable_pressure", U32),
        ("pressure", IntegerField),
        ("enable_width", U32),
        ("width", IntegerField),
        ("enable_height", U32),
        ("height", IntegerField),
        ("enable_azimuth", U32),
        ("azimuth", IntegerField),
        ("enable_scan_time", U32),
        ("scan_time", IntegerField),
        ("scan_time_unit_100us", U32),
        ("enable_contact_count_maximum_feature_declaration", U32),
        ("enable_multi_packet_frames", U32),
        ("button_count", U32),
        ("enable_battery", U32),
        ("battery", BatteryReport),
    ]


class PenOptions(c.Structure):
    _fields_ = [
        ("struct_size", U32),
        ("reserved", U32),
        ("report_id", ReportIdOption),
        ("x", IntegerField),
        ("y", IntegerField),
        ("enable_pressure", U32),
        ("pressure", IntegerField),
        ("enable_tilt", U32),
        ("tilt_x", IntegerField),
        ("tilt_y", IntegerField),
        ("enable_twist_target_specific", U32),
        ("twist", IntegerField),
        ("barrel_usages", U16_PTR),
        ("barrel_usage_count", SIZE),
        ("enable_eraser", U32),
        ("enable_hover", U32),
        ("enable_battery", U32),
        ("battery", BatteryReport),
    ]


class DigitizerOptions(c.Structure):
    _fields_ = [
        ("struct_size", U32),
        ("reserved", U32),
        ("report_id", ReportIdOption),
        ("x", IntegerField),
        ("y", IntegerField),
        ("enable_pressure", U32),
        ("pressure", IntegerField),
        ("enable_tilt", U32),
        ("tilt_x", IntegerField),
        ("tilt_y", IntegerField),
        ("enable_twist_target_specific", U32),
        ("twist", IntegerField),
        ("barrel_usages", U16_PTR),
        ("barrel_usage_count", SIZE),
        ("enable_eraser", U32),
        ("enable_hover", U32),
        ("enable_battery", U32),
        ("battery", BatteryReport),
    ]


class RawReport(c.Structure):
    _fields_ = [
        ("report_id", ReportIdOption),
        ("wire_length", U32),
    ]


class RawOptions(c.Structure):
    _fields_ = [
        ("struct_size", U32),
        ("reserved", U32),
        ("descriptor", U8_PTR),
        ("descriptor_length", SIZE),
        ("reports", c.POINTER(RawReport)),
        ("report_count", SIZE),
        ("acknowledges_no_android_support", U32),
        ("requires_output", U32),
        ("requires_feature_response", U32),
    ]


class ReportCapability(c.Structure):
    _fields_ = [
        ("report_id", ReportIdOption),
        ("wire_length", U32),
    ]


class CapabilityManifest(c.Structure):
    _fields_ = [
        ("struct_size", U32),
        ("reserved", U32),
        ("input_supported", U32),
        ("output_supported", U32),
        ("feature_transport_supported", U32),
        ("android_status", I32),
        ("profile_kind", I32),
        ("reports", c.POINTER(ReportCapability)),
        ("report_count", SIZE),
        ("descriptor_length", SIZE),
    ]


class TouchscreenExtra(c.Structure):
    _fields_ = [
        ("pressure", I32),
        ("width", I32),
        ("height", I32),
        ("azimuth", I32),
    ]


class TouchpadExtra(c.Structure):
    _fields_ = [
        ("pressure", I32),
        ("width", I32),
        ("height", I32),
        ("azimuth", I32),
    ]


class PenSample(c.Structure):
    _fields_ = [
        ("in_range", U32),
        ("tip", U32),
        ("eraser", U32),
        ("barrel_buttons", U32),
        ("x", I32),
        ("y", I32),
        ("pressure", I32),
        ("tilt_x", I32),
        ("tilt_y", I32),
        ("twist", I32),
    ]


class DigitizerSample(c.Structure):
    _fields_ = [
        ("in_range", U32),
        ("tip", U32),
        ("eraser", U32),
        ("barrel_buttons", U32),
        ("x", I32),
        ("y", I32),
        ("pressure", I32),
        ("tilt_x", I32),
        ("tilt_y", I32),
        ("twist", I32),
    ]


GOLDEN_STRUCTS = {
    "aoahid_error_detail": ErrorDetail,
    "aoahid_context_options": ContextOptions,
    "aoahid_device_info": DeviceInfo,
    "aoahid_aoa_strings": AoaStrings,
    "aoahid_linux_hid_policy": LinuxHidPolicy,
    "aoahid_device_options": DeviceOptions,
    "aoahid_accessory_options": AccessoryOptions,
    "aoahid_channel_options": ChannelOptions,
    "aoahid_node_options": NodeOptions,
    "aoahid_physical_properties": PhysicalProperties,
    "aoahid_integer_field": IntegerField,
    "aoahid_report_id_option": ReportIdOption,
    "aoahid_battery_report": BatteryReport,
    "aoahid_keyboard_options": KeyboardOptions,
    "aoahid_mouse_options": MouseOptions,
    "aoahid_toggle_options": ToggleOptions,
    "aoahid_gamepad_axis_option": GamepadAxisOption,
    "aoahid_gamepad_options": GamepadOptions,
    "aoahid_touchscreen_options": TouchscreenOptions,
    "aoahid_touchpad_options": TouchpadOptions,
    "aoahid_pen_options": PenOptions,
    "aoahid_digitizer_options": DigitizerOptions,
    "aoahid_raw_report": RawReport,
    "aoahid_raw_options": RawOptions,
    "aoahid_report_capability": ReportCapability,
    "aoahid_capability_manifest": CapabilityManifest,
    "aoahid_touchscreen_extra": TouchscreenExtra,
    "aoahid_touchpad_extra": TouchpadExtra,
    "aoahid_pen_sample": PenSample,
    "aoahid_digitizer_sample": DigitizerSample,
}


GOLDEN_CONSTANTS = {
    "AOAHID_VERSION_MAJOR": 2,
    "AOAHID_VERSION_MINOR": 0,
    "AOAHID_VERSION_PATCH": 3,
    "AOAHID_CONTROL_TIMEOUT_MS": 500,
    "AOAHID_SEND_TIMEOUT_MS": 500,
    "AOAHID_DRAIN_TIMEOUT_MS": 1000,
    "AOAHID_OK": 0,
    "AOAHID_ERR_PARAM": 1,
    "AOAHID_ERR_UNSET_FIELD": 2,
    "AOAHID_ERR_UNSUPPORTED": 3,
    "AOAHID_ERR_NOT_AOA": 4,
    "AOAHID_ERR_ACCESS": 6,
    "AOAHID_ERR_BUSY": 7,
    "AOAHID_ERR_NO_DEVICE": 8,
    "AOAHID_ERR_STALL": 9,
    "AOAHID_ERR_TIMEOUT": 10,
    "AOAHID_ERR_SHORT_TRANSFER": 11,
    "AOAHID_ERR_DESCRIPTOR_REJECTED": 12,
    "AOAHID_ERR_IO": 13,
    "AOAHID_ERR_OVERFLOW": 14,
    "AOAHID_CLOSE_PENDING": 15,
    "AOAHID_ERR_INTERNAL": 16,
    "AOAHID_EVENT_CALLER_POLL": 1,
    "AOAHID_EVENT_INTERNAL_THREAD": 2,
    "AOAHID_CHANNEL_READ_STREAM": 1,
    "AOAHID_CHANNEL_READ_REQUEST": 2,
    "AOAHID_INTERFACE_CLAIM_NONE": 1,
    "AOAHID_INTERFACE_CLAIM_EXPLICIT": 2,
    "AOAHID_LOG_DISABLED": 1,
    "AOAHID_LOG_ERROR": 2,
    "AOAHID_LOG_INFO": 3,
    "AOAHID_LOG_TRACE": 4,
    "AOAHID_PROFILE_KEYBOARD": 1,
    "AOAHID_PROFILE_MOUSE": 2,
    "AOAHID_PROFILE_TOGGLE": 8,
    "AOAHID_PROFILE_GAMEPAD": 7,
    "AOAHID_PROFILE_TOUCHSCREEN": 3,
    "AOAHID_PROFILE_PEN": 5,
    "AOAHID_PROFILE_RAW": 9,
    "AOAHID_PROFILE_TOUCHPAD": 4,
    "AOAHID_PROFILE_DIGITIZER": 6,
    "AOAHID_ANDROID_PORTABLE_CANDIDATE": 1,
    "AOAHID_ANDROID_CONDITIONAL": 2,
    "AOAHID_ANDROID_CUSTOM_SYSTEM_ONLY": 3,
    "AOAHID_ANDROID_UNSUPPORTED": 4,
    "AOAHID_ANDROID_UNKNOWN": 5,
    "AOAHID_AXIS_X": 1,
    "AOAHID_AXIS_Y": 2,
    "AOAHID_AXIS_Z": 3,
    "AOAHID_AXIS_RX": 4,
    "AOAHID_AXIS_RY": 5,
    "AOAHID_AXIS_RZ": 6,
    "AOAHID_AXIS_SLIDER": 7,
    "AOAHID_AXIS_SIMULATION_ACCELERATOR": 8,
    "AOAHID_AXIS_SIMULATION_BRAKE": 9,
    "AOAHID_AXIS_SIMULATION_STEERING": 10,
    "AOAHID_AXIS_DIAL": 11,
    "AOAHID_AXIS_WHEEL": 12,
    "AOAHID_AXIS_SIMULATION_RUDDER": 13,
    "AOAHID_AXIS_SIMULATION_THROTTLE": 14,
    "AOAHID_DPAD_NONE": 1,
    "AOAHID_DPAD_HAT": 2,
    "AOAHID_DPAD_BUTTONS": 3,
    "AOAHID_USAGE_SELECTOR_BITMAP": 1,
    "AOAHID_USAGE_ON_OFF_TOGGLE": 2,
    "AOAHID_USAGE_ON_OFF_MAINTAINED": 3,
    "AOAHID_USAGE_MOMENTARY": 4,
    "AOAHID_USAGE_ONE_SHOT": 5,
    "AOAHID_USAGE_RETRIGGER": 6,
    "AOAHID_USAGE_ON_OFF_PAIR": 7,
    "AOAHID_USAGE_LINEAR": 8,
    "AOAHID_USAGE_DYNAMIC_VALUE": 9,
    "AOAHID_USAGE_NAMED_ARRAY": 10,
}


def expected_values() -> dict[str, int]:
    result = dict(GOLDEN_CONSTANTS)
    for public_name, structure in GOLDEN_STRUCTS.items():
        result[f"{public_name}.size"] = c.sizeof(structure)
        for field_name, _field_type in structure._fields_:
            result[f"{public_name}.{field_name}"] = getattr(structure, field_name).offset
    return result


def parse_observed(output: str) -> dict[str, int]:
    result: dict[str, int] = {}
    for line in output.splitlines():
        key, separator, value = line.partition("=")
        if not separator or key in result:
            raise ValueError(f"malformed or duplicate layout-oracle line: {line!r}")
        result[key] = int(value)
    return result


def main() -> int:
    if len(sys.argv) != 2:
        print("usage: check_abi_golden.py LAYOUT_ORACLE", file=sys.stderr)
        return 2
    executable = Path(sys.argv[1])
    completed = subprocess.run(
        [str(executable)], check=True, text=True, stdout=subprocess.PIPE
    )
    observed = parse_observed(completed.stdout)
    expected = expected_values()
    failed = False
    for key in sorted(expected.keys() - observed.keys()):
        print(f"ABI oracle omitted {key}", file=sys.stderr)
        failed = True
    for key in sorted(observed.keys() - expected.keys()):
        print(f"ABI oracle added unknown entry {key}", file=sys.stderr)
        failed = True
    for key in sorted(expected.keys() & observed.keys()):
        if observed[key] != expected[key]:
            print(
                f"ABI drift: {key}: observed {observed[key]}, "
                f"expected {expected[key]}",
                file=sys.stderr,
            )
            failed = True
    if failed:
        return 1
    print(
        f"C ABI matches: {len(GOLDEN_STRUCTS)} structures, "
        f"{len(GOLDEN_CONSTANTS)} constants"
    )
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
