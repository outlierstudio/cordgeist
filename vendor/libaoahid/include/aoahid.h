// SPDX-License-Identifier: MIT
// Copyright (c) 2026 libaoahid contributors
#ifndef AOAHID_H
#define AOAHID_H

/*
 * libaoahid: HID input to an Android device over USB, through Android Open Accessory (AOA) 2.0.
 * This header is the whole stable C ABI. It exposes no libusb or C++ type.
 *
 * Objects
 *   Context   libusb and, in AOAHID_EVENT_INTERNAL_THREAD mode, one event thread.
 *   Device    one USB device opened on a Context.
 *   Spec      an immutable HID report descriptor and report layout, built by a factory
 *             (aoahid_spec_create_*). It needs no device and is reference counted.
 *   Node      a Spec registered on a Device as one AOA HID, with the state its input calls
 *             change (aoahid_keyboard_key, aoahid_touchscreen_contact, ...). A Node sends a
 *             report when it is submitted.
 *   Channel   a Bulk IN/OUT interface of a Device, such as ADB, read and written as a stream.
 *
 * Rules every function follows
 *   Results    Every call records the details of its result for the calling thread, read with
 *              aoahid_last_error(). Results that come from USB (the "transport errors") are
 *              AOAHID_ERR_UNSUPPORTED, _ACCESS, _BUSY, _NO_DEVICE, _STALL, _TIMEOUT,
 *              _SHORT_TRANSFER, _IO and _OVERFLOW; error_detail.transport_status holds the libusb
 *              status. A report transfer fails after the call that queued it has returned, so its
 *              error is returned by a later call on the same Node, Device or Context. Any call can
 *              also return AOAHID_ERR_INTERNAL (an allocation failed or an internal invariant
 *              broke). The Returns lines below list what is particular to each function.
 *   Outputs    A function that creates an object returns it through an out_* pointer, which is
 *              set to null first and stays null when the call fails.
 *   Closing    The *_close and *_destroy functions follow one rule:
 *                AOAHID_ERR_PARAM or AOAHID_ERR_UNSET_FIELD  nothing was done; the handle is still
 *                                                           the caller's.
 *                AOAHID_CLOSE_PENDING                       a transfer of the object is still in
 *                                                           flight; the handle is still the
 *                                                           caller's: call again later.
 *                any other result                           the object is gone and its handle must
 *                                                           not be used again. An error says what
 *                                                           failed on the way out.
 *   Threads    A Context and every Device, Node and Channel on it form one domain: the caller
 *              makes sure no two calls in a domain run at the same time, unless a function says
 *              otherwise. Specs and Discovery snapshots are outside any domain. A log sink must
 *              not call back into the library.
 *   Timeouts   The library keeps no timeout: every call that can wait takes its timeouts in
 *              milliseconds as arguments. A control, send or drain timeout of zero is refused with
 *              AOAHID_ERR_UNSET_FIELD before anything is done. AOAHID_CONTROL_TIMEOUT_MS and the
 *              constants next to it are values that work.
 *   Structures Each options structure starts with struct_size (its sizeof) and reserved (zero).
 *              Its *_init function sets both and fills in working values. A required field that
 *              is zero is refused, and every enumeration starts at 1, so a field left zero never
 *              selects something by accident. A flag (enable_*, down, ...) is exactly 0 or 1.
 *
 * Specifications cited: HID 1.11 and HID Usage Tables 1.7 (HUT) from usb.org, and AOA 2.0:
 * https://source.android.com/docs/core/interaction/accessories/aoa2
 * docs/API.md is the guide to this header.
 */

#include <stddef.h>
#include <stdint.h>
#include <string.h>

#if defined(_WIN32)
#if defined(AOAHID_BUILDING_LIBRARY)
#define AOAHID_API __declspec(dllexport)
#elif defined(AOAHID_SHARED)
#define AOAHID_API __declspec(dllimport)
#else
#define AOAHID_API
#endif
#define AOAHID_CALL __cdecl
#else
#define AOAHID_API __attribute__((visibility("default")))
#define AOAHID_CALL
#endif

#ifdef __cplusplus
extern "C" {
#endif

#define AOAHID_VERSION_MAJOR 2
#define AOAHID_VERSION_MINOR 0
#define AOAHID_VERSION_PATCH 3

typedef struct aoahid_context aoahid_context;
typedef struct aoahid_discovery aoahid_discovery;
typedef struct aoahid_device aoahid_device;
typedef struct aoahid_spec aoahid_spec;
typedef struct aoahid_node aoahid_node;
typedef struct aoahid_channel aoahid_channel;

/* ---- Constants --------------------------------------------------------------------------
 * Every enumeration is an int32_t typedef with anonymous constants, so structure layout does not
 * depend on how a compiler sizes an enum. */

/* A result libmimhid also has keeps the same number there. 5 is not used. */
typedef int32_t aoahid_result;
enum {
    AOAHID_OK = 0,
    AOAHID_ERR_PARAM = 1,       /* a null pointer, a bad value or a handle in the wrong state */
    AOAHID_ERR_UNSET_FIELD = 2, /* a required field or timeout is zero */
    AOAHID_ERR_UNSUPPORTED = 3, /* not possible with these arguments or this device */
    AOAHID_ERR_NOT_AOA = 4,     /* the device does not speak AOA (request 51) */
    AOAHID_ERR_ACCESS = 6,      /* no permission to open the USB device */
    AOAHID_ERR_BUSY = 7,        /* in use: a report in flight, a claimed interface, ... */
    AOAHID_ERR_NO_DEVICE = 8,   /* the device is gone; this stays until it is closed */
    AOAHID_ERR_STALL = 9,       /* the device refused the request */
    AOAHID_ERR_TIMEOUT = 10,
    AOAHID_ERR_SHORT_TRANSFER = 11,      /* a transfer moved fewer bytes than asked */
    AOAHID_ERR_DESCRIPTOR_REJECTED = 12, /* reserved: no call returns it (libmimhid does) */
    AOAHID_ERR_IO = 13,
    AOAHID_ERR_OVERFLOW = 14,  /* a size, count or limit was exceeded */
    AOAHID_CLOSE_PENDING = 15, /* see "Closing" above */
    AOAHID_ERR_INTERNAL = 16
};

typedef int32_t aoahid_event_mode;
enum {
    /* The caller drives libusb with aoahid_context_poll. No library thread and no lock on the
     * send path; the caller serializes every call on the Context. */
    AOAHID_EVENT_CALLER_POLL = 1,
    /* One library thread drives libusb. Completions are synchronized, at the cost of some lock
     * contention that caller-poll mode does not have. */
    AOAHID_EVENT_INTERNAL_THREAD = 2
};

typedef int32_t aoahid_interface_claim_policy;
enum {
    AOAHID_INTERFACE_CLAIM_NONE = 1,    /* claim no interface for the AOA control requests */
    AOAHID_INTERFACE_CLAIM_EXPLICIT = 2 /* claim interface_number while the Device is open */
};

typedef int32_t aoahid_log_level;
enum { AOAHID_LOG_DISABLED = 1, AOAHID_LOG_ERROR = 2, AOAHID_LOG_INFO = 3, AOAHID_LOG_TRACE = 4 };

/* The Application Collection of each profile is in docs/PROFILES.md. */
typedef int32_t aoahid_profile_kind;
enum {
    AOAHID_PROFILE_KEYBOARD = 1,
    AOAHID_PROFILE_MOUSE = 2,
    AOAHID_PROFILE_TOUCHSCREEN = 3,
    AOAHID_PROFILE_TOUCHPAD = 4,
    AOAHID_PROFILE_PEN = 5,
    AOAHID_PROFILE_DIGITIZER = 6,
    AOAHID_PROFILE_GAMEPAD = 7,
    AOAHID_PROFILE_TOGGLE = 8,
    AOAHID_PROFILE_RAW = 9
};

/* A static classification of a descriptor's form against documented Android input behavior; see
 * docs/PROFILES.md. It is not a test result. */
typedef int32_t aoahid_android_status;
enum {
    AOAHID_ANDROID_PORTABLE_CANDIDATE = 1, /* follows documented Android behavior */
    AOAHID_ANDROID_CONDITIONAL = 2,        /* depends on the Android release, kernel or OEM */
    AOAHID_ANDROID_CUSTOM_SYSTEM_ONLY = 3,
    AOAHID_ANDROID_UNSUPPORTED = 4,
    AOAHID_ANDROID_UNKNOWN = 5 /* a Raw descriptor, which the library does not classify */
};

typedef int32_t aoahid_axis_role;
enum {
    AOAHID_AXIS_X = 1,
    AOAHID_AXIS_Y = 2,
    AOAHID_AXIS_Z = 3,
    AOAHID_AXIS_RX = 4,
    AOAHID_AXIS_RY = 5,
    AOAHID_AXIS_RZ = 6,
    AOAHID_AXIS_SLIDER = 7,
    AOAHID_AXIS_SIMULATION_ACCELERATOR = 8,
    AOAHID_AXIS_SIMULATION_BRAKE = 9,
    AOAHID_AXIS_SIMULATION_STEERING = 10,
    AOAHID_AXIS_DIAL = 11,
    AOAHID_AXIS_WHEEL = 12,
    AOAHID_AXIS_SIMULATION_RUDDER = 13,
    AOAHID_AXIS_SIMULATION_THROTTLE = 14
};

typedef int32_t aoahid_dpad_representation;
enum {
    AOAHID_DPAD_NONE = 1,
    AOAHID_DPAD_HAT = 2,    /* one eight-way Hat Switch */
    AOAHID_DPAD_BUTTONS = 3 /* four D-pad Up/Down/Right/Left bits */
};

/* The HUT control type of one Toggle Usage (HUT 1.7 section 3.4); it selects the Input item flags.
 * ON_OFF_PAIR, LINEAR, DYNAMIC_VALUE and NAMED_ARRAY carry a value or a direction that press and
 * release cannot express, so aoahid_spec_create_toggle refuses them with AOAHID_ERR_UNSUPPORTED. */
typedef int32_t aoahid_usage_semantic;
enum {
    AOAHID_USAGE_SELECTOR_BITMAP = 1,
    AOAHID_USAGE_ON_OFF_TOGGLE = 2,
    AOAHID_USAGE_ON_OFF_MAINTAINED = 3,
    AOAHID_USAGE_MOMENTARY = 4,
    AOAHID_USAGE_ONE_SHOT = 5,
    AOAHID_USAGE_RETRIGGER = 6,
    AOAHID_USAGE_ON_OFF_PAIR = 7,
    AOAHID_USAGE_LINEAR = 8,
    AOAHID_USAGE_DYNAMIC_VALUE = 9,
    AOAHID_USAGE_NAMED_ARRAY = 10
};

typedef int32_t aoahid_channel_read_mode;
enum {
    /* in_transfers IN transfers stay submitted and reads return their bytes as one stream. A
     * transfer ends only when it is full or on a short packet, so a device that ends a
     * packet-aligned message without a zero-length packet leaves those bytes waiting. */
    AOAHID_CHANNEL_READ_STREAM = 1,
    /* No read-ahead: a read with nothing buffered submits one IN transfer of its capacity rounded
     * up to wMaxPacketSize (at most transfer_bytes), as AOSP adb does for a length-prefixed
     * payload. It completes as soon as it is full, with no zero-length packet needed. */
    AOAHID_CHANNEL_READ_REQUEST = 2
};

/* ---- Diagnostics ------------------------------------------------------------------------ */

/* Called with one formatted line per event, on the thread that caused it. */
typedef void(AOAHID_CALL* aoahid_log_sink)(void* user, aoahid_log_level level, const char* message);

/* The details of the calling thread's last result. Fields that do not apply are zero or null. */
typedef struct aoahid_error_detail {
    aoahid_result code;
    const char* field;        /* the argument, option or step concerned, e.g. "node.close" */
    const char* reason;       /* one English sentence */
    int32_t transport_status; /* the libusb status of a USB failure */
    int32_t aoa_request;      /* the AOA request involved (51 to 57) */
    uint16_t node_id;         /* the AOA HID ID of the Node involved */
    uint16_t report_id;
    uint32_t offset;
    uint32_t length;
} aoahid_error_detail;

/* ---- Context, discovery and Device options ---------------------------------------------- */

typedef struct aoahid_context_options {
    uint32_t struct_size;
    uint32_t reserved;
    aoahid_event_mode event_mode;
    aoahid_log_level log_level; /* anything but AOAHID_LOG_DISABLED needs log_sink */
    aoahid_log_sink log_sink;
    void* log_user;
} aoahid_context_options;

/* A USB device as aoahid_discover found it. A caller may also fill one in itself to open a known
 * device; only bus_number, device_address and port_path then identify it. */
typedef struct aoahid_device_info {
    uint8_t bus_number;
    uint8_t device_address;
    const uint8_t* port_path; /* libusb port numbers from the root hub, at most 7 */
    size_t port_path_length;
    uint16_t vendor_id;
    uint16_t product_id;
    uint16_t protocol_version; /* the AOA version request 51 returned; 2 or more has HID */
    const char* serial;
    const char* product;
} aoahid_device_info;

/* The AOA identification strings (request 52, string IDs 0 to 5): each NUL-terminated, at most
 * 256 bytes with the NUL, or null to leave it out. Android matches manufacturer and model against
 * an app's accessory filter. */
typedef struct aoahid_aoa_strings {
    const char* manufacturer; /* required */
    const char* model;        /* required */
    const char* description;
    const char* version;
    const char* uri;
    const char* serial;
} aoahid_aoa_strings;

/* Limits of the target's Linux HID parser for the kernel it runs; docs/LIMITS.md names the kernel
 * constant behind each and lists them for one revision. aoahid_node_open refuses a Spec that
 * exceeds one with AOAHID_ERR_OVERFLOW. All are required. */
typedef struct aoahid_linux_hid_policy {
    uint32_t descriptor_bytes;   /* largest report descriptor */
    uint32_t fields_per_report;  /* fields in one report */
    uint32_t global_stack_depth; /* Push items in effect at once */
    uint32_t report_size_bits;   /* largest Report Size item */
    uint64_t usages;             /* Usages in one descriptor */
    uint64_t report_data_bits;   /* largest report, in bits */
} aoahid_linux_hid_policy;

/* Every field but interface_number and validate_reports is required. */
typedef struct aoahid_device_options {
    uint32_t struct_size;
    uint32_t reserved;
    uint32_t descriptor_fragment_bytes; /* bytes of descriptor per request 56 */
    uint32_t transfer_pool_slots;       /* report transfers the Device's Nodes share */
    uint32_t maximum_report_bytes;      /* largest report a Node may send */
    /* Largest descriptor sent over AOA: at most 65535, the 16-bit length of request 54. */
    uint32_t aoa_descriptor_wire_policy_bytes;
    aoahid_linux_hid_policy linux_hid;
    uint32_t target_ep0_data_policy_bytes;     /* largest control data stage the target takes */
    uint32_t host_control_buffer_policy_bytes; /* largest control data stage the host takes */
    aoahid_interface_claim_policy interface_claim_policy;
    /* The interface to claim (0 to 255), or -1 with AOAHID_INTERFACE_CLAIM_NONE. Interface 0 is a
     * real interface, so -1 is what "none" is. */
    int32_t interface_number;
    uint32_t validate_reports; /* 1 decodes every generated report again before sending it */
} aoahid_device_options;

typedef struct aoahid_accessory_options {
    uint32_t struct_size;
    uint32_t reserved;
    aoahid_aoa_strings strings;
} aoahid_accessory_options;

/* Selects the first interface (alternate setting 0) with this class, subclass and protocol that
 * has one Bulk IN and one Bulk OUT endpoint. Zero is a valid class, subclass or protocol. */
typedef struct aoahid_channel_options {
    uint32_t struct_size;
    uint32_t reserved;
    uint8_t interface_class;
    uint8_t interface_subclass;
    uint8_t interface_protocol;
    uint8_t reserved8;
    uint32_t in_transfers;   /* read-ahead transfers; required for READ_STREAM, unused otherwise */
    uint32_t out_transfers;  /* required */
    uint32_t transfer_bytes; /* required: bytes per transfer, rounded up to wMaxPacketSize */
    /* 1 ends every write whose length is a nonzero multiple of wMaxPacketSize with a
     * zero-length packet. */
    uint32_t zero_length_termination;
    aoahid_channel_read_mode read_mode; /* required */
} aoahid_channel_options;

/* A Node has one report in flight at a time, taken from the Device's shared pool. With
 * has_reserved_slots 1 and reserved_slots 1 it keeps one slot of its own instead; the reserved
 * slots of all Nodes must leave at least one slot shared. Zero and zero reserve nothing. */
typedef struct aoahid_node_options {
    uint32_t struct_size;
    uint32_t reserved;
    uint32_t has_reserved_slots;
    uint32_t reserved_slots;
} aoahid_node_options;

/* ---- Profile options --------------------------------------------------------------------
 * docs/PROFILES.md describes each profile's descriptor and the limits of every field. A value
 * that may be zero (a logical minimum, a neutral value, a count of optional parts) is never
 * taken for "unset": what is optional has its own enable_* flag or count. */

/* The four HID 1.11 physical Global items for one field. They are emitted only when enabled is 1
 * and are reset right after the field, so they never apply to the next one. */
typedef struct aoahid_physical_properties {
    uint32_t enabled;
    int32_t minimum;
    int32_t maximum;
    int32_t unit_exponent;
    uint32_t unit;
} aoahid_physical_properties;

/* One report field. The logical range must fit bit_width (1 to 32 bits); a bit_width of zero is
 * an unset field. */
typedef struct aoahid_integer_field {
    int32_t logical_minimum;
    int32_t logical_maximum;
    uint32_t bit_width;
    aoahid_physical_properties physical;
} aoahid_integer_field;

/* With enabled 1, every report starts with value (1 to 255). */
typedef struct aoahid_report_id_option {
    uint32_t enabled;
    uint8_t value;
    uint8_t reserved8[3];
} aoahid_report_id_option;

/* Battery Strength (Generic Device Controls 0x06/0x20) as a second Input report in the profile's
 * Application Collection. Both this report_id and the profile's must be enabled and differ.
 * Start the range at 1: some Linux releases ignore a value of 0 (docs/PROFILES.md). */
typedef struct aoahid_battery_report {
    aoahid_report_id_option report_id;
    aoahid_integer_field strength;
} aoahid_battery_report;

/* Full N-key rollover: one bit per Usage from usage_minimum (at least 0x04) to usage_maximum, plus
 * the eight modifiers 0xE0 to 0xE7, which the range must not include. */
typedef struct aoahid_keyboard_options {
    uint32_t struct_size;
    uint32_t reserved;
    aoahid_report_id_option report_id;
    uint16_t usage_minimum;
    uint16_t usage_maximum;
    uint32_t enable_battery;
    aoahid_battery_report battery;
} aoahid_keyboard_options;

/* X, Y, wheel and pan are relative; their ranges must hold negative and positive values. */
typedef struct aoahid_mouse_options {
    uint32_t struct_size;
    uint32_t reserved;
    aoahid_report_id_option report_id;
    uint32_t button_count; /* 1 to 65535 */
    aoahid_integer_field x;
    aoahid_integer_field y;
    uint32_t enable_wheel;
    aoahid_integer_field wheel;
    uint32_t enable_pan;
    aoahid_integer_field pan;
    uint32_t enable_battery;
    aoahid_battery_report battery;
} aoahid_mouse_options;

/* One-bit controls on any HUT page: Consumer, System Control, Camera Control, Telephony, ...
 * allowed_usages, usage_semantics, expected_linux_event_types and expected_linux_codes are
 * parallel arrays of allowed_usage_count entries, all required. The expected Linux strings (such
 * as "EV_KEY" and "KEY_PLAYPAUSE") are kept with the Spec as a record; the library does not read
 * them. On the Camera Control page (0x90) only Auto-focus (0x20) and Shutter (0x21) exist, each
 * as AOAHID_USAGE_ONE_SHOT. */
typedef struct aoahid_toggle_options {
    uint32_t struct_size;
    uint32_t reserved;
    aoahid_report_id_option report_id;
    uint16_t application_page; /* the Application Collection, e.g. 0x0C/0x01 Consumer Control */
    uint16_t application_usage;
    uint16_t field_page; /* the Usage Page of every allowed Usage */
    uint16_t reserved16;
    const uint16_t* allowed_usages;
    size_t allowed_usage_count;
    const aoahid_usage_semantic* usage_semantics;
    const char* const* expected_linux_event_types;
    const char* const* expected_linux_codes;
    uint32_t enable_battery;
    aoahid_battery_report battery;
} aoahid_toggle_options;

/* One axis. usage_page and usage must be the ones of role (docs/PROFILES.md); neutral_value is the
 * value sent at rest and on close, and may be any value in the range, zero included. The two
 * expected strings (such as "ABS_X" and "AXIS_X") are required and kept with the Spec as a record;
 * the library does not read them. */
typedef struct aoahid_gamepad_axis_option {
    aoahid_axis_role role;
    uint16_t usage_page;
    uint16_t usage;
    aoahid_integer_field value;
    int32_t neutral_value;
    const char* expected_linux_code;
    const char* expected_android_axis;
} aoahid_gamepad_axis_option;

/* At least two axes, one of them AOAHID_AXIS_X and one AOAHID_AXIS_Y, each role once. A Hat must
 * be logical 0 to 7 in 4 bits; outside AOAHID_DPAD_HAT the three hat_* fields are zero. */
typedef struct aoahid_gamepad_options {
    uint32_t struct_size;
    uint32_t reserved;
    aoahid_report_id_option report_id;
    const aoahid_gamepad_axis_option* axes;
    size_t axis_count;
    uint32_t button_count;         /* at least 1 */
    uint16_t button_usage_minimum; /* the Button page Usage of the first button, nonzero */
    aoahid_dpad_representation dpad_representation;
    int32_t hat_logical_minimum;
    int32_t hat_logical_maximum;
    uint32_t hat_bit_width;
    uint32_t enable_battery;
    aoahid_battery_report battery;
} aoahid_gamepad_options;

/* Fixed-slot Multi-Touch under the Digitizers / Touch Screen Application Collection. */
typedef struct aoahid_touchscreen_options {
    uint32_t struct_size;
    uint32_t reserved;
    aoahid_report_id_option report_id;
    uint32_t maximum_contacts;          /* 1 to 16 */
    uint32_t contacts_per_report;       /* maximum_contacts, or fewer with multi-packet frames */
    aoahid_integer_field contact_id;    /* holds 0 and at least maximum_contacts values */
    aoahid_integer_field x;             /* holds 0 */
    aoahid_integer_field y;             /* holds 0 */
    aoahid_integer_field contact_count; /* holds 0 to maximum_contacts */
    uint32_t enable_pressure;
    aoahid_integer_field pressure;
    uint32_t enable_width;
    aoahid_integer_field width;
    uint32_t enable_height;
    aoahid_integer_field height;
    uint32_t enable_azimuth;
    aoahid_integer_field azimuth; /* counter-clockwise, minimum 0 (docs/LIMITS.md) */
    uint32_t enable_scan_time;
    aoahid_integer_field scan_time;
    uint32_t scan_time_unit_100us; /* must be 1 with enable_scan_time */
    /* 1 adds Contact Count Maximum as a Constant Feature item (no Feature transport). */
    uint32_t enable_contact_count_maximum_feature_declaration;
    uint32_t enable_multi_packet_frames;
    uint32_t enable_battery;
    aoahid_battery_report battery;
} aoahid_touchscreen_options;

/* The touchscreen report under the Digitizers / Touch Pad Application Collection, plus
 * button_count click buttons (0 for a buttonless clickpad). */
typedef struct aoahid_touchpad_options {
    uint32_t struct_size;
    uint32_t reserved;
    aoahid_report_id_option report_id;
    uint32_t maximum_contacts;
    uint32_t contacts_per_report;
    aoahid_integer_field contact_id;
    aoahid_integer_field x;
    aoahid_integer_field y;
    aoahid_integer_field contact_count;
    uint32_t enable_pressure;
    aoahid_integer_field pressure;
    uint32_t enable_width;
    aoahid_integer_field width;
    uint32_t enable_height;
    aoahid_integer_field height;
    uint32_t enable_azimuth;
    aoahid_integer_field azimuth;
    uint32_t enable_scan_time;
    aoahid_integer_field scan_time;
    uint32_t scan_time_unit_100us;
    uint32_t enable_contact_count_maximum_feature_declaration;
    uint32_t enable_multi_packet_frames;
    uint32_t button_count;
    uint32_t enable_battery;
    aoahid_battery_report battery;
} aoahid_touchpad_options;

/* A stylus under the Digitizers / Pen Application Collection (a direct, on-screen pen). */
typedef struct aoahid_pen_options {
    uint32_t struct_size;
    uint32_t reserved;
    aoahid_report_id_option report_id;
    aoahid_integer_field x;
    aoahid_integer_field y;
    uint32_t enable_pressure;
    aoahid_integer_field pressure;
    uint32_t enable_tilt; /* X Tilt and Y Tilt together */
    aoahid_integer_field tilt_x;
    aoahid_integer_field tilt_y;
    uint32_t enable_twist_target_specific; /* how a target uses Twist varies */
    aoahid_integer_field twist;
    /* Up to 32 unique Digitizers-page Usages, each Barrel Switch (0x44) or Secondary Barrel
     * Switch (0x5A); bit i of a sample's barrel_buttons is barrel_usages[i]. A count of 0 means
     * no barrel button. */
    const uint16_t* barrel_usages;
    size_t barrel_usage_count;
    /* 1 adds HUT Invert (0x3C), which selects the eraser end; not the Eraser Usage (0x45). */
    uint32_t enable_eraser;
    uint32_t enable_hover; /* 1 lets a sample be in range without touching */
    uint32_t enable_battery;
    aoahid_battery_report battery;
} aoahid_pen_options;

/* The pen report under the Digitizers / Digitizer Application Collection (an indirect tablet). */
typedef struct aoahid_digitizer_options {
    uint32_t struct_size;
    uint32_t reserved;
    aoahid_report_id_option report_id;
    aoahid_integer_field x;
    aoahid_integer_field y;
    uint32_t enable_pressure;
    aoahid_integer_field pressure;
    uint32_t enable_tilt;
    aoahid_integer_field tilt_x;
    aoahid_integer_field tilt_y;
    uint32_t enable_twist_target_specific;
    aoahid_integer_field twist;
    const uint16_t* barrel_usages;
    size_t barrel_usage_count;
    uint32_t enable_eraser;
    uint32_t enable_hover;
    uint32_t enable_battery;
    aoahid_battery_report battery;
} aoahid_digitizer_options;

/* An Input report a Raw Node may send: its Report ID and its exact length on the wire. */
typedef struct aoahid_raw_report {
    aoahid_report_id_option report_id;
    uint32_t wire_length;
} aoahid_raw_report;

/* A descriptor of the caller's own. Android support is unknown, so it must be acknowledged; AOA
 * carries no Output or Feature report, so the two requires_* fields must be 0. */
typedef struct aoahid_raw_options {
    uint32_t struct_size;
    uint32_t reserved;
    const uint8_t* descriptor;
    size_t descriptor_length;
    const aoahid_raw_report* reports;
    size_t report_count;
    uint32_t acknowledges_no_android_support; /* must be 1 */
    uint32_t requires_output;                 /* must be 0 */
    uint32_t requires_feature_response;       /* must be 0 */
} aoahid_raw_options;

typedef struct aoahid_report_capability {
    aoahid_report_id_option report_id;
    uint32_t wire_length;
} aoahid_report_capability;

/* What a Spec sends. Filled in by aoahid_spec_manifest and aoahid_node_manifest. */
typedef struct aoahid_capability_manifest {
    uint32_t struct_size;
    uint32_t reserved;
    uint32_t input_supported;
    uint32_t output_supported;            /* always 0: AOA carries no Output report */
    uint32_t feature_transport_supported; /* always 0: AOA carries no Feature report */
    aoahid_android_status android_status;
    aoahid_profile_kind profile_kind;
    const aoahid_report_capability* reports; /* valid while the Spec is */
    size_t report_count;
    size_t descriptor_length;
} aoahid_capability_manifest;

/* ---- Input samples ---------------------------------------------------------------------- */

/* Optional values of one touch contact. A field the Spec did not enable must be 0. */
typedef struct aoahid_touchscreen_extra {
    int32_t pressure;
    int32_t width;
    int32_t height;
    int32_t azimuth;
} aoahid_touchscreen_extra;

typedef struct aoahid_touchpad_extra {
    int32_t pressure;
    int32_t width;
    int32_t height;
    int32_t azimuth;
} aoahid_touchpad_extra;

/* One pen state. tip 1 needs in_range 1; without enable_hover, in_range must equal tip. A field the
 * Spec did not enable must be 0. */
typedef struct aoahid_pen_sample {
    uint32_t in_range;
    uint32_t tip;
    uint32_t eraser;         /* 1 for the eraser end; tip may be 1 too */
    uint32_t barrel_buttons; /* bit i is barrel_usages[i] */
    int32_t x;
    int32_t y;
    int32_t pressure;
    int32_t tilt_x;
    int32_t tilt_y;
    int32_t twist;
} aoahid_pen_sample;

typedef struct aoahid_digitizer_sample {
    uint32_t in_range;
    uint32_t tip;
    uint32_t eraser;
    uint32_t barrel_buttons;
    int32_t x;
    int32_t y;
    int32_t pressure;
    int32_t tilt_x;
    int32_t tilt_y;
    int32_t twist;
} aoahid_digitizer_sample;

/* ---- Initializers -----------------------------------------------------------------------
 * Each *_init function zeroes a structure, sets struct_size, and fills in values that work with a
 * current Android phone, so a caller changes only what it needs. Fields that only the caller can
 * know (a Raw descriptor, the keys a Toggle Node may press) stay zero and are still required.
 * A value the caller sets afterwards is used as given, zero included where zero is valid.
 *
 * The timeouts are values that work for the same targets; the library never uses them itself. */
#define AOAHID_CONTROL_TIMEOUT_MS 500U /* each AOA control request (51 to 56) */
#define AOAHID_SEND_TIMEOUT_MS 500U    /* one report transfer (request 57) */
#define AOAHID_DRAIN_TIMEOUT_MS 1000U  /* a close: reports in flight, then neutral reports */

static inline void aoahid_struct_init(void* value, size_t size) {
    if (value != NULL) {
        const uint32_t struct_size = (uint32_t)size;
        memset(value, 0, size);
        memcpy(value, &struct_size, sizeof(struct_size));
    }
}

static inline aoahid_integer_field
aoahid_integer_field_make(int32_t logical_minimum, // NOLINT(bugprone-easily-swappable-parameters)
                          int32_t logical_maximum, uint32_t bit_width) {
    aoahid_integer_field field;
    memset(&field, 0, sizeof(field));
    field.logical_minimum = logical_minimum;
    field.logical_maximum = logical_maximum;
    field.bit_width = bit_width;
    return field;
}

/* An internal event thread (no aoahid_context_poll needed) and no logging. */
static inline void aoahid_context_options_init(aoahid_context_options* options) {
    aoahid_struct_init(options, sizeof(*options));
    if (options != NULL) {
        options->event_mode = AOAHID_EVENT_INTERNAL_THREAD;
        options->log_level = AOAHID_LOG_DISABLED;
    }
}

/* 4096-byte descriptor fragments and control buffers, 4 transfer slots, reports of up to 1024
 * bytes, no interface claim, and the Linux HID parser limits of the kernel revision in
 * docs/LIMITS.md. */
static inline void aoahid_device_options_init(aoahid_device_options* options) {
    aoahid_struct_init(options, sizeof(*options));
    if (options != NULL) {
        options->descriptor_fragment_bytes = 4096U;
        options->transfer_pool_slots = 4U;
        options->maximum_report_bytes = 1024U;
        options->aoa_descriptor_wire_policy_bytes = 4096U;
        options->linux_hid.descriptor_bytes = 4096U;
        options->linux_hid.fields_per_report = 256U;
        options->linux_hid.global_stack_depth = 4U;
        options->linux_hid.report_size_bits = 256U;
        options->linux_hid.usages = 12288U;
        options->linux_hid.report_data_bits = 65528U;
        options->target_ep0_data_policy_bytes = 4096U;
        options->host_control_buffer_policy_bytes = 4096U;
        options->interface_claim_policy = AOAHID_INTERFACE_CLAIM_NONE;
        options->interface_number = -1;
    }
}

/* Manufacturer and model "libaoahid"; the other strings are not sent. An Android app that filters
 * accessories matches these two, so set your own when you ship one. */
static inline void aoahid_accessory_options_init(aoahid_accessory_options* options) {
    aoahid_struct_init(options, sizeof(*options));
    if (options != NULL) {
        options->strings.manufacturer = "libaoahid";
        options->strings.model = "libaoahid";
    }
}

/* The ADB interface (class 0xFF, subclass 0x42, protocol 0x01, as AOSP adb.h defines it), read on
 * request, transfers of up to 1 MiB (the largest ADB payload), two OUT transfers, and zero-length
 * packet termination: the settings aoahid_adb_proxy runs with. */
static inline void aoahid_channel_options_init(aoahid_channel_options* options) {
    aoahid_struct_init(options, sizeof(*options));
    if (options != NULL) {
        options->interface_class = 0xFFU;
        options->interface_subclass = 0x42U;
        options->interface_protocol = 0x01U;
        options->out_transfers = 2U;
        options->transfer_bytes = 1048576U;
        options->zero_length_termination = 1U;
        options->read_mode = AOAHID_CHANNEL_READ_REQUEST;
    }
}

/* No reserved transfer slot: the Node shares the Device's pool. */
static inline void aoahid_node_options_init(aoahid_node_options* options) {
    aoahid_struct_init(options, sizeof(*options));
}

/* Every key from Keyboard a (0x04) to Keypad Hexadecimal (0xDD). The eight modifiers (0xE0 to
 * 0xE7) are always part of the report. */
static inline void aoahid_keyboard_options_init(aoahid_keyboard_options* options) {
    aoahid_struct_init(options, sizeof(*options));
    if (options != NULL) {
        options->usage_minimum = 0x04U;
        options->usage_maximum = 0xDDU;
    }
}

/* Five buttons, 16-bit X and Y, and an 8-bit wheel and pan. */
static inline void aoahid_mouse_options_init(aoahid_mouse_options* options) {
    aoahid_struct_init(options, sizeof(*options));
    if (options != NULL) {
        options->button_count = 5U;
        options->x = aoahid_integer_field_make(-32767, 32767, 16U);
        options->y = options->x;
        options->enable_wheel = 1U;
        options->wheel = aoahid_integer_field_make(-127, 127, 8U);
        options->enable_pan = 1U;
        options->pan = options->wheel;
    }
}

/* A Consumer Control (0x0C/0x01) collection of Consumer page keys. The keys themselves
 * (allowed_usages and the three arrays next to it) are the caller's to list. */
static inline void aoahid_toggle_options_init(aoahid_toggle_options* options) {
    aoahid_struct_init(options, sizeof(*options));
    if (options != NULL) {
        options->application_page = 0x0CU;
        options->application_usage = 0x01U;
        options->field_page = 0x0CU;
    }
}

/* Two sticks (X/Y and Z/Rz, 8 bits, at rest at 0), twelve buttons from Button 1, and an eight-way
 * Hat: the layout Android maps to AXIS_X, AXIS_Y, AXIS_Z, AXIS_RZ and the D-pad. */
static inline void aoahid_gamepad_options_init(aoahid_gamepad_options* options) {
    static const aoahid_gamepad_axis_option axes[4] = {
        {AOAHID_AXIS_X, 0x01U, 0x30U, {-127, 127, 8U, {0U, 0, 0, 0, 0U}}, 0, "ABS_X", "AXIS_X"},
        {AOAHID_AXIS_Y, 0x01U, 0x31U, {-127, 127, 8U, {0U, 0, 0, 0, 0U}}, 0, "ABS_Y", "AXIS_Y"},
        {AOAHID_AXIS_Z, 0x01U, 0x32U, {-127, 127, 8U, {0U, 0, 0, 0, 0U}}, 0, "ABS_Z", "AXIS_Z"},
        {AOAHID_AXIS_RZ, 0x01U, 0x35U, {-127, 127, 8U, {0U, 0, 0, 0, 0U}}, 0, "ABS_RZ", "AXIS_RZ"}};
    aoahid_struct_init(options, sizeof(*options));
    if (options != NULL) {
        options->axes = axes;
        options->axis_count = 4U;
        options->button_count = 12U;
        options->button_usage_minimum = 1U;
        options->dpad_representation = AOAHID_DPAD_HAT;
        options->hat_logical_minimum = 0;
        options->hat_logical_maximum = 7;
        options->hat_bit_width = 4U;
    }
}

/* Ten contacts in one report, X and Y from 0 to 32767, and no optional field. */
static inline void aoahid_touchscreen_options_init(aoahid_touchscreen_options* options) {
    aoahid_struct_init(options, sizeof(*options));
    if (options != NULL) {
        options->maximum_contacts = 10U;
        options->contacts_per_report = 10U;
        options->contact_id = aoahid_integer_field_make(0, 15, 4U);
        options->x = aoahid_integer_field_make(0, 32767, 16U);
        options->y = options->x;
        options->contact_count = aoahid_integer_field_make(0, 10, 4U);
    }
}

/* Five contacts in one report, X and Y from 0 to 32767, no optional field, and one click button. */
static inline void aoahid_touchpad_options_init(aoahid_touchpad_options* options) {
    aoahid_struct_init(options, sizeof(*options));
    if (options != NULL) {
        options->maximum_contacts = 5U;
        options->contacts_per_report = 5U;
        options->contact_id = aoahid_integer_field_make(0, 15, 4U);
        options->x = aoahid_integer_field_make(0, 32767, 16U);
        options->y = options->x;
        options->contact_count = aoahid_integer_field_make(0, 5, 4U);
        options->button_count = 1U;
    }
}

/* X and Y from 0 to 32767, 12-bit pressure, one Barrel Switch, and hover. */
static inline void aoahid_pen_options_init(aoahid_pen_options* options) {
    static const uint16_t barrel_usages[1] = {0x44U};
    aoahid_struct_init(options, sizeof(*options));
    if (options != NULL) {
        options->x = aoahid_integer_field_make(0, 32767, 16U);
        options->y = options->x;
        options->enable_pressure = 1U;
        options->pressure = aoahid_integer_field_make(0, 4095, 12U);
        options->barrel_usages = barrel_usages;
        options->barrel_usage_count = 1U;
        options->enable_hover = 1U;
    }
}

/* The same values as aoahid_pen_options_init. */
static inline void aoahid_digitizer_options_init(aoahid_digitizer_options* options) {
    static const uint16_t barrel_usages[1] = {0x44U};
    aoahid_struct_init(options, sizeof(*options));
    if (options != NULL) {
        options->x = aoahid_integer_field_make(0, 32767, 16U);
        options->y = options->x;
        options->enable_pressure = 1U;
        options->pressure = aoahid_integer_field_make(0, 4095, 12U);
        options->barrel_usages = barrel_usages;
        options->barrel_usage_count = 1U;
        options->enable_hover = 1U;
    }
}

/* Only struct_size: the descriptor and its report table are the caller's. */
static inline void aoahid_raw_options_init(aoahid_raw_options* options) {
    aoahid_struct_init(options, sizeof(*options));
}

static inline void aoahid_capability_manifest_init(aoahid_capability_manifest* manifest) {
    aoahid_struct_init(manifest, sizeof(*manifest));
}

/* ---- Diagnostics ------------------------------------------------------------------------ */

/* aoahid_last_error
 * Ownership: Returns the calling thread's record, valid until that thread's next call.
 * Blocking: None.
 * Synchronization: The record belongs to the calling thread; do not pass it to another.
 * Returns: Never null. Results of report transfers completed on the event thread are not here;
 * they come back from a later call (see "Results" above), AOAHID_OK and AOAHID_ERR_* alike. */
AOAHID_API const aoahid_error_detail* AOAHID_CALL aoahid_last_error(void);

/* aoahid_result_name
 * Ownership: Returns a string that lives as long as the process.
 * Blocking: None.
 * Synchronization: Any thread.
 * Returns: The constant's name, such as "AOAHID_OK" or "AOAHID_ERR_STALL", or
 * "AOAHID_RESULT_UNKNOWN" (with AOAHID_ERR_PARAM recorded) for a value that is not a result. */
AOAHID_API const char* AOAHID_CALL aoahid_result_name(aoahid_result result);

/* aoahid_version
 * Ownership: Nothing.
 * Blocking: None.
 * Synchronization: Any thread.
 * Returns: (major << 24) | (minor << 16) | patch of the loaded library; AOAHID_OK and the
 * AOAHID_ERR_* codes do not apply. */
AOAHID_API uint32_t AOAHID_CALL aoahid_version(void);

/* ---- Context ---------------------------------------------------------------------------- */

/* aoahid_context_create
 * Ownership: Keeps log_sink and log_user until the Context is destroyed.
 * Blocking: Initializes libusb and, in AOAHID_EVENT_INTERNAL_THREAD mode, starts the event thread.
 * Sends nothing to any device.
 * Synchronization: Any thread; the new Context is a domain of its own.
 * Returns: AOAHID_OK; AOAHID_ERR_PARAM for a null pointer or a wrong struct_size;
 * AOAHID_ERR_UNSET_FIELD for an unknown event mode or log level, or a log level without a sink;
 * a transport error when libusb cannot start. */
AOAHID_API aoahid_result AOAHID_CALL aoahid_context_create(const aoahid_context_options* options,
                                                           aoahid_context** out_context);

/* aoahid_context_poll
 * Ownership: Nothing.
 * Blocking: Handles libusb events for up to timeout_ms (zero only handles what is ready). Report
 * completions and Device teardowns the Context finishes run inside this call.
 * Synchronization: Context domain; AOAHID_EVENT_CALLER_POLL only.
 * Returns: AOAHID_OK; AOAHID_ERR_PARAM for a null Context; AOAHID_ERR_UNSUPPORTED in
 * AOAHID_EVENT_INTERNAL_THREAD mode; a transport error from libusb or an earlier failure the
 * Context kept, such as one of a Device teardown it finished. */
AOAHID_API aoahid_result AOAHID_CALL aoahid_context_poll(aoahid_context* context,
                                                         uint32_t timeout_ms);

/* aoahid_context_destroy
 * Ownership: Closes every open Device as aoahid_device_close does, then frees the Context. Follows
 * the "Closing" rule: AOAHID_CLOSE_PENDING while a Device teardown is still waiting for a transfer
 * callback.
 * Blocking: control_timeout_ms for each AOA request and drain_timeout_ms for each Device's
 * reports, then joins the event thread. It never waits for a callback past these.
 * Synchronization: Context domain; not from the log sink.
 * Returns: AOAHID_OK; AOAHID_ERR_PARAM for a null Context; AOAHID_ERR_UNSET_FIELD for a zero
 * timeout; AOAHID_CLOSE_PENDING; otherwise the first error of the teardown, and the Context is
 * gone. */
AOAHID_API aoahid_result AOAHID_CALL aoahid_context_destroy(aoahid_context* context,
                                                            uint32_t control_timeout_ms,
                                                            uint32_t drain_timeout_ms);

/* aoahid_context_destroy_blocking
 * Ownership: As aoahid_context_destroy, retried until it no longer returns AOAHID_CLOSE_PENDING
 * or timeout_ms has passed.
 * Blocking: Up to timeout_ms, handling libusb events itself when no thread does.
 * Synchronization: Context domain; not from the event thread or the log sink.
 * Returns: AOAHID_OK; AOAHID_ERR_PARAM for a null Context; AOAHID_ERR_UNSET_FIELD for a zero
 * timeout; AOAHID_CLOSE_PENDING when timeout_ms passed first (the Context is still the caller's);
 * otherwise as aoahid_context_destroy. */
AOAHID_API aoahid_result AOAHID_CALL aoahid_context_destroy_blocking(aoahid_context* context,
                                                                     uint32_t control_timeout_ms,
                                                                     uint32_t drain_timeout_ms,
                                                                     uint32_t timeout_ms);

/* ---- Discovery and accessory mode ------------------------------------------------------- */

/* aoahid_discover
 * Ownership: Returns a snapshot that owns every entry and string in it; free it with
 * aoahid_discovery_destroy.
 * Blocking: Sends AOA request 51 to every USB device, waiting up to control_timeout_ms for each.
 * Synchronization: Context domain. The snapshot is outside it.
 * Returns: AOAHID_OK, with only the devices that answered a nonzero AOA version (HID needs 2);
 * AOAHID_ERR_PARAM for a null argument; AOAHID_ERR_UNSET_FIELD for a zero timeout; a transport
 * error from enumeration. */
AOAHID_API aoahid_result AOAHID_CALL aoahid_discover(aoahid_context* context,
                                                     uint32_t control_timeout_ms,
                                                     aoahid_discovery** out_discovery);

/* aoahid_discovery_count
 * Ownership: Nothing.
 * Blocking: None.
 * Synchronization: Any thread, but not during aoahid_discovery_destroy.
 * Returns: The number of entries. Zero is also returned for a null snapshot, with
 * AOAHID_ERR_PARAM recorded; AOAHID_OK is recorded otherwise. */
AOAHID_API size_t AOAHID_CALL aoahid_discovery_count(const aoahid_discovery* discovery);

/* aoahid_discovery_get
 * Ownership: The entry and its strings belong to the snapshot.
 * Blocking: None.
 * Synchronization: Any thread, but not during aoahid_discovery_destroy.
 * Returns: The entry with AOAHID_OK recorded, or null with AOAHID_ERR_PARAM recorded for a null
 * snapshot or an index out of range. */
AOAHID_API const aoahid_device_info* AOAHID_CALL
aoahid_discovery_get(const aoahid_discovery* discovery, size_t index);

/* aoahid_discovery_destroy
 * Ownership: Frees the snapshot and every entry it returned.
 * Blocking: None.
 * Synchronization: Not during any other call on the snapshot.
 * Returns: Nothing. AOAHID_OK is recorded, or AOAHID_ERR_PARAM for null (nothing is done). */
AOAHID_API void AOAHID_CALL aoahid_discovery_destroy(aoahid_discovery* discovery);

/* aoahid_accessory_start
 * Ownership: Nothing is kept.
 * Blocking: Sends AOA request 51, request 52 for each non-null string and request 53 (start), each
 * waiting up to control_timeout_ms, and returns once request 53 completes. The device then
 * disconnects and comes back in accessory mode (VID 0x18D1, PID 0x2D00 to 0x2D05); find it again
 * with aoahid_discover. A device already in accessory mode gets no request.
 * Synchronization: Context domain.
 * Returns: AOAHID_OK; AOAHID_ERR_PARAM for a null argument or a wrong struct_size;
 * AOAHID_ERR_UNSET_FIELD for a zero timeout, a missing manufacturer or model, or a nonzero
 * reserved; AOAHID_ERR_NOT_AOA when request 51 shows no AOA support; AOAHID_ERR_OVERFLOW, before
 * any request, for a string over 256 bytes or a port path over 7 ports; a transport error from
 * the requests (after a failed request 53 the device may still have switched). */
AOAHID_API aoahid_result AOAHID_CALL aoahid_accessory_start(aoahid_context* context,
                                                            const aoahid_device_info* selected,
                                                            const aoahid_accessory_options* options,
                                                            uint32_t control_timeout_ms);

/* ---- Device ----------------------------------------------------------------------------- */

/* aoahid_device_open
 * Ownership: Copies selected and options. The Device belongs to the caller and to the Context,
 * which closes it if it is still open when the Context is destroyed.
 * Blocking: Opens the USB device as it is, claims interface_number if asked, and allocates the
 * transfer pool. Sends no AOA request: a device without AOA 2.0 HID opens and then fails at
 * aoahid_node_open with AOAHID_ERR_STALL.
 * Synchronization: Context domain.
 * Returns: AOAHID_OK; AOAHID_ERR_PARAM for a null argument, a wrong struct_size or an invalid
 * value; AOAHID_ERR_UNSET_FIELD for a zero required field; AOAHID_ERR_OVERFLOW for a size above
 * one of the byte policies or a port path over 7 ports; a transport error from opening or
 * claiming. */
AOAHID_API aoahid_result AOAHID_CALL aoahid_device_open(aoahid_context* context,
                                                        const aoahid_device_info* selected,
                                                        const aoahid_device_options* options,
                                                        aoahid_device** out_device);

/* aoahid_device_close
 * Ownership: Closes every Node (as aoahid_node_close does) and Channel of the Device, then the
 * Device. It never returns AOAHID_CLOSE_PENDING: when a transfer is still in flight at the drain
 * deadline, the Context finishes the teardown and reports its errors from aoahid_context_poll or
 * aoahid_context_destroy.
 * Blocking: drain_timeout_ms for all reports together, control_timeout_ms for each request 55.
 * Synchronization: Context domain; not from the log sink.
 * Returns: AOAHID_OK; AOAHID_ERR_PARAM for a null Device or one already closed;
 * AOAHID_ERR_UNSET_FIELD for a zero timeout; otherwise the first error of the teardown. */
AOAHID_API aoahid_result AOAHID_CALL aoahid_device_close(aoahid_device* device,
                                                         uint32_t control_timeout_ms,
                                                         uint32_t drain_timeout_ms);

/* aoahid_device_latched_error
 * Ownership: Clears the error it returns, except AOAHID_ERR_NO_DEVICE, which stays.
 * Blocking: None.
 * Synchronization: Context domain. The event thread may record an error at the same time.
 * Returns: AOAHID_OK when no error is waiting; AOAHID_ERR_PARAM for an invalid Device; otherwise
 * the error a report transfer, control request or event left on the Device or its Context. */
AOAHID_API aoahid_result AOAHID_CALL aoahid_device_latched_error(aoahid_device* device);

/* ---- Channel ---------------------------------------------------------------------------- */

/* aoahid_channel_open
 * Ownership: The Channel shares the Device's USB handle; aoahid_device_close closes it too.
 * Blocking: Waits up to drain_timeout_ms for report transfers in flight, selects configuration 1
 * if the device has none (AOA 1.0), claims the interface, and submits the read-ahead transfers.
 * Synchronization: Context domain.
 * Returns: AOAHID_OK; AOAHID_ERR_PARAM for a null argument, a wrong struct_size or a closing
 * Device; AOAHID_ERR_UNSET_FIELD for a zero required field or timeout, a nonzero reserved field, or
 * a flag other than 0 or 1; AOAHID_ERR_UNSUPPORTED when no interface matches; AOAHID_ERR_TIMEOUT
 * when report transfers were still in flight; AOAHID_ERR_OVERFLOW for transfer_bytes above the
 * libusb limit; a transport error from claiming or submitting. */
AOAHID_API aoahid_result AOAHID_CALL aoahid_channel_open(aoahid_device* device,
                                                         const aoahid_channel_options* options,
                                                         uint32_t drain_timeout_ms,
                                                         aoahid_channel** out_channel);

/* aoahid_channel_close
 * Ownership: Follows the "Closing" rule: AOAHID_CLOSE_PENDING while a cancelled transfer has not
 * completed.
 * Blocking: Cancels every transfer and waits up to drain_timeout_ms for them, then within the same
 * time for report transfers in flight, and releases the interface.
 * Synchronization: Context domain; never during a read or write on the Channel.
 * Returns: AOAHID_OK; AOAHID_ERR_PARAM for a null Channel; AOAHID_ERR_UNSET_FIELD for a zero
 * timeout; AOAHID_CLOSE_PENDING; a transport error from handling libusb events. */
AOAHID_API aoahid_result AOAHID_CALL aoahid_channel_close(aoahid_channel* channel,
                                                          uint32_t drain_timeout_ms);

/* aoahid_channel_write
 * Ownership: Copies data. out_written is set on every return.
 * Blocking: Waits up to timeout_ms (zero does not wait) for free OUT transfers and returns once
 * every byte is submitted. It does not wait for completion and never retries.
 * Synchronization: In AOAHID_EVENT_INTERNAL_THREAD mode one thread may write while another reads,
 * alongside other calls on the Context, but not while the Channel, its Device or the Context is
 * closing. In AOAHID_EVENT_CALLER_POLL mode, Context domain.
 * Returns: AOAHID_OK when every byte was queued; AOAHID_ERR_PARAM for a null pointer;
 * AOAHID_ERR_TIMEOUT when the transfers stayed busy; AOAHID_ERR_NO_DEVICE once the Channel is
 * lost; a transport error of an earlier write, returned once. */
AOAHID_API aoahid_result AOAHID_CALL aoahid_channel_write(aoahid_channel* channel,
                                                          const uint8_t* data, size_t length,
                                                          size_t* out_written, uint32_t timeout_ms);

/* aoahid_channel_read
 * Ownership: out_received is set on every return.
 * Blocking: Waits up to timeout_ms (zero does not wait) for data. A call may return part of a
 * transfer or stop at a transfer boundary. In AOAHID_CHANNEL_READ_REQUEST mode a read with nothing
 * buffered submits one transfer sized from capacity; a timeout leaves it submitted and the next
 * read waits for it, so nothing is lost. Zero-length packets are skipped.
 * Synchronization: As aoahid_channel_write; one reader at a time.
 * Returns: AOAHID_OK with at least one byte; AOAHID_ERR_PARAM for a null pointer or a zero
 * capacity; AOAHID_ERR_TIMEOUT when nothing arrived; AOAHID_ERR_NO_DEVICE once the Channel is
 * lost; a transport error of a failed IN transfer, which also loses the Channel. */
AOAHID_API aoahid_result AOAHID_CALL aoahid_channel_read(aoahid_channel* channel, uint8_t* buffer,
                                                         size_t capacity, size_t* out_received,
                                                         uint32_t timeout_ms);

/* ---- Spec -------------------------------------------------------------------------------
 * The factories copy their options and every array and string the options point to. The Spec
 * holds one reference for the caller. All factories return:
 *   AOAHID_OK; AOAHID_ERR_PARAM for a null pointer, a wrong struct_size or a value out of range;
 *   AOAHID_ERR_UNSET_FIELD for a required field left zero; AOAHID_ERR_OVERFLOW when the descriptor
 *   or report would be too large; the factory's own results listed with it. */

/* aoahid_spec_create_keyboard
 * Ownership: As every factory.
 * Blocking: None.
 * Synchronization: Any thread.
 * Returns: As every factory, AOAHID_OK through AOAHID_ERR_OVERFLOW; AOAHID_ERR_PARAM also for a
 * range that includes 0x00 to 0x03 or the modifiers. */
AOAHID_API aoahid_result AOAHID_CALL
aoahid_spec_create_keyboard(const aoahid_keyboard_options* options, aoahid_spec** out_spec);

/* aoahid_spec_create_mouse
 * Ownership: As every factory.
 * Blocking: None.
 * Synchronization: Any thread.
 * Returns: As every factory, AOAHID_OK through AOAHID_ERR_OVERFLOW. */
AOAHID_API aoahid_result AOAHID_CALL aoahid_spec_create_mouse(const aoahid_mouse_options* options,
                                                              aoahid_spec** out_spec);

/* aoahid_spec_create_toggle
 * Ownership: As every factory.
 * Blocking: None.
 * Synchronization: Any thread.
 * Returns: As every factory, AOAHID_OK through AOAHID_ERR_OVERFLOW; AOAHID_ERR_UNSUPPORTED for a
 * semantic that press and release cannot express, or a Camera Control Usage that is not
 * AOAHID_USAGE_ONE_SHOT. */
AOAHID_API aoahid_result AOAHID_CALL aoahid_spec_create_toggle(const aoahid_toggle_options* options,
                                                               aoahid_spec** out_spec);

/* aoahid_spec_create_gamepad
 * Ownership: As every factory.
 * Blocking: None.
 * Synchronization: Any thread.
 * Returns: As every factory, AOAHID_OK through AOAHID_ERR_OVERFLOW. */
AOAHID_API aoahid_result AOAHID_CALL
aoahid_spec_create_gamepad(const aoahid_gamepad_options* options, aoahid_spec** out_spec);

/* aoahid_spec_create_touchscreen
 * Ownership: As every factory.
 * Blocking: None.
 * Synchronization: Any thread.
 * Returns: As every factory, AOAHID_OK through AOAHID_ERR_OVERFLOW. */
AOAHID_API aoahid_result AOAHID_CALL
aoahid_spec_create_touchscreen(const aoahid_touchscreen_options* options, aoahid_spec** out_spec);

/* aoahid_spec_create_touchpad
 * Ownership: As every factory.
 * Blocking: None.
 * Synchronization: Any thread.
 * Returns: As every factory, AOAHID_OK through AOAHID_ERR_OVERFLOW. */
AOAHID_API aoahid_result AOAHID_CALL
aoahid_spec_create_touchpad(const aoahid_touchpad_options* options, aoahid_spec** out_spec);

/* aoahid_spec_create_pen
 * Ownership: As every factory.
 * Blocking: None.
 * Synchronization: Any thread.
 * Returns: As every factory, AOAHID_OK through AOAHID_ERR_OVERFLOW. */
AOAHID_API aoahid_result AOAHID_CALL aoahid_spec_create_pen(const aoahid_pen_options* options,
                                                            aoahid_spec** out_spec);

/* aoahid_spec_create_digitizer
 * Ownership: As every factory.
 * Blocking: None.
 * Synchronization: Any thread.
 * Returns: As every factory, AOAHID_OK through AOAHID_ERR_OVERFLOW. */
AOAHID_API aoahid_result AOAHID_CALL
aoahid_spec_create_digitizer(const aoahid_digitizer_options* options, aoahid_spec** out_spec);

/* aoahid_spec_create_raw
 * Ownership: As every factory.
 * Blocking: None.
 * Synchronization: Any thread.
 * Returns: As every factory, AOAHID_OK through AOAHID_ERR_OVERFLOW; AOAHID_ERR_PARAM also for a
 * descriptor that does not parse or does not match the report table; AOAHID_ERR_UNSUPPORTED
 * without the acknowledgement, with requires_output or requires_feature_response set, or for a
 * long item or a construct the parser does not handle. */
AOAHID_API aoahid_result AOAHID_CALL aoahid_spec_create_raw(const aoahid_raw_options* options,
                                                            aoahid_spec** out_spec);

/* aoahid_spec_retain
 * Ownership: Adds a reference for the caller, who must already hold one.
 * Blocking: None.
 * Synchronization: Any thread.
 * Returns: Nothing. AOAHID_OK is recorded; AOAHID_ERR_PARAM for null, or AOAHID_ERR_OVERFLOW when
 * the count is full (no reference is added). */
AOAHID_API void AOAHID_CALL aoahid_spec_retain(aoahid_spec* spec);

/* aoahid_spec_release
 * Ownership: Gives up one of the caller's references; the last one frees the Spec. A Node keeps
 * its own reference until it closes.
 * Blocking: None.
 * Synchronization: Any thread, as long as the last release does not race any other use.
 * Returns: Nothing. AOAHID_OK is recorded, or AOAHID_ERR_PARAM for null (nothing is done). */
AOAHID_API void AOAHID_CALL aoahid_spec_release(aoahid_spec* spec);

/* aoahid_spec_descriptor
 * Ownership: The bytes belong to the Spec.
 * Blocking: None.
 * Synchronization: Any thread.
 * Returns: AOAHID_OK; AOAHID_ERR_PARAM for a null pointer. */
AOAHID_API aoahid_result AOAHID_CALL aoahid_spec_descriptor(const aoahid_spec* spec,
                                                            const uint8_t** bytes, size_t* length);

/* aoahid_spec_manifest
 * Ownership: Fills the caller's manifest; manifest->reports belongs to the Spec.
 * Blocking: None.
 * Synchronization: Any thread.
 * Returns: AOAHID_OK; AOAHID_ERR_PARAM for a null pointer, a wrong struct_size or a nonzero
 * reserved. */
AOAHID_API aoahid_result AOAHID_CALL aoahid_spec_manifest(const aoahid_spec* spec,
                                                          aoahid_capability_manifest* manifest);

/* ---- Node -------------------------------------------------------------------------------- */

/* aoahid_node_open
 * Ownership: The Node keeps a reference to spec until it closes.
 * Blocking: Sends request 54 (register) and the descriptor in request 56 fragments, each waiting
 * up to control_timeout_ms. Android registers the HID after the last fragment, without telling the
 * host: a report sent at once may STALL or be lost, so wait a little before the first one
 * (docs/API.md).
 * Synchronization: Context domain.
 * Returns: AOAHID_OK; AOAHID_ERR_PARAM for a null argument, a wrong struct_size or a reservation
 * that leaves no shared slot; AOAHID_ERR_UNSET_FIELD for a zero timeout or a reservation that is
 * not 0/0 or 1/0..1; AOAHID_ERR_OVERFLOW when the Spec exceeds a policy of the Device or no AOA
 * HID ID is left; a transport error from the requests (AOAHID_ERR_STALL: the device has no AOA 2.0
 * HID). */
AOAHID_API aoahid_result AOAHID_CALL aoahid_node_open(aoahid_device* device, aoahid_spec* spec,
                                                      const aoahid_node_options* options,
                                                      uint32_t control_timeout_ms,
                                                      aoahid_node** out_node);

/* aoahid_node_close
 * Ownership: Follows the "Closing" rule: AOAHID_CLOSE_PENDING while a report of the Node is still
 * in flight at the drain deadline.
 * Blocking: Finishes a touch frame that has started, sends the neutral report (keys and buttons
 * up, contacts lifted, axes at rest) within drain_timeout_ms, then request 55 (unregister) within
 * control_timeout_ms. A failed step does not stop the next one.
 * Synchronization: Context domain; not from the log sink.
 * Returns: AOAHID_OK; AOAHID_ERR_PARAM for a null or closed Node; AOAHID_ERR_UNSET_FIELD for a zero
 * timeout; AOAHID_CLOSE_PENDING; otherwise the first error on the way out, such as a report that
 * failed earlier or a refused request 55. */
AOAHID_API aoahid_result AOAHID_CALL aoahid_node_close(aoahid_node* node,
                                                       uint32_t control_timeout_ms,
                                                       uint32_t drain_timeout_ms);

/* aoahid_node_id
 * Ownership: Nothing.
 * Blocking: None.
 * Synchronization: Context domain.
 * Returns: The Node's AOA HID ID (nonzero) with AOAHID_OK recorded, or 0 with AOAHID_ERR_PARAM
 * recorded for a null or closed Node. */
AOAHID_API uint16_t AOAHID_CALL aoahid_node_id(const aoahid_node* node);

/* aoahid_node_manifest
 * Ownership: As aoahid_spec_manifest; manifest->reports is valid while the Node is open.
 * Blocking: None.
 * Synchronization: Context domain.
 * Returns: AOAHID_OK; AOAHID_ERR_PARAM for a null pointer, a wrong struct_size or a nonzero
 * reserved. */
AOAHID_API aoahid_result AOAHID_CALL aoahid_node_manifest(const aoahid_node* node,
                                                          aoahid_capability_manifest* manifest);

/* aoahid_node_submit
 * Ownership: The report is built in the library's own transfer buffer.
 * Blocking: Queues one report of the Node's current state, if it changed, and returns. In
 * AOAHID_EVENT_CALLER_POLL mode it then handles ready libusb events once without waiting.
 * send_timeout_ms is the deadline of that transfer; its failure comes back from a later call.
 * Synchronization: Context domain.
 * Returns: AOAHID_OK, also when there was nothing to send; AOAHID_ERR_PARAM for an invalid Node;
 * AOAHID_ERR_UNSET_FIELD for a zero timeout; AOAHID_ERR_UNSUPPORTED for a Raw Node (use
 * aoahid_raw_submit); AOAHID_ERR_BUSY when the Node's report is still in flight or the pool is
 * full; a transport error of this or an earlier transfer. */
AOAHID_API aoahid_result AOAHID_CALL aoahid_node_submit(aoahid_node* node,
                                                        uint32_t send_timeout_ms);

/* aoahid_node_submit_blocking
 * Ownership: As aoahid_node_submit.
 * Blocking: Sends reports until the Node's state has been delivered, a mouse's pending movement
 * included, or timeout_ms has passed. Each report's deadline is what is left of timeout_ms. In
 * AOAHID_EVENT_CALLER_POLL mode this call handles libusb events.
 * Synchronization: Context domain.
 * Returns: AOAHID_OK; AOAHID_ERR_PARAM for an invalid Node; AOAHID_ERR_UNSET_FIELD for a zero
 * timeout; AOAHID_ERR_TIMEOUT when timeout_ms passed first; a transport error of a transfer. */
AOAHID_API aoahid_result AOAHID_CALL aoahid_node_submit_blocking(aoahid_node* node,
                                                                 uint32_t timeout_ms);

/* ---- Input ------------------------------------------------------------------------------
 * These calls change a Node's state; aoahid_node_submit sends it. None of them blocks, allocates
 * or does I/O. Each belongs to the Context domain and works only on a Node of its own profile.
 * A press and the release after it are never merged: until the report carrying the press has
 * completed, the release is refused with AOAHID_ERR_BUSY, so a quick tap is not lost. Every input
 * call returns:
 *   AOAHID_OK; AOAHID_ERR_PARAM for a Node of another profile or a value out of range;
 *   AOAHID_ERR_BUSY while the Node's report is in flight or an opposite edge is unsent;
 *   AOAHID_ERR_NO_DEVICE once the device is gone; the call's own results listed with it. */

/* aoahid_keyboard_key
 * Ownership: Nothing.
 * Blocking: None.
 * Synchronization: Context domain.
 * Presses (down 1) or releases (down 0) one Keyboard page Usage, a modifier or a key of the Spec's
 * range. Closing the Node releases every key still down.
 * Returns: As every input call, AOAHID_OK through AOAHID_ERR_NO_DEVICE. */
AOAHID_API aoahid_result AOAHID_CALL aoahid_keyboard_key(aoahid_node* node, uint16_t usage,
                                                         uint32_t down);

/* aoahid_mouse_move
 * Ownership: Nothing.
 * Blocking: None.
 * Synchronization: Context domain.
 * Adds to the pending X and Y movement. A report carries as much as the fields hold and keeps the
 * rest for the next one.
 * Returns: As every input call, AOAHID_OK through AOAHID_ERR_NO_DEVICE; AOAHID_ERR_OVERFLOW when
 * the 64-bit pending total overflows. */
AOAHID_API aoahid_result AOAHID_CALL aoahid_mouse_move(aoahid_node* node, int32_t dx, int32_t dy);

/* aoahid_mouse_scroll
 * Ownership: Nothing.
 * Blocking: None.
 * Synchronization: Context domain.
 * Adds to the pending wheel and pan movement, as aoahid_mouse_move does.
 * Returns: As every input call, AOAHID_OK through AOAHID_ERR_NO_DEVICE; AOAHID_ERR_UNSUPPORTED
 * for a nonzero value on a field the Spec lacks; AOAHID_ERR_OVERFLOW when the 64-bit pending total
 * overflows. */
AOAHID_API aoahid_result AOAHID_CALL aoahid_mouse_scroll(aoahid_node* node, int32_t wheel,
                                                         int32_t pan);

/* aoahid_mouse_button
 * Ownership: Nothing.
 * Blocking: None.
 * Synchronization: Context domain.
 * Presses (down 1) or releases (down 0) button 1 to button_count.
 * Returns: As every input call, AOAHID_OK through AOAHID_ERR_NO_DEVICE. */
AOAHID_API aoahid_result AOAHID_CALL aoahid_mouse_button(aoahid_node* node, uint32_t button,
                                                         uint32_t down);

/* aoahid_toggle_usage
 * Ownership: Nothing.
 * Blocking: None.
 * Synchronization: Context domain.
 * Presses (down 1) or releases (down 0) one Usage of the Spec's allow-list; any number may be
 * pressed at once. Closing the Node releases every Usage still down.
 * Returns: As every input call, AOAHID_OK through AOAHID_ERR_NO_DEVICE. */
AOAHID_API aoahid_result AOAHID_CALL aoahid_toggle_usage(aoahid_node* node, uint16_t usage,
                                                         uint32_t down);

/* aoahid_gamepad_button
 * Ownership: Nothing.
 * Blocking: None.
 * Synchronization: Context domain.
 * Presses (down 1) or releases (down 0) button 1 to button_count.
 * Returns: As every input call, AOAHID_OK through AOAHID_ERR_NO_DEVICE. */
AOAHID_API aoahid_result AOAHID_CALL aoahid_gamepad_button(aoahid_node* node, uint32_t button,
                                                           uint32_t down);

/* aoahid_gamepad_axis
 * Ownership: Nothing.
 * Blocking: None.
 * Synchronization: Context domain.
 * Sets axes[axis_index] of the Spec to value, which must be in its logical range.
 * Returns: As every input call, AOAHID_OK through AOAHID_ERR_NO_DEVICE. */
AOAHID_API aoahid_result AOAHID_CALL aoahid_gamepad_axis(aoahid_node* node, size_t axis_index,
                                                         int32_t value);

/* aoahid_gamepad_dpad
 * Ownership: Nothing.
 * Blocking: None.
 * Synchronization: Context domain.
 * Sets the D-pad from four 0/1 directions. With a Hat, up and down (or left and right) together
 * are refused and no direction sends the Null state; with four buttons the bits go out as given.
 * Returns: As every input call, AOAHID_OK through AOAHID_ERR_NO_DEVICE; AOAHID_ERR_PARAM also
 * when the Spec has no D-pad. */
AOAHID_API aoahid_result AOAHID_CALL aoahid_gamepad_dpad(aoahid_node* node, uint32_t up,
                                                         uint32_t down, uint32_t right,
                                                         uint32_t left);

/* aoahid_touchscreen_contact
 * Ownership: Copies extra, which may be null (every optional field 0).
 * Blocking: None.
 * Synchronization: Context domain.
 * down 1 places contact_id in a free slot, or moves it if it is already down; down 0 lifts it at
 * x, y. A lifted contact_id can be placed again once its lift has been sent. While a frame split
 * over several reports is being sent, every call is refused with AOAHID_ERR_BUSY.
 * Returns: As every input call, AOAHID_OK through AOAHID_ERR_NO_DEVICE; AOAHID_ERR_PARAM also for
 * lifting a contact_id that is not down; AOAHID_ERR_BUSY also for placing one whose lift is
 * unsent; AOAHID_ERR_OVERFLOW when every slot is taken. */
AOAHID_API aoahid_result AOAHID_CALL
aoahid_touchscreen_contact(aoahid_node* node, uint32_t contact_id, uint32_t down, int32_t x,
                           int32_t y, const aoahid_touchscreen_extra* extra);

/* aoahid_touchpad_contact
 * Ownership: Copies extra, which may be null.
 * Blocking: None.
 * Synchronization: Context domain.
 * As aoahid_touchscreen_contact, for a Touchpad Node.
 * Returns: As aoahid_touchscreen_contact, AOAHID_OK through AOAHID_ERR_OVERFLOW. */
AOAHID_API aoahid_result AOAHID_CALL aoahid_touchpad_contact(aoahid_node* node, uint32_t contact_id,
                                                             uint32_t down, int32_t x, int32_t y,
                                                             const aoahid_touchpad_extra* extra);

/* aoahid_touchpad_button
 * Ownership: Nothing.
 * Blocking: None.
 * Synchronization: Context domain.
 * Presses (down 1) or releases (down 0) click button 1 to button_count.
 * Returns: As every input call, AOAHID_OK through AOAHID_ERR_NO_DEVICE. */
AOAHID_API aoahid_result AOAHID_CALL aoahid_touchpad_button(aoahid_node* node, uint32_t button,
                                                            uint32_t down);

/* aoahid_pen_update
 * Ownership: Copies sample.
 * Blocking: None.
 * Synchronization: Context domain.
 * Sets the whole pen state. in_range 0 takes the pen away: In Range, Tip, pressure, Invert and the
 * barrel bits go out as 0 with the sample's own X, Y, tilt and twist, so repeat the last position
 * to leave the pointer where it was. Switching between tip and eraser while in range first sends
 * an out-of-range report.
 * Returns: As every input call, AOAHID_OK through AOAHID_ERR_NO_DEVICE. */
AOAHID_API aoahid_result AOAHID_CALL aoahid_pen_update(aoahid_node* node,
                                                       const aoahid_pen_sample* sample);

/* aoahid_digitizer_update
 * Ownership: Copies sample.
 * Blocking: None.
 * Synchronization: Context domain.
 * As aoahid_pen_update, for a Digitizer Node.
 * Returns: As every input call, AOAHID_OK through AOAHID_ERR_NO_DEVICE. */
AOAHID_API aoahid_result AOAHID_CALL aoahid_digitizer_update(aoahid_node* node,
                                                             const aoahid_digitizer_sample* sample);

/* aoahid_battery_update
 * Ownership: Nothing.
 * Blocking: None.
 * Synchronization: Context domain.
 * Sets the battery level of a Node whose Spec has enable_battery. It goes out with the next
 * aoahid_node_submit that has no input report to send, and only when it changed. A level not yet
 * sent is dropped on close.
 * Returns: As every input call, AOAHID_OK through AOAHID_ERR_NO_DEVICE; AOAHID_ERR_PARAM also for
 * a Node without a battery report. */
AOAHID_API aoahid_result AOAHID_CALL aoahid_battery_update(aoahid_node* node, int32_t strength);

/* aoahid_raw_submit
 * Ownership: Copies report.
 * Blocking: Queues the report and returns; send_timeout_ms is the deadline of its transfer.
 * Synchronization: Context domain.
 * Returns: AOAHID_OK; AOAHID_ERR_PARAM for a Node that is not Raw, a null report, a Report ID or
 * length the Spec does not accept, or nonzero padding bits; AOAHID_ERR_UNSET_FIELD for a zero
 * timeout; AOAHID_ERR_BUSY while the Node's report is in flight or the pool is full; a transport
 * error of this or an earlier transfer. */
AOAHID_API aoahid_result AOAHID_CALL aoahid_raw_submit(aoahid_node* node, const uint8_t* report,
                                                       size_t length, uint32_t send_timeout_ms);

#ifdef __cplusplus
}
#endif

#endif
