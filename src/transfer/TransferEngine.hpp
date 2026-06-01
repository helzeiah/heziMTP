#pragma once
#include "mtp/MTPSession.hpp"
#include "mtp/MTPOperations.hpp"
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <functional>
#include <memory>
#include <mutex>
#include <queue>
#include <string>
#include <thread>
#include <vector>

namespace transfer {

enum class Direction { Download, Upload };
enum class State     { Queued, Active, Done, Failed, Cancelled };

struct TransferItem {
    std::string  id;
    std::string  filename;
    Direction    direction;
    uint64_t     total_bytes  = 0;
    std::string  error_msg;

    std::atomic<uint64_t> bytes_done{0};
    std::atomic<State>    state{State::Queued};
    std::atomic<bool>     cancel{false};

    std::chrono::steady_clock::time_point started_at;
    std::chrono::steady_clock::time_point finished_at;

    float progress() const {
        if (total_bytes == 0) return 0.f;
        return static_cast<float>(bytes_done.load()) / static_cast<float>(total_bytes);
    }

    // Bytes per second since transfer started
    double speed_bps() const {
        if (state.load() != State::Active) return 0.0;
        auto elapsed = std::chrono::duration<double>(
            std::chrono::steady_clock::now() - started_at).count();
        return elapsed > 0.0 ? static_cast<double>(bytes_done.load()) / elapsed : 0.0;
    }

    double eta_seconds() const {
        double sp = speed_bps();
        if (sp <= 0) return -1.0;
        uint64_t remaining = total_bytes > bytes_done.load()
                           ? total_bytes - bytes_done.load() : 0;
        return static_cast<double>(remaining) / sp;
    }

    // Download-specific fields
    uint32_t    object_handle  = 0;
    uint32_t    storage_id     = 0;
    std::string dest_path;

    // Upload-specific fields
    std::string src_path;
    uint32_t    parent_handle  = 0;
};

// Single-worker transfer engine.  MTP is inherently serial so one worker is
// correct; the worker uses dedicated operations to keep the mutex clean.
class TransferEngine {
public:
    explicit TransferEngine(mtp::MTPSession& session);
    ~TransferEngine();

    // Returns a shared_ptr to the queued item (so the UI can observe progress).
    std::shared_ptr<TransferItem>
    enqueue_download(uint32_t handle, uint32_t storage_id,
                     const std::string& dest_path,
                     const std::string& filename,
                     uint64_t expected_size);

    std::shared_ptr<TransferItem>
    enqueue_upload(const std::string& src_path,
                   uint32_t parent_handle, uint32_t storage_id,
                   const std::string& filename,
                   uint64_t file_size);

    void cancel(const std::string& transfer_id);
    void cancel_all();

    // Thread-safe snapshot for the UI to display
    std::vector<std::shared_ptr<TransferItem>> all_transfers() const;
    bool has_active() const;

private:
    void worker_loop();
    void run_download(TransferItem& item);
    void run_upload(TransferItem& item);

    mtp::MTPSession&    session_;
    mtp::MTPOperations  ops_;

    mutable std::mutex              mtx_;
    std::condition_variable         cv_;
    std::queue<std::shared_ptr<TransferItem>> pending_;
    std::vector<std::shared_ptr<TransferItem>> history_; // all items ever queued

    std::jthread   worker_;
    std::atomic<bool> shutdown_{false};
    static std::string make_id();
};

} // namespace transfer
