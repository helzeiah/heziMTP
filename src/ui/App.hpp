#pragma once
#include "mtp/MTPSession.hpp"
#include "mtp/MTPOperations.hpp"
#include "mtp/MTPProtocol.hpp"
#include "transfer/TransferEngine.hpp"
#include <atomic>
#include <filesystem>
#include <memory>
#include <mutex>
#include <set>
#include <string>
#include <thread>
#include <vector>

namespace heziMTP {

namespace fs = std::filesystem;

// ── Entry in a file browser panel ────────────────────────────────────────────
struct LocalEntry {
    fs::path    path;
    std::string name;
    bool        is_dir   = false;
    uint64_t    size     = 0;
    std::string size_str;
    std::string date_str;
};

struct RemoteEntry {
    uint32_t    handle     = 0;
    uint32_t    storage_id = 0;
    std::string name;
    bool        is_dir     = false;
    uint64_t    size       = 0;
    std::string size_str;
    std::string date_str;
};

// Breadcrumb stack item
struct NavEntry {
    uint32_t    handle = 0;     // mtp::ROOT_PARENT = root of storage
    uint32_t    storage_id = 0;
    std::string label;
};

// ── Main application ──────────────────────────────────────────────────────────
class App {
public:
    App();
    ~App();

    void init();
    void shutdown();

    // ── Web bridge API ────────────────────────────────────────────────────────
    std::string state_json() const;
    std::string current_local_path() const;
    void navigate_local(const std::string& path);
    void navigate_local_up();
    void navigate_remote(uint32_t handle, uint32_t storage_id, const std::string& name);
    void navigate_remote_up();
    void navigate_remote_to(size_t idx);
    void set_active_storage(uint32_t storage_id);
    void request_refresh_remote();
    void do_upload(const std::string& src_path);
    void do_download(uint32_t handle, uint32_t storage_id, const std::string& filename, uint64_t size);
    void do_cancel(const std::string& id);
    void do_cancel_all();
    void do_delete(uint32_t handle);
    void do_create_folder(const std::string& name);
    void reconnect();

    // Called from the OS drag-and-drop callback (may be on a different thread).
    void enqueue_finder_drop(const std::string& path);

private:
    // ── Local filesystem ──────────────────────────────────────────────────────
    void refresh_local(const fs::path& path);

    // ── Remote filesystem ─────────────────────────────────────────────────────
    void refresh_remote();

    // ── Transfer helpers ──────────────────────────────────────────────────────
    void start_download(uint32_t handle, uint32_t storage_id,
                        const std::string& filename, uint64_t size);
    void start_upload(const fs::path& src_path, uint64_t file_size);

    // ── Device connection loop (runs on background thread) ────────────────────
    void device_monitor_loop(std::stop_token st);
    void on_device_connected(mtp::DeviceInfo di);
    void on_device_disconnected();

    // ── Status bar ────────────────────────────────────────────────────────────
    void set_status(const std::string& msg, float duration_sec = 5.f);

    // ── Finder drop processing (called from state_json) ───────────────────────
    void process_finder_drops();

    // ── State: connection ─────────────────────────────────────────────────────
    std::unique_ptr<mtp::MTPSession>    session_;
    std::unique_ptr<mtp::MTPOperations> ops_;
    std::unique_ptr<transfer::TransferEngine> engine_;

    std::atomic<bool>   connected_{false};
    std::jthread        monitor_thread_;
    mutable std::mutex  device_mtx_;
    mtp::DeviceInfo     device_info_;

    std::vector<std::pair<uint32_t, mtp::StorageInfo>> storages_;
    uint32_t active_storage_ = 0;

    // ── State: local panel ────────────────────────────────────────────────────
    fs::path                local_path_;
    std::vector<LocalEntry> local_entries_;

    // ── State: remote panel ───────────────────────────────────────────────────
    std::vector<NavEntry>    remote_nav_stack_;
    std::vector<RemoteEntry> remote_entries_;
    bool                     remote_needs_refresh_ = true;
    bool                     remote_refreshing_    = false;

    // Status bar message with timeout
    std::string status_msg_;
    float       status_msg_timer_ = 0.f;

    // Files/folders dropped from macOS Finder.
    // Written from the drop callback thread, read + cleared elsewhere.
    std::vector<std::string> finder_drops_;
    std::mutex               finder_drops_mtx_;
};

} // namespace heziMTP
