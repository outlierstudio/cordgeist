// SPDX-License-Identifier: MIT
// Copyright (c) 2026 libaoahid contributors
/* Declares the AOA/libusb transport boundary and its ownership model; HID
 * profile semantics remain in the layers above it. */
#pragma once

/*
 * This header is the boundary of the libusb transport. It exposes only plain
 * host-side values to the API and profile layers; no libusb type crosses it.
 */

#include "aoahid.h"

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <mutex>
#include <string>
#include <vector>

namespace aoa::transport {

class Device;
class Port;

struct Candidate {
    std::uint8_t bus{};
    std::uint8_t address{};
    std::vector<std::uint8_t> port_path;
    std::uint16_t vendor_id{};
    std::uint16_t product_id{};
    std::uint16_t protocol_version{};
    std::string serial;
    std::string product;
};

struct DeviceConfig {
    aoahid_event_mode event_mode{};
    std::uint32_t descriptor_fragment_bytes{};
    std::uint32_t pool_slots{};
    std::uint32_t maximum_report_bytes{};
    aoahid_interface_claim_policy claim_policy{};
    std::int32_t interface_number{};
};

using Completion = void (*)(void* user, aoahid_result result, std::int32_t native_status) noexcept;

struct PreparedTransfer {
    void* slot{};
    std::uint8_t* payload{};
    std::size_t capacity{};
};

struct ReservationToken {
    void* value{};
};

struct ErrorInfo {
    aoahid_result result{AOAHID_OK};
    std::int32_t native_status{};
    std::int32_t aoa_request{};
    std::uint16_t hid_id{};
    std::uint32_t offset{};
    std::uint32_t length{};
    std::uint16_t report_id{};
};

ErrorInfo last_error() noexcept;
bool is_cancelled_completion_status(std::int32_t native_status) noexcept;
void reset_error() noexcept;
void record_error(aoahid_result result, std::int32_t native_status = 0,
                  std::int32_t aoa_request = 0, std::uint16_t hid_id = 0, std::uint32_t offset = 0,
                  std::uint32_t length = 0) noexcept;

class Runtime final {
  public:
    static aoahid_result create(aoahid_event_mode event_mode, Runtime** out) noexcept;
    ~Runtime() noexcept;

    Runtime(const Runtime&) = delete;
    Runtime& operator=(const Runtime&) = delete;

    aoahid_result poll(std::uint32_t timeout_ms) noexcept;
    void interrupt_event_handler() noexcept;
    aoahid_result discover(std::uint32_t timeout_ms, std::vector<Candidate>* out);
    void* native_context() const noexcept;

    /* Opens the candidate's original device as a new Port with one reference.
     * On failure the libusb status is recorded and *out stays null. */
    aoahid_result acquire_port(const Candidate& candidate, Port** out);

    /* Sends AOA requests 51, 52 (for each non-null string), and 53 to the
     * candidate and closes it again; the device then re-enumerates in
     * accessory mode. It does not wait for that and never retries. A device
     * already in accessory mode receives nothing. strings holds manufacturer,
     * model, description, version, URI, and serial in AOA string-ID order. */
    aoahid_result start_accessory(const Candidate& candidate, const char* const strings[6],
                                  std::uint32_t control_timeout_ms);

  private:
    Runtime() noexcept = default;

    friend class Device;
    friend class Port;
    void unregister_port(Port* port) noexcept;
    // Returns a retained Port already open for bus/address, or null.
    Port* retain_port(std::uint8_t bus, std::uint8_t address) noexcept;

    void* context_{};
    aoahid_event_mode event_mode_{};
    // Cold path only: acquire/release happen at open/close, never per report.
    std::mutex ports_mutex_;
    std::vector<Port*> ports_;
};

/* One opened physical USB device: the only owner of its libusb handle and of
 * its claimed interfaces. A Device and its Channels borrow it through a
 * reference count; the handle closes when the last reference is released,
 * which each borrower does only after its own transfers have drained.
 * Discovery probes an already open device through its Port instead of opening
 * a second handle. */
class Port final {
  public:
    Port(const Port&) = delete;
    Port& operator=(const Port&) = delete;

    void retain() noexcept;
    void release() noexcept;
    void* native_handle() const noexcept;
    std::size_t references() const noexcept;

    /* Claims are counted per interface, so a Device and a Channel may claim
     * different interfaces on one handle. */
    std::int32_t claim_interface(std::int32_t interface_number) noexcept;
    void release_interface(std::int32_t interface_number) noexcept;

  private:
    friend class Runtime;
    Port() noexcept = default;
    ~Port() noexcept = default;
    bool try_retain() noexcept;

    struct Claim {
        std::int32_t interface_number{};
        std::uint32_t count{};
    };

    Runtime* runtime_{};
    void* handle_{};
    std::uint8_t bus_{};
    std::uint8_t address_{};
    std::atomic<std::size_t> references_{0U};
    std::mutex claims_mutex_;
    std::vector<Claim> claims_;
};

class Device final {
  public:
    static aoahid_result open(Runtime* runtime, const Candidate& candidate,
                              const DeviceConfig& config, Device** out);
    ~Device() noexcept;

    Device(const Device&) = delete;
    Device& operator=(const Device&) = delete;

    aoahid_result register_hid(std::uint16_t hid_id, const std::uint8_t* descriptor,
                               std::size_t descriptor_length,
                               std::uint32_t control_timeout_ms) noexcept;
    aoahid_result unregister_hid(std::uint16_t hid_id, std::uint32_t control_timeout_ms) noexcept;
    aoahid_result set_reservation(std::uint16_t hid_id, std::size_t count, ReservationToken* out);
    aoahid_result clear_reservation(ReservationToken token) noexcept;
    aoahid_result acquire(std::uint16_t hid_id, ReservationToken reservation,
                          std::size_t report_length, void* user, Completion completion,
                          PreparedTransfer* out) noexcept;
    aoahid_result submit(PreparedTransfer prepared, std::size_t report_length,
                         std::uint16_t report_id, std::uint32_t send_timeout_ms) noexcept;
    void abandon(PreparedTransfer prepared) noexcept;
    aoahid_result cancel_all() noexcept;
    bool drained() const noexcept;
    bool present() const noexcept;
    aoahid_result latched_error(ErrorInfo* detail = nullptr) noexcept;
    // The Port a Channel on this Device borrows.
    Port* port() const noexcept;

  private:
    Device() noexcept = default;

    struct Impl;
    Impl* impl_{};
};

struct ChannelConfig {
    std::uint8_t interface_class{};
    std::uint8_t interface_subclass{};
    std::uint8_t interface_protocol{};
    std::uint32_t in_transfers{};
    std::uint32_t out_transfers{};
    std::uint32_t transfer_bytes{};
    bool zero_length_termination{};
    // One IN transfer per read, sized by the reader, instead of read-ahead.
    bool request_reads{};
    // Caller-poll mode only: the Runtime a blocking read/write pumps.
    Runtime* pump{};
};

/* A Bulk IN/OUT pair on a Device's Port. IN transfers stay submitted (read
 * ahead), or in request mode one IN transfer sized by each read is, and
 * completions reach the reader through a single-producer, single-consumer
 * ring; OUT slots return to the writer the same way. The libusb callback only
 * publishes, counts, and wakes a waiting thread. */
class Channel final {
  public:
    static aoahid_result open(Port* port, const ChannelConfig& config, Channel** out);
    ~Channel() noexcept;

    Channel(const Channel&) = delete;
    Channel& operator=(const Channel&) = delete;

    aoahid_result read(std::uint8_t* buffer, std::size_t capacity, std::size_t* received,
                       std::uint32_t timeout_ms) noexcept;
    aoahid_result write(const std::uint8_t* data, std::size_t length, std::size_t* written,
                        std::uint32_t timeout_ms) noexcept;
    /* Marks the Channel lost and cancels every transfer; reads and writes then
     * fail with AOAHID_ERR_NO_DEVICE. */
    void lose() noexcept;
    bool drained() const noexcept;

  private:
    Channel() noexcept = default;
    struct Impl;
    Impl* impl_{};
};

aoahid_result map_libusb_error(std::int32_t status, bool probe) noexcept;

} // namespace aoa::transport
