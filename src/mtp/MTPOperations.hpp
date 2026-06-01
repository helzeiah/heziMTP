#pragma once
#include "MTPSession.hpp"
#include "MTPProtocol.hpp"
#include <cstdint>
#include <string>
#include <vector>

namespace mtp {

// Stateless MTP operations that run on top of an MTPSession.
// Every public function acquires the session mutex.
class MTPOperations {
public:
    explicit MTPOperations(MTPSession& session);

    // ── Storage ───────────────────────────────────────────────────────────────
    std::vector<uint32_t> get_storage_ids();
    StorageInfo           get_storage_info(uint32_t storage_id);

    // ── Object enumeration ────────────────────────────────────────────────────
    // parent_handle = ROOT_PARENT for root of a storage
    std::vector<uint32_t> get_object_handles(uint32_t storage_id,
                                              uint32_t parent_handle = ROOT_PARENT);
    ObjectInfo            get_object_info(uint32_t handle);

    // ── Transfer ──────────────────────────────────────────────────────────────
    // Download object to a local file path.
    void get_object(uint32_t handle,
                    const std::string& dest_path,
                    uint64_t expected_size,
                    const ProgressFn& progress,
                    std::atomic<bool>& cancel);

    // Upload a local file, returns new object handle on the device.
    uint32_t send_object(uint32_t storage_id,
                         uint32_t parent_handle,
                         const std::string& src_path,
                         const std::string& filename,
                         uint64_t file_size,
                         const ProgressFn& progress,
                         std::atomic<bool>& cancel);

    // ── Management ────────────────────────────────────────────────────────────
    void delete_object(uint32_t handle);

    // Create a directory (association object).
    uint32_t create_directory(uint32_t storage_id,
                              uint32_t parent_handle,
                              const std::string& name);

private:
    MTPSession& session_;

    // Build a packed ObjectInfo blob for SendObjectInfo
    static std::vector<uint8_t> build_object_info(uint32_t storage_id,
                                                    uint32_t parent_handle,
                                                    const std::string& filename,
                                                    uint64_t file_size,
                                                    uint16_t format);

    static void encode_mtp_string(std::vector<uint8_t>& buf, const std::string& utf8);
    static void put_le16(std::vector<uint8_t>& b, uint16_t v);
    static void put_le32(std::vector<uint8_t>& b, uint32_t v);
    static void put_le64(std::vector<uint8_t>& b, uint64_t v);

    static std::vector<uint32_t> parse_u32_array(const std::vector<uint8_t>& data);
    static ObjectInfo            parse_object_info(const std::vector<uint8_t>& data);
};

} // namespace mtp
