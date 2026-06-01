#include "MTPSession.hpp"
#include <cassert>
#include <cstring>
#include <fstream>
#include <stdexcept>
#include <format>

#include <array>

namespace mtp {

// ─── UTF-16LE MTP string → UTF-8 ─────────────────────────────────────────────
// MTP strings: 1 byte length (chars including null), then length×2 bytes UTF-16LE.
static std::string decode_mtp_string(const uint8_t*& p, const uint8_t* end) {
    if (p >= end) return {};
    uint8_t nchars = *p++;
    if (nchars == 0) return {};
    std::string out;
    out.reserve(nchars);
    for (uint8_t i = 0; i < nchars && p + 1 < end; ++i) {
        uint16_t c = static_cast<uint16_t>(p[0]) | (static_cast<uint16_t>(p[1]) << 8);
        p += 2;
        if (c == 0) break;
        if (c < 0x80) {
            out += static_cast<char>(c);
        } else if (c < 0x800) {
            out += static_cast<char>(0xC0 | (c >> 6));
            out += static_cast<char>(0x80 | (c & 0x3F));
        } else {
            out += static_cast<char>(0xE0 | (c >> 12));
            out += static_cast<char>(0x80 | ((c >> 6) & 0x3F));
            out += static_cast<char>(0x80 | (c & 0x3F));
        }
    }
    return out;
}

// ─── Read a uint16 array: [4-byte count][count × uint16] ─────────────────────
static std::vector<uint16_t> decode_u16_array(const uint8_t*& p, const uint8_t* end) {
    if (p + 4 > end) return {};
    uint32_t n = p[0] | (p[1]<<8) | (p[2]<<16) | (p[3]<<24);
    p += 4;
    std::vector<uint16_t> v(n);
    for (auto& x : v) {
        if (p + 2 > end) break;
        x = static_cast<uint16_t>(p[0]) | (static_cast<uint16_t>(p[1]) << 8);
        p += 2;
    }
    return v;
}

// ─── Read a uint32 array ──────────────────────────────────────────────────────
static std::vector<uint32_t> decode_u32_array(const uint8_t*& p, const uint8_t* end) {
    if (p + 4 > end) return {};
    uint32_t n = p[0] | (p[1]<<8) | (p[2]<<16) | (p[3]<<24);
    p += 4;
    std::vector<uint32_t> v(n);
    for (auto& x : v) {
        if (p + 4 > end) break;
        x = p[0] | (p[1]<<8) | (p[2]<<16) | (p[3]<<24);
        p += 4;
    }
    return v;
}

// ─── Little-endian helpers ────────────────────────────────────────────────────
static void put_le32(uint8_t* dst, uint32_t v) {
    dst[0] = v; dst[1] = v>>8; dst[2] = v>>16; dst[3] = v>>24;
}
static uint32_t get_le32(const uint8_t* p) {
    return p[0] | (p[1]<<8) | (p[2]<<16) | (p[3]<<24);
}
static uint16_t get_le16(const uint8_t* p) {
    return static_cast<uint16_t>(p[0]) | (static_cast<uint16_t>(p[1])<<8);
}
static uint64_t get_le64(const uint8_t* p) {
    uint64_t lo = get_le32(p), hi = get_le32(p+4);
    return lo | (hi << 32);
}

// ─── MTPSession ───────────────────────────────────────────────────────────────
MTPSession::MTPSession() {
    if (libusb_init(&ctx_) != LIBUSB_SUCCESS)
        throw std::runtime_error("libusb_init failed");
#ifdef NDEBUG
    libusb_set_option(ctx_, LIBUSB_OPTION_LOG_LEVEL, LIBUSB_LOG_LEVEL_WARNING);
#else
    libusb_set_option(ctx_, LIBUSB_OPTION_LOG_LEVEL, LIBUSB_LOG_LEVEL_INFO);
#endif
}

MTPSession::~MTPSession() {
    if (connected_) disconnect();
    if (ctx_) libusb_exit(ctx_);
}

// ─── Device detection ─────────────────────────────────────────────────────────
bool MTPSession::find_device() {
    libusb_device** list = nullptr;
    ssize_t cnt = libusb_get_device_list(ctx_, &list);
    if (cnt < 0) return false;

    libusb_device* found = nullptr;

    for (ssize_t i = 0; i < cnt; ++i) {
        libusb_device* dev = list[i];
        libusb_device_descriptor desc{};
        if (libusb_get_device_descriptor(dev, &desc) != LIBUSB_SUCCESS) continue;

        // Match by Nintendo VID + known Switch PIDs first
        bool is_switch = (desc.idVendor == NINTENDO_VID)
                      && (desc.idProduct == SWITCH_PID_V1
                       || desc.idProduct == SWITCH_PID_V2);

        // Also accept any device advertising MTP class/subclass/protocol
        bool is_mtp_class = false;
        if (!is_switch) {
            for (uint8_t c = 0; c < desc.bNumConfigurations; ++c) {
                libusb_config_descriptor* cfg = nullptr;
                if (libusb_get_config_descriptor(dev, c, &cfg) != LIBUSB_SUCCESS) continue;
                for (uint8_t i2 = 0; i2 < cfg->bNumInterfaces; ++i2) {
                    for (int a = 0; a < cfg->interface[i2].num_altsetting; ++a) {
                        const auto& alt = cfg->interface[i2].altsetting[a];
                        if (alt.bInterfaceClass    == MTP_CLASS
                         && alt.bInterfaceSubClass == MTP_SUBCLASS
                         && alt.bInterfaceProtocol == MTP_PROTOCOL) {
                            is_mtp_class = true;
                        }
                    }
                }
                libusb_free_config_descriptor(cfg);
                if (is_mtp_class) break;
            }
        }

        if (is_switch || is_mtp_class) {
            found = dev;
            libusb_ref_device(found);
            vendor_id_  = desc.idVendor;
            product_id_ = desc.idProduct;
            break;
        }
    }
    libusb_free_device_list(list, 1);

    if (!found) return false;

    // Try to get a string descriptor for a friendly name
    libusb_device_handle* h = nullptr;
    if (libusb_open(found, &h) == LIBUSB_SUCCESS) {
        char buf[256]{};
        libusb_device_descriptor desc{};
        libusb_get_device_descriptor(found, &desc);
        if (desc.iProduct)
            libusb_get_string_descriptor_ascii(h, desc.iProduct,
                reinterpret_cast<unsigned char*>(buf), sizeof(buf));
        device_name_ = buf[0] ? buf : "Nintendo Switch";
        libusb_close(h);
    }
    libusb_unref_device(found);
    return true;
}

USBEndpoints MTPSession::find_endpoints(libusb_device* dev) {
    libusb_config_descriptor* cfg = nullptr;
    if (libusb_get_active_config_descriptor(dev, &cfg) != LIBUSB_SUCCESS)
        throw MTPException(ResponseCode::GeneralError, "Cannot read USB config descriptor");

    USBEndpoints ep{};
    bool ok = false;

    for (uint8_t i = 0; i < cfg->bNumInterfaces && !ok; ++i) {
        for (int a = 0; a < cfg->interface[i].num_altsetting && !ok; ++a) {
            const auto& alt = cfg->interface[i].altsetting[a];
            if (alt.bInterfaceClass    != MTP_CLASS    ||
                alt.bInterfaceSubClass != MTP_SUBCLASS ||
                alt.bInterfaceProtocol != MTP_PROTOCOL) continue;

            ep.interface_num = alt.bInterfaceNumber;
            for (uint8_t e = 0; e < alt.bNumEndpoints; ++e) {
                const auto& ed = alt.endpoint[e];
                uint8_t type = ed.bmAttributes & LIBUSB_TRANSFER_TYPE_MASK;
                bool in = (ed.bEndpointAddress & LIBUSB_ENDPOINT_DIR_MASK) == LIBUSB_ENDPOINT_IN;

                if (type == LIBUSB_TRANSFER_TYPE_BULK && in  && !ep.bulk_in)
                    ep.bulk_in  = ed.bEndpointAddress;
                if (type == LIBUSB_TRANSFER_TYPE_BULK && !in && !ep.bulk_out) {
                    ep.bulk_out         = ed.bEndpointAddress;
                    ep.bulk_out_max_pkt = ed.wMaxPacketSize;
                }
                if (type == LIBUSB_TRANSFER_TYPE_INTERRUPT && in)
                    ep.interrupt_in = ed.bEndpointAddress;
            }
            ok = (ep.bulk_in && ep.bulk_out);
        }
    }
    libusb_free_config_descriptor(cfg);

    if (!ok)
        throw MTPException(ResponseCode::GeneralError, "Could not locate MTP bulk endpoints");
    return ep;
}

// ─── Connect ──────────────────────────────────────────────────────────────────
DeviceInfo MTPSession::connect() {
    if (!find_device())
        throw MTPException(ResponseCode::GeneralError, "Nintendo Switch not found on USB bus");

    libusb_device** list = nullptr;
    ssize_t cnt = libusb_get_device_list(ctx_, &list);
    if (cnt < 0)
        throw MTPException(ResponseCode::GeneralError, "libusb_get_device_list failed");

    libusb_device* dev = nullptr;
    for (ssize_t i = 0; i < cnt; ++i) {
        libusb_device_descriptor desc{};
        libusb_get_device_descriptor(list[i], &desc);
        if (desc.idVendor == vendor_id_ && desc.idProduct == product_id_) {
            dev = list[i];
            break;
        }
    }

    if (!dev) {
        libusb_free_device_list(list, 1);
        throw MTPException(ResponseCode::GeneralError, "Device disappeared after detection");
    }

    ep_ = find_endpoints(dev);

    if (libusb_open(dev, &handle_) != LIBUSB_SUCCESS) {
        libusb_free_device_list(list, 1);
        throw MTPException(ResponseCode::GeneralError, "Cannot open USB device");
    }
    libusb_free_device_list(list, 1);

    // Detach kernel driver if attached (macOS shouldn't need this but be defensive)
    if (libusb_kernel_driver_active(handle_, ep_.interface_num) == 1)
        libusb_detach_kernel_driver(handle_, ep_.interface_num);

    int r = libusb_claim_interface(handle_, ep_.interface_num);
    if (r != LIBUSB_SUCCESS)
        throw MTPException(ResponseCode::GeneralError,
            std::format("Cannot claim USB interface: {}", libusb_error_name(r)));

    // Clear any stale halt/stall state left over from a previous session or
    // failed transfer — android-file-transfer-linux always does this before use.
    libusb_clear_halt(handle_, ep_.bulk_in);
    libusb_clear_halt(handle_, ep_.bulk_out);

    connected_.store(true);

    // ── MTP Session open ──────────────────────────────────────────────────────
    // GetDeviceInfo doesn't require an open session
    send_command(OpCode::GetDeviceInfo);
    auto raw_info = receive_data();
    auto cmd_resp = receive_response();
    if (cmd_resp.code != ResponseCode::OK)
        throw MTPException(cmd_resp.code, "GetDeviceInfo failed");

    // OpenSession
    uint32_t sid = SESSION_ID;
    send_command(OpCode::OpenSession, std::span<const uint32_t>{&sid, 1});
    auto open_resp = receive_response();
    if (open_resp.code != ResponseCode::OK
     && open_resp.code != ResponseCode::SessionAlreadyOpen)
        throw MTPException(open_resp.code, "OpenSession failed");

    // Parse DeviceInfo blob
    const uint8_t* p = raw_info.data();
    const uint8_t* end = p + raw_info.size();
    // Skip the 12-byte container header that receive_data includes
    if (raw_info.size() > CONTAINER_HEADER_SIZE) p += CONTAINER_HEADER_SIZE;

    DeviceInfo di{};
    if (p + 2 <= end) { di.standard_version = get_le16(p); p += 2; }
    if (p + 4 <= end) { di.vendor_extension_id = get_le32(p); p += 4; }
    if (p + 2 <= end) { di.vendor_extension_version = get_le16(p); p += 2; }
    di.vendor_extension_desc = decode_mtp_string(p, end);
    if (p + 2 <= end) { di.functional_mode = get_le16(p); p += 2; }
    di.operations_supported   = decode_u16_array(p, end);
    di.events_supported       = decode_u16_array(p, end);
    di.device_props_supported = decode_u16_array(p, end);
    di.capture_formats        = decode_u16_array(p, end);
    di.playback_formats       = decode_u16_array(p, end);
    di.manufacturer           = decode_mtp_string(p, end);
    di.model                  = decode_mtp_string(p, end);
    di.device_version         = decode_mtp_string(p, end);
    di.serial_number          = decode_mtp_string(p, end);

    device_info_ = di; // cache so MTPOperations can query supported ops
    return di;
}

// ─── Disconnect ───────────────────────────────────────────────────────────────
void MTPSession::disconnect() {
    if (!connected_.load()) return;
    connected_.store(false);

    try {
        send_command(OpCode::CloseSession);
        receive_response(); // best-effort; ignore result
    } catch (...) {}

    if (handle_) {
        libusb_release_interface(handle_, ep_.interface_num);
        libusb_close(handle_);
        handle_ = nullptr;
    }
}

// ─── Raw USB I/O ──────────────────────────────────────────────────────────────
// Returns true if the libusb error code means the device is physically gone
static bool is_disconnect_error(int r) {
    return r == LIBUSB_ERROR_NO_DEVICE
        || r == LIBUSB_ERROR_IO
        || r == LIBUSB_ERROR_PIPE
        || r == LIBUSB_ERROR_OTHER;
}

void MTPSession::bulk_write(std::span<const uint8_t> data, int timeout_ms) {
    // One libusb_bulk_transfer per call — never split a transfer across multiple
    // calls. If a call times out mid-transfer, calling bulk_transfer again on the
    // remaining bytes starts a new USB bulk transaction; the device sees a broken
    // packet boundary and the endpoint enters an inconsistent state.
    // With 64 KiB chunks each call completes in ~1 ms at USB 2.0 speeds, so
    // the timeout is a genuine error signal, not a flow-control retry trigger.
    int transferred = 0;
    int r = libusb_bulk_transfer(
        handle_, ep_.bulk_out,
        const_cast<uint8_t*>(data.data()),
        static_cast<int>(data.size()),
        &transferred, timeout_ms);

    if (r != LIBUSB_SUCCESS) {
        if (is_disconnect_error(r)) {
            connected_.store(false);
            throw MTPException(ResponseCode::GeneralError,
                "Switch disconnected during transfer — the app may have cancelled "
                "the install (e.g. wrong file format) or the USB cable was unplugged");
        }
        if (r == LIBUSB_ERROR_TIMEOUT)
            throw MTPException(ResponseCode::GeneralError,
                std::format("USB write timeout ({}ms, {} bytes) — "
                            "Switch may be busy writing to storage", timeout_ms,
                            static_cast<int>(data.size())));
        throw MTPException(ResponseCode::GeneralError,
            std::format("USB write error: {}", libusb_error_name(r)));
    }
    if (transferred != static_cast<int>(data.size()))
        throw MTPException(ResponseCode::IncompleteTransfer,
            std::format("USB write: sent {} of {} bytes", transferred, data.size()));
}

std::vector<uint8_t> MTPSession::bulk_read(size_t max_bytes, int timeout_ms) {
    std::vector<uint8_t> buf(max_bytes);
    int transferred = 0;
    int r = libusb_bulk_transfer(
        handle_, ep_.bulk_in,
        buf.data(), static_cast<int>(max_bytes),
        &transferred, timeout_ms);

    if (r != LIBUSB_SUCCESS && r != LIBUSB_ERROR_OVERFLOW) {
        if (is_disconnect_error(r)) {
            connected_.store(false);
            throw MTPException(ResponseCode::GeneralError,
                "Switch disconnected during read");
        }
        if (r == LIBUSB_ERROR_TIMEOUT)
            throw MTPException(ResponseCode::GeneralError,
                std::format("USB read timeout after {}ms", timeout_ms));
        throw MTPException(ResponseCode::GeneralError,
            std::format("USB read error: {}", libusb_error_name(r)));
    }
    buf.resize(static_cast<size_t>(transferred));
    return buf;
}

// ─── MTP container I/O ────────────────────────────────────────────────────────
void MTPSession::send_command(OpCode op, std::span<const uint32_t> params, uint32_t* out_tid) {
    last_op_code_ = static_cast<uint16_t>(op); // echo in subsequent data container
    uint32_t tid = next_transaction_id();
    if (out_tid) *out_tid = tid;

    size_t total = CONTAINER_HEADER_SIZE + params.size() * 4;
    std::vector<uint8_t> buf(total, 0);

    // Container layout: [0..3]=length [4..5]=type [6..7]=code [8..11]=tid [12+]=params
    put_le32(buf.data() + 0, static_cast<uint32_t>(total));
    buf[4] = static_cast<uint8_t>(static_cast<uint16_t>(ContainerType::Command) & 0xFF);
    buf[5] = static_cast<uint8_t>(static_cast<uint16_t>(ContainerType::Command) >> 8);
    buf[6] = static_cast<uint8_t>(static_cast<uint16_t>(op) & 0xFF);
    buf[7] = static_cast<uint8_t>(static_cast<uint16_t>(op) >> 8);
    put_le32(buf.data() + 8, tid);

    for (size_t i = 0; i < params.size(); ++i)
        put_le32(buf.data() + CONTAINER_HEADER_SIZE + i * 4, params[i]);

    bulk_write(buf, TIMEOUT_CMD_MS);
}

void MTPSession::send_data(std::span<const uint8_t> payload) {
    // MTP spec §6.4: data container code field must echo the initiating command's opcode.
    // DBI and some other implementations validate this and return OperationNotSupported
    // if it's wrong (they use it to route the payload to the right handler).
    uint32_t tid = transaction_id_;
    size_t total = CONTAINER_HEADER_SIZE + payload.size();

    std::vector<uint8_t> buf(total);
    put_le32(buf.data(), static_cast<uint32_t>(total));
    buf[4] = static_cast<uint8_t>(static_cast<uint16_t>(ContainerType::Data) & 0xFF);
    buf[5] = static_cast<uint8_t>(static_cast<uint16_t>(ContainerType::Data) >> 8);
    buf[6] = static_cast<uint8_t>(last_op_code_ & 0xFF);
    buf[7] = static_cast<uint8_t>(last_op_code_ >> 8);
    put_le32(buf.data() + 8, tid);
    std::memcpy(buf.data() + CONTAINER_HEADER_SIZE, payload.data(), payload.size());

    bulk_write(buf, TIMEOUT_WRITE_MS);
}

std::vector<uint8_t> MTPSession::receive_data() {
    // First read: get container header + however much data fits in one transfer
    auto first = bulk_read(CHUNK_SIZE, TIMEOUT_READ_MS);
    if (first.size() < CONTAINER_HEADER_SIZE)
        throw MTPException(ResponseCode::GeneralError, "Truncated MTP container header");

    uint32_t total_length = get_le32(first.data());
    uint16_t type         = get_le16(first.data() + 4);

    if (static_cast<ContainerType>(type) == ContainerType::Response) {
        // Device sent a response instead of data — parse and throw
        ResponseCode rc = static_cast<ResponseCode>(get_le16(first.data() + 6));
        if (rc != ResponseCode::OK)
            throw MTPException(rc, MTPException::code_name(rc));
        return first;
    }

    std::vector<uint8_t> data = std::move(first);
    // Read remaining chunks if total_length > what we received
    while (data.size() < total_length) {
        size_t remaining = total_length - data.size();
        auto chunk = bulk_read(std::min(remaining, CHUNK_SIZE), TIMEOUT_READ_MS);
        if (chunk.empty()) break;
        data.insert(data.end(), chunk.begin(), chunk.end());
    }
    return data;
}

CommandResponse MTPSession::receive_response(int timeout_ms) {
    auto buf = bulk_read(SMALL_CHUNK_SIZE, timeout_ms);
    if (buf.size() < CONTAINER_HEADER_SIZE)
        throw MTPException(ResponseCode::GeneralError, "Truncated MTP response");

    CommandResponse resp{};
    resp.code = static_cast<ResponseCode>(get_le16(buf.data() + 6));
    uint32_t length = get_le32(buf.data());
    size_t n_params = (length > CONTAINER_HEADER_SIZE)
                    ? (length - CONTAINER_HEADER_SIZE) / 4 : 0;
    for (size_t i = 0; i < n_params && CONTAINER_HEADER_SIZE + i*4+4 <= buf.size(); ++i)
        resp.params.push_back(get_le32(buf.data() + CONTAINER_HEADER_SIZE + i*4));
    return resp;
}

CommandResponse MTPSession::command(OpCode op, std::span<const uint32_t> params) {
    send_command(op, params);
    return receive_response();
}

// ─── Chunked file receive (GetObject) ─────────────────────────────────────────
void MTPSession::receive_data_to_file(const std::string& dest_path,
                                       uint64_t           expected_size,
                                       const ProgressFn&  progress,
                                       std::atomic<bool>& cancel) {
    std::ofstream ofs(dest_path, std::ios::binary | std::ios::trunc);
    if (!ofs) throw MTPException(ResponseCode::GeneralError,
        std::format("Cannot open destination file: {}", dest_path));

    // Read the header chunk
    auto header_chunk = bulk_read(CHUNK_SIZE, TIMEOUT_READ_MS);
    if (header_chunk.size() < CONTAINER_HEADER_SIZE)
        throw MTPException(ResponseCode::GeneralError, "Truncated data container header");

    uint32_t total_length = get_le32(header_chunk.data());
    uint64_t payload_size = (total_length > CONTAINER_HEADER_SIZE)
                          ? total_length - CONTAINER_HEADER_SIZE : 0;

    // Write payload portion of first chunk
    size_t first_payload = header_chunk.size() > CONTAINER_HEADER_SIZE
                         ? header_chunk.size() - CONTAINER_HEADER_SIZE : 0;
    if (first_payload)
        ofs.write(reinterpret_cast<const char*>(header_chunk.data() + CONTAINER_HEADER_SIZE),
                  static_cast<std::streamsize>(first_payload));

    uint64_t written = first_payload;
    if (progress) progress(written, expected_size ? expected_size : payload_size);

    while (written < payload_size) {
        if (cancel.load()) {
            ofs.close();
            std::remove(dest_path.c_str());
            throw MTPException(ResponseCode::TransactionCancelled, "Transfer cancelled");
        }
        size_t want = std::min<uint64_t>(CHUNK_SIZE, payload_size - written);
        auto chunk  = bulk_read(want, TIMEOUT_READ_MS);
        if (chunk.empty()) break;
        ofs.write(reinterpret_cast<const char*>(chunk.data()),
                  static_cast<std::streamsize>(chunk.size()));
        written += chunk.size();
        if (progress) progress(written, expected_size ? expected_size : payload_size);
    }
    ofs.close();
}

// ─── Chunked file send (SendObject) ───────────────────────────────────────────
void MTPSession::send_data_from_file(const std::string& src_path,
                                      uint64_t           file_size,
                                      OpCode             op,
                                      const ProgressFn&  progress,
                                      std::atomic<bool>& cancel) {
    std::ifstream ifs(src_path, std::ios::binary);
    if (!ifs) throw MTPException(ResponseCode::GeneralError,
        std::format("Cannot open source file: {}", src_path));

    uint32_t tid = transaction_id_;
    uint64_t total = CONTAINER_HEADER_SIZE + file_size;
    uint32_t wire_len = (total > 0xFFFFFFFFu) ? 0xFFFFFFFFu
                                               : static_cast<uint32_t>(total);

    // Build 12-byte container header
    std::array<uint8_t, CONTAINER_HEADER_SIZE> hdr{};
    put_le32(hdr.data(), wire_len);
    hdr[4] = static_cast<uint8_t>(static_cast<uint16_t>(ContainerType::Data) & 0xFF);
    hdr[5] = static_cast<uint8_t>(static_cast<uint16_t>(ContainerType::Data) >> 8);
    hdr[6] = static_cast<uint8_t>(static_cast<uint16_t>(op) & 0xFF);
    hdr[7] = static_cast<uint8_t>(static_cast<uint16_t>(op) >> 8);
    put_le32(hdr.data() + 8, tid);

    // ── First USB packet ────────────────────────────────────────────────────
    // go-mtpfs sends exactly ONE USB max-packet as the first write:
    //   header (12 bytes) + first (max_packet_size - 12) bytes of file data.
    // This gives the device a clean full USB packet containing the MTP header
    // followed immediately by data, matching how every real MTP client behaves.
    // Sending a 12-byte-only short packet (previous approach) causes DBI/Sphaira
    // to think the data phase ended before any file bytes were delivered.
    const size_t max_pkt = static_cast<size_t>(ep_.bulk_out_max_pkt); // 512 HS / 1024 SS
    size_t first_data_want = max_pkt > CONTAINER_HEADER_SIZE
                           ? max_pkt - CONTAINER_HEADER_SIZE : 0;
    first_data_want = static_cast<size_t>(
        std::min<uint64_t>(first_data_want, file_size));

    std::vector<uint8_t> first_pkt(CONTAINER_HEADER_SIZE + first_data_want);
    std::memcpy(first_pkt.data(), hdr.data(), CONTAINER_HEADER_SIZE);
    if (first_data_want > 0)
        ifs.read(reinterpret_cast<char*>(first_pkt.data() + CONTAINER_HEADER_SIZE),
                 static_cast<std::streamsize>(first_data_want));
    size_t first_got = static_cast<size_t>(ifs.gcount());
    first_pkt.resize(CONTAINER_HEADER_SIZE + first_got);

    bulk_write(first_pkt, TIMEOUT_WRITE_MS);
    uint64_t sent = first_got;
    if (progress) progress(sent, file_size);

    // ── Subsequent chunks ────────────────────────────────────────────────────
    // USB 3.0 SuperSpeed (max_pkt = 1024): use 512 KiB chunks — 32× larger than
    // the go-mtpfs 16 KiB baseline. Each libusb_bulk_transfer call costs ~100 µs
    // of host-side overhead; fewer, larger calls are needed to saturate USB 3.0.
    // USB 2.0 High Speed (max_pkt = 512): keep 16 KiB per go-mtpfs.
    const size_t xfer_chunk = (max_pkt >= 1024) ? CHUNK_SIZE_USB3 : CHUNK_SIZE;

    size_t last_write_size = first_pkt.size();
    std::vector<uint8_t> buf(xfer_chunk);
    while (sent < file_size) {
        if (cancel.load())
            throw MTPException(ResponseCode::TransactionCancelled, "Transfer cancelled");

        size_t want = static_cast<size_t>(std::min<uint64_t>(xfer_chunk, file_size - sent));
        ifs.read(reinterpret_cast<char*>(buf.data()), static_cast<std::streamsize>(want));
        auto got = static_cast<size_t>(ifs.gcount());
        if (got == 0) break;

        // Use a longer per-chunk timeout for USB 3.0 large chunks:
        // 512 KiB at even 1 MB/s = 512 ms; 10 s gives plenty of margin.
        const int chunk_timeout = (max_pkt >= 1024) ? 10000 : TIMEOUT_WRITE_MS;
        bulk_write(std::span<const uint8_t>(buf.data(), got), chunk_timeout);
        last_write_size = got;
        sent += got;
        if (progress) progress(sent, file_size);

        if (cancel.load())
            throw MTPException(ResponseCode::TransactionCancelled, "Transfer cancelled");
    }

    // ── Zero-length packet ─────────────────────────────────────────────────
    // go-mtpfs: "if lastTransfer % packetSize == 0, write a short packet just
    // to be sure." Without it, a packet-size-aligned last chunk has no natural
    // short-packet terminator and the device may hang waiting for more data.
    if (last_write_size % max_pkt == 0) {
        int dummy_tr = 0;
        libusb_bulk_transfer(handle_, ep_.bulk_out, nullptr, 0, &dummy_tr, 250);
    }
}

} // namespace mtp
