#pragma once
#include <cstdint>
#include <string>
#include <vector>

namespace mtp {

// ─── Container types ──────────────────────────────────────────────────────────
enum class ContainerType : uint16_t {
    Command  = 0x0001,
    Data     = 0x0002,
    Response = 0x0003,
    Event    = 0x0004,
};

// ─── MTP Operation codes ──────────────────────────────────────────────────────
enum class OpCode : uint16_t {
    GetDeviceInfo           = 0x1001,
    OpenSession             = 0x1002,
    CloseSession            = 0x1003,
    GetStorageIDs           = 0x1004,
    GetStorageInfo          = 0x1005,
    GetNumObjects           = 0x1006,
    GetObjectHandles        = 0x1007,
    GetObjectInfo           = 0x1008,
    GetObject               = 0x1009,
    GetThumb                = 0x100A,
    DeleteObject            = 0x100B,
    SendObjectInfo          = 0x100C,
    SendObject              = 0x100D,
    GetDevicePropDesc       = 0x1014,
    GetDevicePropValue      = 0x1015,
    SetDevicePropValue      = 0x1016,
    MoveObject              = 0x1019,
    CopyObject              = 0x101A,
    GetPartialObject        = 0x101B,
    GetObjectPropsSupported = 0x9801,
    GetObjectPropDesc       = 0x9802,
    GetObjectPropValue      = 0x9803,
    SetObjectPropValue      = 0x9804,
    // Modern object creation — passes full 64-bit size in command params
    // so devices know the real size even for files > 4 GiB.
    SendObjectPropList      = 0x9808,
};

// ─── Response codes ───────────────────────────────────────────────────────────
enum class ResponseCode : uint16_t {
    OK                       = 0x2001,
    GeneralError             = 0x2002,
    SessionNotOpen           = 0x2003,
    InvalidTransactionID     = 0x2004,
    OperationNotSupported    = 0x2005,
    ParameterNotSupported    = 0x2006,
    IncompleteTransfer       = 0x2007,
    InvalidStorageID         = 0x2008,
    InvalidObjectHandle      = 0x2009,
    DevicePropNotSupported   = 0x200A,
    StoreFull                = 0x200C,
    ObjectWriteProtected     = 0x200D,
    StoreReadOnly            = 0x200E,
    AccessDenied             = 0x200F,
    DeviceBusy               = 0x2019,
    InvalidParentObject      = 0x201A,
    InvalidParameter         = 0x201D,
    SessionAlreadyOpen       = 0x201E,
    TransactionCancelled     = 0x201F,
};

// ─── Object format codes ──────────────────────────────────────────────────────
enum class ObjectFormat : uint16_t {
    Undefined  = 0x3000,
    Association = 0x3001,   // Directory/folder
    Script     = 0x3002,
    Text       = 0x3004,
    HTML       = 0x3005,
    MP3        = 0x3009,
    AVI        = 0x300A,
    MPEG       = 0x300B,
    JPEG       = 0x3801,
    BMP        = 0x3804,
    GIF        = 0x3807,
    PNG        = 0x380B,
    TIFF       = 0x380D,
    NSP        = 0xB005,    // Nintendo Switch Package
    XCI        = 0xB006,    // Nintendo Switch Cartridge Image
};

// ─── Wire-format container header (little-endian, packed) ────────────────────
#pragma pack(push, 1)
struct ContainerHeader {
    uint32_t length;          // total bytes including this header
    uint16_t type;
    uint16_t code;
    uint32_t transaction_id;
};
static_assert(sizeof(ContainerHeader) == 12);
#pragma pack(pop)

// ─── Object property codes (for SendObjectPropList payload) ──────────────────
static constexpr uint16_t PROP_OBJECT_FILENAME = 0xDC07;
static constexpr uint16_t PROP_OBJECT_SIZE     = 0xDC04;
static constexpr uint16_t DTYPE_STRING         = 0xFFFF;
static constexpr uint16_t DTYPE_UINT64         = 0x0008;

// ─── Constants ────────────────────────────────────────────────────────────────
static constexpr uint32_t CONTAINER_HEADER_SIZE  = 12;
static constexpr uint32_t ROOT_PARENT            = 0xFFFFFFFF;
static constexpr uint32_t ALL_STORAGE            = 0xFFFFFFFF;
static constexpr uint32_t ALL_FORMATS            = 0x00000000;
static constexpr uint32_t SESSION_ID             = 1;

// Nintendo Switch USB identifiers
static constexpr uint16_t NINTENDO_VID  = 0x057e;
static constexpr uint16_t SWITCH_PID_V1 = 0x2000;
static constexpr uint16_t SWITCH_PID_V2 = 0x3000;   // Switch V2/Lite/OLED

// USB MTP class/subclass/protocol
static constexpr uint8_t MTP_CLASS    = 6;
static constexpr uint8_t MTP_SUBCLASS = 1;
static constexpr uint8_t MTP_PROTOCOL = 1;

// Transfer sizing
// 512 KiB per bulk transfer — safe across all macOS USB controllers and Switch firmware.
// 1 MiB can hit IOUSBLib scatter-gather limits on some hosts.
// USB 2.0 HS: max_pkt = 512  → use go-mtpfs's rwBufSize = 16 KiB
// USB 3.0 SS: max_pkt = 1024 → scale up; each syscall has ~100 µs overhead,
//   so 16 KiB chunks cap throughput at ~164 MB/s even with USB 3.0 bandwidth.
//   512 KiB chunks drop syscall count 32× and saturate USB 3.0 properly.
static constexpr size_t  CHUNK_SIZE          = 0x4000;          // 16 KiB (USB 2.0 HS baseline)
static constexpr size_t  CHUNK_SIZE_USB3     = 512u << 10;      // 512 KiB (USB 3.0 SS)
static constexpr size_t  SMALL_CHUNK_SIZE    = 0x1000;          // 4 KiB

// Timeouts
static constexpr int TIMEOUT_CMD_MS      = 5000;    // command / response containers
static constexpr int TIMEOUT_READ_MS     = 10000;   // bulk read per chunk
// go-mtpfs uses d.Timeout = 2000 ms for every bulk transfer.
// 2 s at 1 MB/s = 2 GB/s worth of leeway; generous but not infinitely blocking.
static constexpr int TIMEOUT_WRITE_MS    = 2000;    // bulk write per chunk
// After the last byte of a file is sent, DBI/Sphaira verify NCAs, write to
// the content database, update storage info, etc. On a slow microSD this can
// take well over 10 seconds, so we give it 3 minutes before calling it dead.
static constexpr int TIMEOUT_INSTALL_MS  = 180000;
static constexpr int CONNECT_TIMEOUT_MS  = 10000;
static constexpr int TIMEOUT_MS = TIMEOUT_CMD_MS;

// ─── High-level data structures ───────────────────────────────────────────────
struct StorageInfo {
    uint16_t    storage_type;
    uint16_t    filesystem_type;
    uint16_t    access_capability;
    uint64_t    max_capacity;
    uint64_t    free_space_bytes;
    uint32_t    free_space_objects;
    std::string description;
    std::string volume_id;
};

struct ObjectInfo {
    uint32_t    storage_id;
    uint16_t    object_format;
    uint16_t    protection_status;
    uint64_t    compressed_size;      // 64-bit to handle files > 4 GiB
    uint32_t    parent_object;
    uint16_t    association_type;
    uint32_t    association_desc;
    uint32_t    sequence_number;
    std::string filename;
    std::string date_created;
    std::string date_modified;

    bool is_directory() const {
        return object_format == static_cast<uint16_t>(ObjectFormat::Association);
    }
};

struct DeviceInfo {
    uint16_t              standard_version;
    uint32_t              vendor_extension_id;
    uint16_t              vendor_extension_version;
    std::string           vendor_extension_desc;
    uint16_t              functional_mode;
    std::vector<uint16_t> operations_supported;
    std::vector<uint16_t> events_supported;
    std::vector<uint16_t> device_props_supported;
    std::vector<uint16_t> capture_formats;
    std::vector<uint16_t> playback_formats;
    std::string           manufacturer;
    std::string           model;
    std::string           device_version;
    std::string           serial_number;
};

// ─── Exception ────────────────────────────────────────────────────────────────
class MTPException : public std::runtime_error {
public:
    MTPException(ResponseCode code, std::string msg)
        : std::runtime_error(std::move(msg)), code_(code) {}

    ResponseCode code() const { return code_; }

    static const char* code_name(ResponseCode c) {
        switch (c) {
            case ResponseCode::OK:                    return "OK";
            case ResponseCode::GeneralError:          return "General Error";
            case ResponseCode::SessionNotOpen:        return "Session Not Open";
            case ResponseCode::OperationNotSupported: return "Operation Not Supported";
            case ResponseCode::ParameterNotSupported: return "Parameter Not Supported";
            case ResponseCode::IncompleteTransfer:    return "Incomplete Transfer";
            case ResponseCode::InvalidStorageID:      return "Invalid Storage ID";
            case ResponseCode::InvalidObjectHandle:   return "Invalid Object Handle";
            case ResponseCode::StoreFull:             return "Store Full";
            case ResponseCode::AccessDenied:          return "Access Denied";
            case ResponseCode::DeviceBusy:            return "Device Busy";
            case ResponseCode::InvalidParentObject:   return "Invalid Parent Object";
            case ResponseCode::InvalidParameter:      return "Invalid Parameter";
            case ResponseCode::SessionAlreadyOpen:    return "Session Already Open";
            case ResponseCode::TransactionCancelled:  return "Transaction Cancelled";
            default:                                  return "Unknown Error";
        }
    }

private:
    ResponseCode code_;
};

} // namespace mtp
