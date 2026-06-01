#pragma once
#include "MTPProtocol.hpp"
#include <libusb.h>
#include <atomic>
#include <functional>
#include <mutex>
#include <span>
#include <vector>

namespace mtp {

struct CommandResponse {
    ResponseCode             code;
    std::vector<uint32_t>    params;
};

struct USBEndpoints {
    uint8_t bulk_in           = 0;
    uint8_t bulk_out          = 0;
    uint8_t interrupt_in      = 0;
    int     interface_num     = 0;
    int     bulk_out_max_pkt  = 512; // wMaxPacketSize: 512 for USB 2.0 HS, 1024 for SS
};

using ProgressFn = std::function<void(uint64_t done, uint64_t total)>;

// MTPSession owns the libusb handle, USB endpoints, and MTP session state.
// All operations are serialized through the session mutex — MTP is strictly
// single-threaded from the device's perspective.
class MTPSession {
public:
    MTPSession();
    ~MTPSession();

    MTPSession(const MTPSession&)            = delete;
    MTPSession& operator=(const MTPSession&) = delete;

    // Scan USB bus, open the Switch, negotiate MTP session.
    // Returns DeviceInfo on success; throws MTPException on failure.
    DeviceInfo connect();

    // Close MTP session and release USB resources.
    void disconnect();

    bool is_connected() const { return connected_.load(); }

    // ── Low-level MTP API (called by MTPOperations) ──────────────────────────

    // Send a command container with up to 5 parameters.
    // Returns the assigned transaction_id via the optional out-param.
    void send_command(OpCode op,
                      std::span<const uint32_t> params = {},
                      uint32_t* out_tid = nullptr);

    // Send a data container following a command.
    void send_data(std::span<const uint8_t> data);

    // Receive a data container into a heap buffer.
    std::vector<uint8_t> receive_data();

    // Chunked receive directly into a file — avoids buffering whole file in RAM.
    void receive_data_to_file(const std::string&  dest_path,
                              uint64_t            expected_size,
                              const ProgressFn&   progress,
                              std::atomic<bool>&  cancel);

    // Chunked send of a file as a data container.
    // op must be the OpCode from the preceding send_command() call so the
    // data container echoes the correct operation code (MTP spec §6.4).
    void send_data_from_file(const std::string&  src_path,
                             uint64_t            file_size,
                             OpCode              op,
                             const ProgressFn&   progress,
                             std::atomic<bool>&  cancel);

    // Receive the response container after a command (+optional data exchange).
    // Pass TIMEOUT_INSTALL_MS after large file transfers so post-install DB work
    // on the Switch side has time to complete before we declare a timeout error.
    CommandResponse receive_response(int timeout_ms = TIMEOUT_CMD_MS);

    // Convenience: send command then immediately receive response (no data phase).
    CommandResponse command(OpCode op, std::span<const uint32_t> params = {});

    uint32_t next_transaction_id() { return ++transaction_id_; }

    // Accessors for device metadata
    uint16_t          vendor_id()    const { return vendor_id_;   }
    uint16_t          product_id()   const { return product_id_;  }
    std::string       device_name()  const { return device_name_; }
    const DeviceInfo& device_info()  const { return device_info_; }

    bool supports_op(OpCode op) const {
        auto code = static_cast<uint16_t>(op);
        for (auto c : device_info_.operations_supported)
            if (c == code) return true;
        return false;
    }

    // Serialize all MTP access through this mutex (TransferEngine uses it)
    std::mutex& mutex() { return mtx_; }

private:
    bool     find_device();
    USBEndpoints find_endpoints(libusb_device* dev);

    // Raw USB bulk I/O
    std::vector<uint8_t> bulk_read(size_t max_bytes, int timeout_ms = TIMEOUT_MS);
    void                 bulk_write(std::span<const uint8_t> data, int timeout_ms = TIMEOUT_MS);

    libusb_context*       ctx_     = nullptr;
    libusb_device_handle* handle_  = nullptr;
    USBEndpoints          ep_;

    uint32_t   transaction_id_ = 0;
    uint16_t   last_op_code_   = 0;
    uint16_t   vendor_id_      = 0;
    uint16_t   product_id_     = 0;
    DeviceInfo device_info_;
    std::string device_name_;

    std::atomic<bool> connected_{false};
    mutable std::mutex mtx_;
};

} // namespace mtp
