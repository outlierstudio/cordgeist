// SPDX-License-Identifier: MIT
// Copyright (c) 2026 libaoahid contributors
/* Proves the public header and exported entry points are usable from C; USB
 * transport behavior is covered by the integration suite. */
#include "aoahid.h"

#include <stdint.h>

/* These assertions are part of the ABI gate, not platform-behaviour claims.
 * They remain true even when a consumer enables a short-enum compiler mode. */
_Static_assert(sizeof(aoahid_result) == sizeof(int32_t), "aoahid_result ABI width");
_Static_assert(sizeof(aoahid_event_mode) == sizeof(int32_t), "aoahid_event_mode ABI width");
_Static_assert(sizeof(aoahid_interface_claim_policy) == sizeof(int32_t),
               "aoahid_interface_claim_policy ABI width");
_Static_assert(sizeof(aoahid_log_level) == sizeof(int32_t), "aoahid_log_level ABI width");
_Static_assert(sizeof(aoahid_profile_kind) == sizeof(int32_t), "aoahid_profile_kind ABI width");
_Static_assert(sizeof(aoahid_android_status) == sizeof(int32_t), "aoahid_android_status ABI width");
_Static_assert(sizeof(aoahid_axis_role) == sizeof(int32_t), "aoahid_axis_role ABI width");
_Static_assert(sizeof(aoahid_dpad_representation) == sizeof(int32_t),
               "aoahid_dpad_representation ABI width");
_Static_assert(sizeof(aoahid_usage_semantic) == sizeof(int32_t), "aoahid_usage_semantic ABI width");

int main(void) {
    aoahid_device_options device;
    device.validate_reports = 1U;
    aoahid_device_options_init(&device);
    aoahid_touchpad_options touchpad;
    aoahid_touchpad_options_init(&touchpad);
    aoahid_capability_manifest manifest;
    aoahid_capability_manifest_init(&manifest);
    aoahid_keyboard_options_init(NULL);
    const int initialized = device.struct_size == sizeof(device) && device.validate_reports == 0U &&
                            touchpad.struct_size == sizeof(touchpad) &&
                            manifest.struct_size == sizeof(manifest);
    return initialized && AOAHID_OK == 0 && AOAHID_ERR_INTERNAL == 16 &&
                   AOAHID_USAGE_NAMED_ARRAY == 10
               ? 0
               : 1;
}
