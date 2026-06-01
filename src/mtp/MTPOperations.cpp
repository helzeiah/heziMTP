#include "MTPOperations.hpp"
#include <cstring>
#include <stdexcept>
#include <format>

namespace mtp {

// ─── Encoding helpers ─────────────────────────────────────────────────────────
void MTPOperations::put_le16(std::vector<uint8_t>& b, uint16_t v) {
    b.push_back(v & 0xFF);
    b.push_back(v >> 8);
}
void MTPOperations::put_le32(std::vector<uint8_t>& b, uint32_t v) {
    b.push_back(v & 0xFF); b.push_back((v>>8)&0xFF);
    b.push_back((v>>16)&0xFF); b.push_back(v>>24);
}
void MTPOperations::put_le64(std::vector<uint8_t>& b, uint64_t v) {
    put_le32(b, static_cast<uint32_t>(v));
    put_le32(b, static_cast<uint32_t>(v >> 32));
}

// UTF-8 → MTP UTF-16LE string: [1-byte len][len × 2-byte UTF-16LE chars]
void MTPOperations::encode_mtp_string(std::vector<uint8_t>& buf, const std::string& utf8) {
    if (utf8.empty()) { buf.push_back(0); return; }
    // Simple ASCII-only encoding (sufficient for filenames in practice)
    size_t len = std::min(utf8.size(), size_t(255)) + 1; // +1 for null terminator
    buf.push_back(static_cast<uint8_t>(len));
    for (size_t i = 0; i < len - 1; ++i) {
        uint8_t c = static_cast<uint8_t>(utf8[i]);
        buf.push_back(c);
        buf.push_back(0);
    }
    // Null terminator
    buf.push_back(0);
    buf.push_back(0);
}

// ─── Parsing helpers ──────────────────────────────────────────────────────────
static uint16_t u16(const uint8_t* p) {
    return static_cast<uint16_t>(p[0]) | (static_cast<uint16_t>(p[1]) << 8);
}
static uint32_t u32(const uint8_t* p) {
    return p[0] | (p[1]<<8) | (p[2]<<16) | (p[3]<<24);
}
static uint64_t u64(const uint8_t* p) {
    uint64_t lo = u32(p), hi = u32(p+4);
    return lo | (hi << 32);
}
static std::string mtp_str(const uint8_t*& p, const uint8_t* end) {
    if (p >= end) return {};
    uint8_t n = *p++;
    if (!n) return {};
    std::string s; s.reserve(n);
    for (uint8_t i = 0; i < n && p+1 < end; ++i) {
        uint16_t c = static_cast<uint16_t>(p[0]) | (static_cast<uint16_t>(p[1])<<8);
        p += 2;
        if (!c) break;
        if (c < 0x80)       s += static_cast<char>(c);
        else if (c < 0x800) { s += static_cast<char>(0xC0|(c>>6)); s += static_cast<char>(0x80|(c&0x3F)); }
        else                { s += static_cast<char>(0xE0|(c>>12)); s += static_cast<char>(0x80|((c>>6)&0x3F)); s += static_cast<char>(0x80|(c&0x3F)); }
    }
    return s;
}

std::vector<uint32_t> MTPOperations::parse_u32_array(const std::vector<uint8_t>& data) {
    const uint8_t* p = data.data() + CONTAINER_HEADER_SIZE;
    const uint8_t* end = data.data() + data.size();
    if (p + 4 > end) return {};
    uint32_t n = u32(p); p += 4;
    std::vector<uint32_t> v; v.reserve(n);
    for (uint32_t i = 0; i < n && p + 4 <= end; ++i) {
        v.push_back(u32(p)); p += 4;
    }
    return v;
}

ObjectInfo MTPOperations::parse_object_info(const std::vector<uint8_t>& data) {
    const uint8_t* p = data.data() + CONTAINER_HEADER_SIZE;
    const uint8_t* end = data.data() + data.size();

    ObjectInfo oi{};
    if (p + 4  <= end) { oi.storage_id        = u32(p); p += 4; }
    if (p + 2  <= end) { oi.object_format      = u16(p); p += 2; }
    if (p + 2  <= end) { oi.protection_status  = u16(p); p += 2; }
    // MTP spec defines compressed_size as uint32_t in the ObjectInfo struct,
    // but DBI/Sphaira follow the standard. For files > 4 GiB use GetObjectPropValue.
    uint32_t wire_size = 0;
    if (p + 4  <= end) { wire_size = u32(p); p += 4; }
    oi.compressed_size = wire_size; // extended size handled via GetObjectPropValue

    if (p + 2  <= end) { /* thumb format  */ p += 2; }
    if (p + 4  <= end) { /* thumb size    */ p += 4; }
    if (p + 4  <= end) { /* thumb w       */ p += 4; }
    if (p + 4  <= end) { /* thumb h       */ p += 4; }
    if (p + 4  <= end) { /* img w         */ p += 4; }
    if (p + 4  <= end) { /* img h         */ p += 4; }
    if (p + 4  <= end) { /* img depth     */ p += 4; }
    if (p + 4  <= end) { oi.parent_object     = u32(p); p += 4; }
    if (p + 2  <= end) { oi.association_type  = u16(p); p += 2; }
    if (p + 4  <= end) { oi.association_desc  = u32(p); p += 4; }
    if (p + 4  <= end) { oi.sequence_number   = u32(p); p += 4; }
    oi.filename     = mtp_str(p, end);
    oi.date_created = mtp_str(p, end);
    oi.date_modified = mtp_str(p, end);
    /* keywords = */ mtp_str(p, end);

    return oi;
}

// ─── MTPOperations implementation ─────────────────────────────────────────────
MTPOperations::MTPOperations(MTPSession& session) : session_(session) {}

std::vector<uint32_t> MTPOperations::get_storage_ids() {
    std::lock_guard lock(session_.mutex());
    session_.send_command(OpCode::GetStorageIDs);
    auto data = session_.receive_data();
    auto resp = session_.receive_response();
    if (resp.code != ResponseCode::OK)
        throw MTPException(resp.code, MTPException::code_name(resp.code));
    return parse_u32_array(data);
}

StorageInfo MTPOperations::get_storage_info(uint32_t storage_id) {
    std::lock_guard lock(session_.mutex());
    std::array<uint32_t,1> p{storage_id};
    session_.send_command(OpCode::GetStorageInfo, p);
    auto data = session_.receive_data();
    auto resp = session_.receive_response();
    if (resp.code != ResponseCode::OK)
        throw MTPException(resp.code, MTPException::code_name(resp.code));

    const uint8_t* ptr = data.data() + CONTAINER_HEADER_SIZE;
    const uint8_t* end = data.data() + data.size();
    StorageInfo si{};
    if (ptr+2<=end){si.storage_type      = u16(ptr); ptr+=2;}
    if (ptr+2<=end){si.filesystem_type   = u16(ptr); ptr+=2;}
    if (ptr+2<=end){si.access_capability = u16(ptr); ptr+=2;}
    if (ptr+8<=end){si.max_capacity      = u64(ptr); ptr+=8;}
    if (ptr+8<=end){si.free_space_bytes  = u64(ptr); ptr+=8;}
    if (ptr+4<=end){si.free_space_objects= u32(ptr); ptr+=4;}
    si.description = mtp_str(ptr, end);
    si.volume_id   = mtp_str(ptr, end);
    return si;
}

std::vector<uint32_t> MTPOperations::get_object_handles(uint32_t storage_id,
                                                          uint32_t parent_handle) {
    std::lock_guard lock(session_.mutex());
    std::array<uint32_t,3> params{ storage_id, ALL_FORMATS, parent_handle };
    session_.send_command(OpCode::GetObjectHandles, params);
    auto data = session_.receive_data();
    auto resp = session_.receive_response();
    if (resp.code != ResponseCode::OK)
        throw MTPException(resp.code, MTPException::code_name(resp.code));
    return parse_u32_array(data);
}

ObjectInfo MTPOperations::get_object_info(uint32_t handle) {
    std::lock_guard lock(session_.mutex());
    std::array<uint32_t,1> p{handle};
    session_.send_command(OpCode::GetObjectInfo, p);
    auto data = session_.receive_data();
    auto resp = session_.receive_response();
    if (resp.code != ResponseCode::OK)
        throw MTPException(resp.code, MTPException::code_name(resp.code));
    return parse_object_info(data);
}

void MTPOperations::get_object(uint32_t handle,
                                const std::string& dest_path,
                                uint64_t expected_size,
                                const ProgressFn& progress,
                                std::atomic<bool>& cancel) {
    std::lock_guard lock(session_.mutex());
    std::array<uint32_t,1> p{handle};
    session_.send_command(OpCode::GetObject, p);
    session_.receive_data_to_file(dest_path, expected_size, progress, cancel);
    auto resp = session_.receive_response();
    if (resp.code != ResponseCode::OK && resp.code != ResponseCode::TransactionCancelled)
        throw MTPException(resp.code, MTPException::code_name(resp.code));
}

std::vector<uint8_t> MTPOperations::build_object_info(uint32_t storage_id,
                                                        uint32_t parent_handle,
                                                        const std::string& filename,
                                                        uint64_t file_size,
                                                        uint16_t format) {
    std::vector<uint8_t> b;
    b.reserve(128);

    // Container header placeholder (will be filled in by session)
    // Actual wire bytes are handled by send_data(), so we just build the payload here.
    put_le32(b, storage_id);
    put_le16(b, format);
    put_le16(b, 0);  // protection_status = none
    // compressed_size as uint32 (0 = unknown, or truncated for large files)
    put_le32(b, file_size > 0xFFFFFFFFu ? 0xFFFFFFFFu : static_cast<uint32_t>(file_size));
    put_le16(b, 0);  // thumb_format
    put_le32(b, 0);  // thumb_compressed_size
    put_le32(b, 0);  // thumb_pix_w
    put_le32(b, 0);  // thumb_pix_h
    put_le32(b, 0);  // image_pix_w
    put_le32(b, 0);  // image_pix_h
    put_le32(b, 0);  // image_bit_depth
    put_le32(b, parent_handle);
    put_le16(b, 0);  // association_type
    put_le32(b, 0);  // association_desc
    put_le32(b, 0);  // sequence_number
    encode_mtp_string(b, filename);
    b.push_back(0);  // date_created (empty string)
    b.push_back(0);  // date_modified (empty string)
    b.push_back(0);  // keywords (empty string)
    return b;
}

// Build the SendObjectPropList data payload — just the filename property.
// The actual 64-bit file size is passed in the command parameters, not here.
static std::vector<uint8_t> build_prop_list(const std::string& filename) {
    std::vector<uint8_t> b;
    // number of properties = 1
    b.push_back(1); b.push_back(0); b.push_back(0); b.push_back(0);
    // object handle = 0 (new object)
    b.push_back(0); b.push_back(0); b.push_back(0); b.push_back(0);
    // property code = ObjectFilename (0xDC07)
    b.push_back(static_cast<uint8_t>(PROP_OBJECT_FILENAME & 0xFF));
    b.push_back(static_cast<uint8_t>(PROP_OBJECT_FILENAME >> 8));
    // data type = String (0xFFFF)
    b.push_back(static_cast<uint8_t>(DTYPE_STRING & 0xFF));
    b.push_back(static_cast<uint8_t>(DTYPE_STRING >> 8));
    // MTP string value
    if (filename.empty()) {
        b.push_back(0);
    } else {
        size_t len = std::min(filename.size(), size_t(255)) + 1;
        b.push_back(static_cast<uint8_t>(len));
        for (size_t i = 0; i < len - 1; ++i) {
            b.push_back(static_cast<uint8_t>(filename[i]));
            b.push_back(0);
        }
        b.push_back(0); b.push_back(0); // null terminator
    }
    return b;
}

uint32_t MTPOperations::send_object(uint32_t storage_id,
                                     uint32_t parent_handle,
                                     const std::string& src_path,
                                     const std::string& filename,
                                     uint64_t file_size,
                                     const ProgressFn& progress,
                                     std::atomic<bool>& cancel) {
    // Always use Undefined (0x3000) for game files.
    // DBI/Sphaira reject vendor-specific format codes like 0xB005 for NSP/XCI
    // and return OperationNotSupported. Undefined is universally accepted.
    uint16_t fmt = static_cast<uint16_t>(ObjectFormat::Undefined);

    // All of SendObjectInfo/SendObjectPropList + SendObject must be atomic.
    std::lock_guard lock(session_.mutex());

    // Step 1: announce the object to the device.
    //
    // Prefer SendObjectPropList (0x9808) when supported — it passes the actual
    // 64-bit file size in the command parameters (size_hi, size_lo), so the
    // device knows the real byte count for files >4 GiB instead of receiving
    // 0xFFFFFFFF and stopping at ~4.29 GB.
    //
    // Fall back to SendObjectInfo (0x100C) for devices that don't support it.
    if (session_.supports_op(OpCode::SendObjectPropList)) {
        // params: [storage_id, parent_handle, format, size_hi, size_lo]
        uint32_t size_hi = static_cast<uint32_t>(file_size >> 32);
        uint32_t size_lo = static_cast<uint32_t>(file_size & 0xFFFFFFFFu);
        std::array<uint32_t,5> params{
            storage_id, parent_handle,
            static_cast<uint32_t>(fmt),
            size_hi, size_lo
        };
        session_.send_command(OpCode::SendObjectPropList, params);
        auto prop_list = build_prop_list(filename);
        session_.send_data(prop_list);
    } else {
        std::array<uint32_t,2> params{ storage_id, parent_handle };
        session_.send_command(OpCode::SendObjectInfo, params);
        auto obj_info = build_object_info(storage_id, parent_handle, filename, file_size, fmt);
        session_.send_data(obj_info);
    }
    {
        auto resp = session_.receive_response();
        if (resp.code != ResponseCode::OK)
            throw MTPException(resp.code, MTPException::code_name(resp.code));
    }

    // Step 2: SendObject — immediately follows, no gap for foreign commands
    session_.send_command(OpCode::SendObject);
    session_.send_data_from_file(src_path, file_size, OpCode::SendObject, progress, cancel);

    // Use TIMEOUT_INSTALL_MS: after the last byte DBI/Sphaira verify NCAs and
    // update the content database, which can take well over 5 s on slow microSD.
    {
        auto resp = session_.receive_response(TIMEOUT_INSTALL_MS);
        if (resp.code != ResponseCode::OK && resp.code != ResponseCode::TransactionCancelled)
            throw MTPException(resp.code, MTPException::code_name(resp.code));
    }

    return 0;
}

void MTPOperations::delete_object(uint32_t handle) {
    std::lock_guard lock(session_.mutex());
    std::array<uint32_t,2> params{ handle, ALL_FORMATS };
    session_.send_command(OpCode::DeleteObject, params);
    auto resp = session_.receive_response();
    if (resp.code != ResponseCode::OK)
        throw MTPException(resp.code, MTPException::code_name(resp.code));
}

uint32_t MTPOperations::create_directory(uint32_t storage_id,
                                          uint32_t parent_handle,
                                          const std::string& name) {
    std::lock_guard lock(session_.mutex());
    std::array<uint32_t,2> params{ storage_id, parent_handle };
    session_.send_command(OpCode::SendObjectInfo, params);

    auto info = build_object_info(storage_id, parent_handle, name, 0,
        static_cast<uint16_t>(ObjectFormat::Association));
    session_.send_data(info);

    auto resp = session_.receive_response();
    if (resp.code != ResponseCode::OK)
        throw MTPException(resp.code, MTPException::code_name(resp.code));
    return resp.params.size() >= 3 ? resp.params[2] : 0;
}

} // namespace mtp
