# SPDX-License-Identifier: MIT
# Copyright (c) 2026 libaoahid contributors
"""What the Python package promises: a small surface, and no value chosen for the caller.

The layout against the compiled C is test_layout.py, which is generated.
"""

import ctypes as c

import aoahid
from aoahid import native
from aoahid.native import ContextOptions, DeviceOptions, KeyboardOptions


def test_struct_size_is_not_filled_by_binding():
    options = KeyboardOptions()
    assert options.struct_size == 0
    options.struct_size = c.sizeof(options)
    assert options.struct_size == c.sizeof(KeyboardOptions)


def test_context_options_exposes_explicit_choice_fields():
    names = {name for name, *_ in ContextOptions._fields_}
    assert {"struct_size", "event_mode", "log_level", "log_sink"} <= names


def test_device_options_exposes_target_parser_policies():
    names = {name for name, *_ in DeviceOptions._fields_}
    assert "linux_hid" in names
    policy = {name for name, *_ in native.LinuxHidPolicy._fields_}
    assert policy == {
        "descriptor_bytes",
        "fields_per_report",
        "global_stack_depth",
        "report_size_bits",
        "usages",
        "report_data_bits",
    }


def test_package_exports_only_the_binding_surface():
    assert "load" in aoahid.__all__
    assert "AOAHID_OK" in aoahid.__all__
    assert not hasattr(aoahid, "c")
    assert not hasattr(aoahid, "os")
    assert not hasattr(aoahid, "Union")


if __name__ == "__main__":
    test_struct_size_is_not_filled_by_binding()
    test_context_options_exposes_explicit_choice_fields()
    test_device_options_exposes_target_parser_policies()
    test_package_exports_only_the_binding_surface()
