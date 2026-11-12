// SPDX-License-Identifier: MIT
// Copyright (c) 2026 libaoahid contributors
/* Declares the deterministic test-only subset of libusb; production builds
 * must use the official libusb headers and implementation. */
#ifndef AOAHID_FAKE_LIBUSB_H
#define AOAHID_FAKE_LIBUSB_H

/*
 * A deterministic libusb 1.0 subset used only by transport tests. The fake's
 * control API is C-compatible so integration tests need not depend on C++
 * implementation details.
 */

#include <stddef.h>
#include <stdint.h>

#if defined(_WIN32)
#include <BaseTsd.h>
typedef SSIZE_T ssize_t;
struct timeval {
    long tv_sec;
    long tv_usec;
};
#else
#include <sys/time.h>
#include <sys/types.h>
#endif

#if defined(_WIN32)
#define LIBUSB_CALL __cdecl
#else
#define LIBUSB_CALL
#endif

#ifdef __cplusplus
extern "C" {
#endif

#define LIBUSB_CONTROL_SETUP_SIZE 8U
#define LIBUSB_API_VERSION 0x0100010C

enum libusb_error {
    LIBUSB_SUCCESS = 0,
    LIBUSB_ERROR_IO = -1,
    LIBUSB_ERROR_INVALID_PARAM = -2,
    LIBUSB_ERROR_ACCESS = -3,
    LIBUSB_ERROR_NO_DEVICE = -4,
    LIBUSB_ERROR_NOT_FOUND = -5,
    LIBUSB_ERROR_BUSY = -6,
    LIBUSB_ERROR_TIMEOUT = -7,
    LIBUSB_ERROR_OVERFLOW = -8,
    LIBUSB_ERROR_PIPE = -9,
    LIBUSB_ERROR_INTERRUPTED = -10,
    LIBUSB_ERROR_NO_MEM = -11,
    LIBUSB_ERROR_NOT_SUPPORTED = -12,
    LIBUSB_ERROR_OTHER = -99
};

enum libusb_transfer_status {
    LIBUSB_TRANSFER_COMPLETED = 0,
    LIBUSB_TRANSFER_ERROR = 1,
    LIBUSB_TRANSFER_TIMED_OUT = 2,
    LIBUSB_TRANSFER_CANCELLED = 3,
    LIBUSB_TRANSFER_STALL = 4,
    LIBUSB_TRANSFER_NO_DEVICE = 5,
    LIBUSB_TRANSFER_OVERFLOW = 6
};

enum libusb_transfer_type {
    LIBUSB_TRANSFER_TYPE_CONTROL = 0,
    LIBUSB_TRANSFER_TYPE_ISOCHRONOUS = 1,
    LIBUSB_TRANSFER_TYPE_BULK = 2,
    LIBUSB_TRANSFER_TYPE_INTERRUPT = 3,
    LIBUSB_TRANSFER_TYPE_BULK_STREAM = 4
};

#define LIBUSB_ENDPOINT_IN 0x80
#define LIBUSB_TRANSFER_TYPE_MASK 0x03
enum libusb_endpoint_transfer_type { LIBUSB_ENDPOINT_TRANSFER_TYPE_BULK = 0x2 };

typedef struct libusb_context libusb_context;
typedef struct libusb_device libusb_device;
typedef struct libusb_device_handle libusb_device_handle;

enum libusb_log_level {
    LIBUSB_LOG_LEVEL_NONE = 0,
    LIBUSB_LOG_LEVEL_ERROR = 1,
    LIBUSB_LOG_LEVEL_WARNING = 2,
    LIBUSB_LOG_LEVEL_INFO = 3,
    LIBUSB_LOG_LEVEL_DEBUG = 4
};

enum libusb_option {
    LIBUSB_OPTION_LOG_LEVEL = 0,
    LIBUSB_OPTION_USE_USBDK = 1,
    LIBUSB_OPTION_NO_DEVICE_DISCOVERY = 2,
    LIBUSB_OPTION_LOG_CB = 3,
    LIBUSB_OPTION_MAX = 4
};

typedef void(LIBUSB_CALL* libusb_log_cb)(libusb_context* context, enum libusb_log_level level,
                                         const char* message);

typedef struct libusb_init_option {
    enum libusb_option option;
    union {
        int ival;
        libusb_log_cb log_cbval;
    } value;
} libusb_init_option;

typedef struct libusb_version {
    uint16_t major;
    uint16_t minor;
    uint16_t micro;
    uint16_t nano;
    const char* rc;
    const char* describe;
} libusb_version;

typedef struct libusb_device_descriptor {
    uint8_t bLength;
    uint8_t bDescriptorType;
    uint16_t bcdUSB;
    uint8_t bDeviceClass;
    uint8_t bDeviceSubClass;
    uint8_t bDeviceProtocol;
    uint8_t bMaxPacketSize0;
    uint16_t idVendor;
    uint16_t idProduct;
    uint16_t bcdDevice;
    uint8_t iManufacturer;
    uint8_t iProduct;
    uint8_t iSerialNumber;
    uint8_t bNumConfigurations;
} libusb_device_descriptor;

struct libusb_endpoint_descriptor {
    uint8_t bLength;
    uint8_t bDescriptorType;
    uint8_t bEndpointAddress;
    uint8_t bmAttributes;
    uint16_t wMaxPacketSize;
    uint8_t bInterval;
    uint8_t bRefresh;
    uint8_t bSynchAddress;
    const unsigned char* extra;
    int extra_length;
};

struct libusb_interface_descriptor {
    uint8_t bLength;
    uint8_t bDescriptorType;
    uint8_t bInterfaceNumber;
    uint8_t bAlternateSetting;
    uint8_t bNumEndpoints;
    uint8_t bInterfaceClass;
    uint8_t bInterfaceSubClass;
    uint8_t bInterfaceProtocol;
    uint8_t iInterface;
    const struct libusb_endpoint_descriptor* endpoint;
    const unsigned char* extra;
    int extra_length;
};

struct libusb_interface {
    const struct libusb_interface_descriptor* altsetting;
    int num_altsetting;
};

struct libusb_config_descriptor {
    uint8_t bLength;
    uint8_t bDescriptorType;
    uint16_t wTotalLength;
    uint8_t bNumInterfaces;
    uint8_t bConfigurationValue;
    uint8_t iConfiguration;
    uint8_t bmAttributes;
    uint8_t MaxPower;
    const struct libusb_interface* interface;
    const unsigned char* extra;
    int extra_length;
};

typedef struct libusb_endpoint_descriptor libusb_endpoint_descriptor;
typedef struct libusb_interface_descriptor libusb_interface_descriptor;
typedef struct libusb_interface libusb_interface;
typedef struct libusb_config_descriptor libusb_config_descriptor;

typedef struct libusb_control_setup {
    uint8_t bmRequestType;
    uint8_t bRequest;
    uint16_t wValue;
    uint16_t wIndex;
    uint16_t wLength;
} libusb_control_setup;

struct libusb_transfer;
typedef void(LIBUSB_CALL* libusb_transfer_cb_fn)(struct libusb_transfer* transfer);

typedef struct libusb_transfer {
    libusb_device_handle* dev_handle;
    uint8_t flags;
    unsigned char endpoint;
    unsigned char type;
    unsigned int timeout;
    enum libusb_transfer_status status;
    int length;
    int actual_length;
    libusb_transfer_cb_fn callback;
    void* user_data;
    unsigned char* buffer;
    int num_iso_packets;

    /* Test-backend state; production code never reads these fields. */
    int fake_submitted;
    int fake_cancel_requested;
    enum libusb_transfer_status fake_completion_status;
    int fake_completion_length;
    uint64_t fake_ready_poll;
} libusb_transfer;

int libusb_init(libusb_context** context);
int libusb_init_context(libusb_context** context, const struct libusb_init_option options[],
                        int num_options);
void libusb_exit(libusb_context* context);
const struct libusb_version* libusb_get_version(void);
ssize_t libusb_get_device_list(libusb_context* context, libusb_device*** list);
void libusb_free_device_list(libusb_device** list, int unref_devices);
int libusb_get_device_descriptor(libusb_device* device, libusb_device_descriptor* descriptor);
uint8_t libusb_get_bus_number(libusb_device* device);
uint8_t libusb_get_device_address(libusb_device* device);
int libusb_get_port_numbers(libusb_device* device, uint8_t* ports, int port_count);
int libusb_open(libusb_device* device, libusb_device_handle** handle);
void libusb_close(libusb_device_handle* handle);
libusb_device* libusb_get_device(libusb_device_handle* handle);
int libusb_get_configuration(libusb_device_handle* handle, int* config);
int libusb_set_configuration(libusb_device_handle* handle, int configuration);
int libusb_get_active_config_descriptor(libusb_device* device, libusb_config_descriptor** config);
void libusb_free_config_descriptor(libusb_config_descriptor* config);
int libusb_get_string_descriptor_ascii(libusb_device_handle* handle, uint8_t descriptor_index,
                                       unsigned char* data, int length);
int libusb_control_transfer(libusb_device_handle* handle, uint8_t request_type, uint8_t request,
                            uint16_t value, uint16_t index, unsigned char* data, uint16_t length,
                            unsigned int timeout);
int libusb_claim_interface(libusb_device_handle* handle, int interface_number);
int libusb_release_interface(libusb_device_handle* handle, int interface_number);
libusb_transfer* libusb_alloc_transfer(int iso_packets);
void libusb_free_transfer(libusb_transfer* transfer);
int libusb_submit_transfer(libusb_transfer* transfer);
int libusb_cancel_transfer(libusb_transfer* transfer);
int libusb_handle_events_timeout_completed(libusb_context* context, const struct timeval* timeout,
                                           int* completed);
void libusb_interrupt_event_handler(libusb_context* context);

static inline uint16_t aoahid_fake_cpu_to_le16(const uint16_t value) {
#if defined(__BYTE_ORDER__) && __BYTE_ORDER__ == __ORDER_BIG_ENDIAN__
    return (uint16_t)((value << 8U) | (value >> 8U));
#else
    return value;
#endif
}

static inline uint16_t aoahid_fake_le16_to_cpu(const uint16_t value) {
    return aoahid_fake_cpu_to_le16(value);
}

static inline void libusb_fill_control_setup(unsigned char* buffer, const uint8_t request_type,
                                             const uint8_t request, const uint16_t value,
                                             const uint16_t index, const uint16_t length) {
    libusb_control_setup* setup = (libusb_control_setup*)(void*)buffer;
    setup->bmRequestType = request_type;
    setup->bRequest = request;
    setup->wValue = aoahid_fake_cpu_to_le16(value);
    setup->wIndex = aoahid_fake_cpu_to_le16(index);
    setup->wLength = aoahid_fake_cpu_to_le16(length);
}

static inline void libusb_fill_control_transfer(libusb_transfer* transfer,
                                                libusb_device_handle* handle, unsigned char* buffer,
                                                libusb_transfer_cb_fn callback, void* user_data,
                                                const unsigned int timeout) {
    transfer->dev_handle = handle;
    transfer->endpoint = 0U;
    transfer->type = (unsigned char)LIBUSB_TRANSFER_TYPE_CONTROL;
    transfer->timeout = timeout;
    transfer->buffer = buffer;
    transfer->callback = callback;
    transfer->user_data = user_data;
    if (buffer != NULL) {
        const libusb_control_setup* setup = (const libusb_control_setup*)(const void*)buffer;
        transfer->length =
            (int)LIBUSB_CONTROL_SETUP_SIZE + (int)aoahid_fake_le16_to_cpu(setup->wLength);
    }
}

static inline void libusb_fill_bulk_transfer(libusb_transfer* transfer,
                                             libusb_device_handle* handle,
                                             const unsigned char endpoint, unsigned char* buffer,
                                             const int length, libusb_transfer_cb_fn callback,
                                             void* user_data, const unsigned int timeout) {
    transfer->dev_handle = handle;
    transfer->endpoint = endpoint;
    transfer->type = (unsigned char)LIBUSB_TRANSFER_TYPE_BULK;
    transfer->timeout = timeout;
    transfer->buffer = buffer;
    transfer->length = length;
    transfer->callback = callback;
    transfer->user_data = user_data;
}

typedef struct aoahid_fake_libusb_device_config {
    uint8_t bus;
    uint8_t address;
    const uint8_t* port_path;
    size_t port_path_length;
    uint16_t vendor_id;
    uint16_t product_id;
    uint16_t protocol_version;
    const char* serial;
    const char* product;
    int open_status;
} aoahid_fake_libusb_device_config;

typedef struct aoahid_fake_libusb_control_record {
    size_t device_index;
    uint8_t request_type;
    uint8_t request;
    uint16_t value;
    uint16_t index;
    uint16_t length;
    unsigned int timeout;
    int asynchronous;
} aoahid_fake_libusb_control_record;

typedef struct aoahid_fake_libusb_claim_record {
    size_t device_index;
    int interface_number;
    int release;
    int result;
} aoahid_fake_libusb_claim_record;

typedef struct aoahid_fake_libusb_init_option_record {
    int option;
    int integer_value;
    int callback_present;
} aoahid_fake_libusb_init_option_record;

typedef struct aoahid_fake_libusb_event_stats {
    size_t handle_calls;
    size_t waits_started;
    size_t active_waiters;
    size_t zero_timeout_calls;
    uint64_t requested_timeout_us;
    size_t notification_wakeups;
    size_t cancellation_wakeups;
    size_t interrupt_calls;
    size_t interrupt_wakeups;
    size_t timeout_wakeups;
    size_t callbacks_dispatched;
    size_t successful_submits;
} aoahid_fake_libusb_event_stats;

void aoahid_fake_libusb_reset(void);
int aoahid_fake_libusb_add_device(const aoahid_fake_libusb_device_config* config);
void aoahid_fake_libusb_set_present(size_t device_index, int present);
void aoahid_fake_libusb_set_protocol(size_t device_index, uint16_t protocol_version);
void aoahid_fake_libusb_set_claim_result(size_t device_index, int result);
void aoahid_fake_libusb_queue_control_result(int result);
void aoahid_fake_libusb_queue_submit_result(int result);
void aoahid_fake_libusb_queue_cancel_result(int result);
void aoahid_fake_libusb_queue_event_result(int result);
void aoahid_fake_libusb_queue_async_completion(enum libusb_transfer_status status,
                                               int actual_length, uint32_t delay_polls);
void aoahid_fake_libusb_make_pending_transfers_ready(void);
void aoahid_fake_libusb_set_control_recording(int enabled);
size_t aoahid_fake_libusb_control_count(void);
int aoahid_fake_libusb_get_control(size_t index, aoahid_fake_libusb_control_record* record);
size_t aoahid_fake_libusb_copy_control_data(size_t index, uint8_t* output, size_t capacity);
size_t aoahid_fake_libusb_claim_count(void);
int aoahid_fake_libusb_get_claim(size_t index, aoahid_fake_libusb_claim_record* record);
size_t aoahid_fake_libusb_pending_transfer_count(void);
size_t aoahid_fake_libusb_open_handle_count(void);
/* Successful libusb_open calls since reset. */
size_t aoahid_fake_libusb_open_call_count(void);
size_t aoahid_fake_libusb_cancel_count(void);
size_t aoahid_fake_libusb_event_handle_count(void);
int aoahid_fake_libusb_get_event_stats(aoahid_fake_libusb_event_stats* stats);
int aoahid_fake_libusb_wait_for_event_calls(size_t minimum, unsigned int timeout_ms);
int aoahid_fake_libusb_wait_for_event_waiters(size_t minimum, unsigned int timeout_ms);
size_t aoahid_fake_libusb_init_context_count(void);
size_t aoahid_fake_libusb_init_option_count(void);
int aoahid_fake_libusb_get_init_option(size_t index, aoahid_fake_libusb_init_option_record* record);

/* Bulk and re-enumeration emulation. */
typedef struct aoahid_fake_libusb_bulk_interface {
    uint8_t number;
    uint8_t interface_class;
    uint8_t interface_subclass;
    uint8_t interface_protocol;
    uint8_t endpoint_in;
    uint8_t endpoint_out;
    uint16_t max_packet;
} aoahid_fake_libusb_bulk_interface;

void aoahid_fake_libusb_add_bulk_interface(size_t device_index,
                                           const aoahid_fake_libusb_bulk_interface* value);
/* Delivers bytes as one Bulk IN transfer, now if one is pending, else later. */
void aoahid_fake_libusb_push_bulk_in(size_t device_index, uint8_t endpoint, const uint8_t* data,
                                     size_t length);
/* Every submitted Bulk IN transfer length, in submission order. */
size_t aoahid_fake_libusb_bulk_in_submit_count(void);
int aoahid_fake_libusb_bulk_in_submit_length(size_t index);
size_t aoahid_fake_libusb_bulk_out_count(void);
/* Returns the recorded length of one Bulk OUT transfer (zero for a ZLP). */
size_t aoahid_fake_libusb_copy_bulk_out(size_t index, uint8_t* output, size_t capacity);
/* Unplug: the device leaves the list and its pending transfers complete with
 * LIBUSB_TRANSFER_NO_DEVICE at the next event poll. */
void aoahid_fake_libusb_unplug(size_t device_index);
/* Active configuration value (1 by default; 0 is unconfigured). */
void aoahid_fake_libusb_set_active_configuration(size_t device_index, int value);
size_t aoahid_fake_libusb_set_configuration_count(void);

#ifdef __cplusplus
}
#endif

#endif
