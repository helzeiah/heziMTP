#include "TransferEngine.hpp"
#include <filesystem>
#include <format>
#include <random>
#include <sstream>
#include <iomanip>

namespace fs = std::filesystem;

namespace transfer {

std::string TransferEngine::make_id() {
    static std::mt19937_64 rng{std::random_device{}()};
    static std::uniform_int_distribution<uint64_t> dist;
    std::ostringstream ss;
    ss << std::hex << std::setw(16) << std::setfill('0') << dist(rng);
    return ss.str();
}

TransferEngine::TransferEngine(mtp::MTPSession& session)
    : session_(session), ops_(session)
{
    worker_ = std::jthread([this](std::stop_token) { worker_loop(); });
}

TransferEngine::~TransferEngine() {
    shutdown_.store(true);
    cv_.notify_all();
    // std::jthread destructor requests stop and joins automatically
}

std::shared_ptr<TransferItem>
TransferEngine::enqueue_download(uint32_t handle, uint32_t storage_id,
                                  const std::string& dest_path,
                                  const std::string& filename,
                                  uint64_t expected_size) {
    auto item = std::make_shared<TransferItem>();
    item->id           = make_id();
    item->direction    = Direction::Download;
    item->filename     = filename;
    item->total_bytes  = expected_size;
    item->object_handle = handle;
    item->storage_id   = storage_id;
    item->dest_path    = dest_path;

    std::lock_guard lock(mtx_);
    history_.push_back(item);
    pending_.push(item);
    cv_.notify_one();
    return item;
}

std::shared_ptr<TransferItem>
TransferEngine::enqueue_upload(const std::string& src_path,
                                uint32_t parent_handle, uint32_t storage_id,
                                const std::string& filename,
                                uint64_t file_size) {
    auto item = std::make_shared<TransferItem>();
    item->id           = make_id();
    item->direction    = Direction::Upload;
    item->filename     = filename;
    item->total_bytes  = file_size;
    item->src_path     = src_path;
    item->parent_handle = parent_handle;
    item->storage_id   = storage_id;

    std::lock_guard lock(mtx_);
    history_.push_back(item);
    pending_.push(item);
    cv_.notify_one();
    return item;
}

void TransferEngine::cancel(const std::string& transfer_id) {
    std::lock_guard lock(mtx_);
    for (auto& item : history_) {
        if (item->id == transfer_id) {
            item->cancel.store(true);
            break;
        }
    }
}

void TransferEngine::cancel_all() {
    std::lock_guard lock(mtx_);
    for (auto& item : history_)
        item->cancel.store(true);
}

std::vector<std::shared_ptr<TransferItem>> TransferEngine::all_transfers() const {
    std::lock_guard lock(mtx_);
    return history_;
}

bool TransferEngine::has_active() const {
    std::lock_guard lock(mtx_);
    for (auto& item : history_)
        if (item->state.load() == State::Active) return true;
    return false;
}

void TransferEngine::worker_loop() {
    while (!shutdown_.load()) {
        std::shared_ptr<TransferItem> item;
        {
            std::unique_lock lock(mtx_);
            cv_.wait(lock, [&]{ return shutdown_.load() || !pending_.empty(); });
            if (shutdown_.load()) break;
            if (pending_.empty()) continue;
            item = pending_.front();
            pending_.pop();
        }

        if (item->cancel.load()) {
            item->state.store(State::Cancelled);
            continue;
        }

        item->state.store(State::Active);
        item->started_at = std::chrono::steady_clock::now();

        try {
            if (item->direction == Direction::Download)
                run_download(*item);
            else
                run_upload(*item);

            if (item->cancel.load())
                item->state.store(State::Cancelled);
            else
                item->state.store(State::Done);
        } catch (const mtp::MTPException& e) {
            item->error_msg = e.what();
            item->state.store(State::Failed);
        } catch (const std::exception& e) {
            item->error_msg = e.what();
            item->state.store(State::Failed);
        }

        item->finished_at = std::chrono::steady_clock::now();
    }
}

void TransferEngine::run_download(TransferItem& item) {
    // Ensure destination directory exists
    if (auto parent = fs::path(item.dest_path).parent_path(); !parent.empty())
        fs::create_directories(parent);

    mtp::ProgressFn progress = [&](uint64_t done, uint64_t total) {
        item.bytes_done.store(done);
        if (total && !item.total_bytes) item.total_bytes = total;
    };

    ops_.get_object(item.object_handle, item.dest_path, item.total_bytes,
                    progress, item.cancel);
}

void TransferEngine::run_upload(TransferItem& item) {
    mtp::ProgressFn progress = [&](uint64_t done, uint64_t /*total*/) {
        item.bytes_done.store(done);
    };

    ops_.send_object(item.storage_id, item.parent_handle,
                     item.src_path, item.filename, item.total_bytes,
                     progress, item.cancel);
}

} // namespace transfer
