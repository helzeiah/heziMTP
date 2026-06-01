#include "App.hpp"
#include <cstdio>
#include <filesystem>
#include <format>
#include <algorithm>
#include <chrono>
#include <thread>
#include <sys/stat.h>
#include <cstdlib>
#include <cstring>

namespace fs = std::filesystem;
namespace heziMTP {

// ─── Static helpers ───────────────────────────────────────────────────────────

static std::string date_str_from_fs(const fs::directory_entry& e) {
    struct ::stat st{};
    if (::stat(e.path().c_str(), &st) == 0) {
        char buf[32];
        std::strftime(buf, sizeof(buf), "%b %e, %Y", std::localtime(&st.st_mtime));
        return buf;
    }
    return "";
}

using CrumbList = std::vector<std::pair<std::string, fs::path>>;
static CrumbList build_crumbs(const fs::path& path) {
    std::vector<std::pair<std::string, fs::path>> segs;
    fs::path p = path;
    while (true) {
        auto par = p.parent_path();
        if (par == p) break; // reached root — skip it
        segs.insert(segs.begin(), {p.filename().string(), p});
        p = par;
    }
    return segs;
}

static std::string jstr(const std::string& s) {
    std::string r; r.reserve(s.size()+4);
    for (char c : s) {
        if (c=='"')  r+="\\\"";
        else if (c=='\\') r+="\\\\";
        else if (c=='\n') r+="\\n";
        else if (c=='\r') r+="\\r";
        else if (c=='\t') r+="\\t";
        else r+=c;
    }
    return r;
}

static std::string fmt_size(uint64_t b) {
    if (!b) return "0 B";
    if (b<1024) return std::to_string(b)+" B";
    if (b<(1u<<20)) return std::to_string(b/1024)+" KB";
    if (b<(1ull<<30)) { char x[24]; snprintf(x,sizeof(x),"%.1f MB",b/1048576.0); return x; }
    char x[24]; snprintf(x,sizeof(x),"%.2f GB",b/1073741824.0); return x;
}

static std::string fmt_speed(double bps) {
    char b[32];
    if (bps>1048576) snprintf(b,sizeof(b),"%.0f MB/s",bps/1048576.0);
    else             snprintf(b,sizeof(b),"%.0f KB/s",bps/1024.0);
    return b;
}

static std::string fmt_eta(double s) {
    if (s<0) return "--:--";
    int si = (int)s;
    if (si<60)   { char b[16]; snprintf(b,sizeof(b),"0:%02d",si); return b; }
    if (si<3600) { char b[16]; snprintf(b,sizeof(b),"%d:%02d",si/60,si%60); return b; }
    char b[16]; snprintf(b,sizeof(b),"%dh%02dm",si/3600,(si%3600)/60); return b;
}

// ─── App lifecycle ────────────────────────────────────────────────────────────
App::App() : local_path_(fs::path(std::getenv("HOME") ? std::getenv("HOME") : "/")) {}
App::~App() { shutdown(); }

void App::init() {
    local_path_ = fs::path(std::getenv("HOME") ? std::getenv("HOME") : "/");
    refresh_local(local_path_);
    monitor_thread_ = std::jthread([this](std::stop_token st) { device_monitor_loop(st); });
}

void App::shutdown() {
    monitor_thread_.request_stop();
    engine_.reset();
    if (connected_.load())
        try { if (session_) session_->disconnect(); } catch (...) {}
}

// ─── Device monitor ───────────────────────────────────────────────────────────
void App::device_monitor_loop(std::stop_token st) {
    while (!st.stop_requested()) {
        if (!connected_.load()) {
            try {
                auto sess = std::make_unique<mtp::MTPSession>();
                auto di   = sess->connect();
                {
                    std::lock_guard lock(device_mtx_);
                    session_ = std::move(sess);
                    ops_     = std::make_unique<mtp::MTPOperations>(*session_);
                    engine_  = std::make_unique<transfer::TransferEngine>(*session_);
                }
                on_device_connected(std::move(di));
            } catch (...) {}
        } else {
            bool transfer_active = false;
            {
                std::lock_guard lock(device_mtx_);
                transfer_active = engine_ && engine_->has_active();
            }
            if (!transfer_active) {
                mtp::MTPOperations* ops_raw = nullptr;
                {
                    std::lock_guard lock(device_mtx_);
                    ops_raw = ops_.get();
                }
                if (ops_raw) {
                    try { ops_raw->get_storage_ids(); }
                    catch (...) { on_device_disconnected(); }
                }
            }
        }
        for (int i = 0; i < 8 && !st.stop_requested(); ++i)
            std::this_thread::sleep_for(std::chrono::milliseconds(250));
    }
}

void App::on_device_connected(mtp::DeviceInfo di) {
    {
        std::lock_guard lock(device_mtx_);
        device_info_ = std::move(di);
        storages_.clear();
        remote_nav_stack_.clear();
        remote_entries_.clear();
        try {
            auto ids = ops_->get_storage_ids();
            for (auto id : ids) {
                auto si = ops_->get_storage_info(id);
                storages_.emplace_back(id, std::move(si));
            }
            if (!storages_.empty()) {
                active_storage_ = storages_.front().first;
                const auto& si  = storages_.front().second;
                std::string label = si.description.empty()
                    ? std::format("Storage {:04X}", active_storage_)
                    : si.description;
                remote_nav_stack_.push_back({mtp::ROOT_PARENT, active_storage_, label});
            }
        } catch (...) {}
    }
    connected_.store(true);
    remote_needs_refresh_ = true;
    set_status(std::format("Connected  {}", device_info_.model));
    refresh_remote();
}

void App::on_device_disconnected() {
    {
        std::lock_guard lock(device_mtx_);
        if (engine_) { engine_->cancel_all(); engine_.reset(); }
        ops_.reset();
        if (session_) { try { session_->disconnect(); } catch (...) {} session_.reset(); }
        storages_.clear();
        remote_entries_.clear();
        remote_nav_stack_.clear();
    }
    connected_.store(false);
    remote_needs_refresh_ = false;
    set_status("Switch disconnected");
}

// ─── Local filesystem ─────────────────────────────────────────────────────────
void App::refresh_local(const fs::path& path) {
    local_entries_.clear();
    try {
        for (auto& e : fs::directory_iterator(path,
                fs::directory_options::skip_permission_denied)) {
            LocalEntry le{};
            le.path     = e.path();
            le.name     = e.path().filename().string();
            le.is_dir   = e.is_directory();
            if (!le.is_dir) { le.size = e.file_size(); le.size_str = fmt_size(le.size); }
            le.date_str = date_str_from_fs(e);
            local_entries_.push_back(std::move(le));
        }
        std::sort(local_entries_.begin(), local_entries_.end(), [](auto& a, auto& b) {
            if (a.is_dir != b.is_dir) return a.is_dir > b.is_dir;
            return a.name < b.name;
        });
    } catch (...) {}
    local_path_ = path;
}

// ─── Remote filesystem ────────────────────────────────────────────────────────
void App::refresh_remote() {
    if (!connected_.load() || remote_refreshing_) return;
    remote_refreshing_ = true;
    remote_entries_.clear();
    try {
        std::lock_guard lock(device_mtx_);
        if (!ops_ || remote_nav_stack_.empty()) { remote_refreshing_ = false; return; }
        const auto& top = remote_nav_stack_.back();
        auto handles = ops_->get_object_handles(top.storage_id, top.handle);
        for (auto h : handles) {
            auto oi = ops_->get_object_info(h);
            RemoteEntry re{};
            re.handle     = h;
            re.storage_id = oi.storage_id;
            re.name       = oi.filename;
            re.is_dir     = oi.is_directory();
            re.size       = oi.compressed_size;
            re.size_str   = re.is_dir ? "" : fmt_size(re.size);
            re.date_str   = oi.date_modified.empty() ? oi.date_created : oi.date_modified;
            remote_entries_.push_back(std::move(re));
        }
        std::sort(remote_entries_.begin(), remote_entries_.end(), [](auto& a, auto& b) {
            if (a.is_dir != b.is_dir) return a.is_dir > b.is_dir;
            return a.name < b.name;
        });
    } catch (const mtp::MTPException& e) {
        set_status(std::format("Error listing: {}", e.what()));
    }
    remote_refreshing_    = false;
    remote_needs_refresh_ = false;
}

// ─── Transfers / misc ─────────────────────────────────────────────────────────
void App::start_download(uint32_t handle, uint32_t storage_id,
                          const std::string& filename, uint64_t size) {
    if (!engine_) return;
    engine_->enqueue_download(handle, storage_id,
        (local_path_ / filename).string(), filename, size);
    set_status(std::format("Downloading  {}", filename));
}
void App::start_upload(const fs::path& src, uint64_t sz) {
    if (!engine_ || remote_nav_stack_.empty()) return;
    const auto& top = remote_nav_stack_.back();
    engine_->enqueue_upload(src.string(), top.handle, top.storage_id,
        src.filename().string(), sz);
    set_status(std::format("Uploading  {}", src.filename().string()));
}
void App::set_status(const std::string& msg, float dur) {
    status_msg_ = msg; status_msg_timer_ = dur;
}
void App::enqueue_finder_drop(const std::string& path) {
    std::lock_guard lock(finder_drops_mtx_);
    finder_drops_.push_back(path);
}

// ─── Web bridge API ───────────────────────────────────────────────────────────

std::string App::current_local_path() const { return local_path_.string(); }

std::string App::state_json() const {
    // Process any pending finder drops
    {
        const_cast<App*>(this)->process_finder_drops();
    }

    bool conn = connected_.load();
    std::string dn;
    std::vector<std::pair<uint32_t,mtp::StorageInfo>> stor;
    uint32_t astor;
    std::vector<NavEntry> nav;
    bool refreshing;
    {
        std::lock_guard lock(device_mtx_);
        dn = device_info_.model;
        stor = storages_;
        astor = active_storage_;
        nav = remote_nav_stack_;
        refreshing = remote_refreshing_;
    }

    std::string j = "{";
    j += "\"connected\":" + std::string(conn?"true":"false") + ",";
    j += "\"deviceName\":\"" + jstr(dn) + "\",";
    j += "\"storages\":[";
    for (size_t i=0;i<stor.size();++i) {
        auto& [id,si]=stor[i];
        j+="{\"id\":"+std::to_string(id)+",\"name\":\""+jstr(si.description)+"\",";
        j+="\"freeBytes\":"+std::to_string(si.free_space_bytes)+",\"freeStr\":\""+fmt_size(si.free_space_bytes)+"\",";
        j+="\"totalBytes\":"+std::to_string(si.max_capacity)+",\"totalStr\":\""+fmt_size(si.max_capacity)+"\"}";
        if (i+1<stor.size()) j+=",";
    }
    j+="],\"activeStorageId\":"+std::to_string(astor)+",";
    j+="\"remoteNavStack\":[";
    for (size_t i=0;i<nav.size();++i) {
        j+="{\"handle\":"+std::to_string(nav[i].handle)+",\"storageId\":"+std::to_string(nav[i].storage_id)+",\"label\":\""+jstr(nav[i].label)+"\"}";
        if (i+1<nav.size()) j+=",";
    }
    j+="],\"localPath\":\""+jstr(local_path_.string())+"\",\"localFiles\":[";
    for (size_t i=0;i<local_entries_.size();++i) {
        auto& e=local_entries_[i];
        j+="{\"name\":\""+jstr(e.name)+"\",\"isDir\":"+std::string(e.is_dir?"true":"false")+",";
        j+="\"size\":"+std::to_string(e.size)+",\"sizeStr\":\""+jstr(e.size_str)+"\",";
        j+="\"date\":\""+jstr(e.date_str)+"\",\"path\":\""+jstr(e.path.string())+"\"}";
        if (i+1<local_entries_.size()) j+=",";
    }
    j+="],\"remoteRefreshing\":"+std::string(refreshing?"true":"false")+",\"remoteFiles\":[";
    for (size_t i=0;i<remote_entries_.size();++i) {
        auto& e=remote_entries_[i];
        j+="{\"handle\":"+std::to_string(e.handle)+",\"storageId\":"+std::to_string(e.storage_id)+",";
        j+="\"name\":\""+jstr(e.name)+"\",\"isDir\":"+std::string(e.is_dir?"true":"false")+",";
        j+="\"size\":"+std::to_string(e.size)+",\"sizeStr\":\""+jstr(e.size_str)+"\",\"date\":\""+jstr(e.date_str)+"\"}";
        if (i+1<remote_entries_.size()) j+=",";
    }
    j+="],\"transfers\":[";
    if (engine_) {
        auto items=engine_->all_transfers();
        for (size_t i=0;i<items.size();++i) {
            auto& t=items[i];
            auto st=t->state.load();
            char pr[16]; snprintf(pr,sizeof(pr),"%.4f",t->progress());
            j+="{\"id\":\""+jstr(t->id)+"\",\"filename\":\""+jstr(t->filename)+"\",";
            j+="\"direction\":"+std::to_string((int)t->direction)+",\"state\":"+std::to_string((int)st)+",";
            j+="\"bytesDone\":"+std::to_string(t->bytes_done.load())+",\"totalBytes\":"+std::to_string(t->total_bytes)+",";
            j+="\"progress\":"+std::string(pr)+",\"speedStr\":\""+fmt_speed(t->speed_bps())+"\",";
            j+="\"speedBps\":"+std::to_string((uint64_t)t->speed_bps())+",";
            j+="\"etaStr\":\""+fmt_eta(t->eta_seconds())+"\",\"error\":\""+jstr(t->error_msg)+"\"}";
            if (i+1<items.size()) j+=",";
        }
    }
    j+="],\"statusMessage\":\""+jstr(status_msg_)+"\",";
    char tm[16]; snprintf(tm,sizeof(tm),"%.2f",(double)status_msg_timer_);
    j+="\"statusTimer\":"+std::string(tm)+"}";
    return j;
}

void App::process_finder_drops() {
    std::vector<std::string> drops;
    {
        std::lock_guard lock(finder_drops_mtx_);
        drops.swap(finder_drops_);
    }
    for (auto& dropped : drops) {
        fs::path p(dropped);
        std::error_code ec;
        if (fs::is_directory(p, ec)) {
            refresh_local(p);
        } else if (!ec && connected_.load()) {
            auto sz = fs::file_size(p, ec);
            if (!ec) start_upload(p, sz);
        }
    }
}

void App::navigate_local(const std::string& p) {
    refresh_local(fs::path(p == "~" ? (std::getenv("HOME") ? std::getenv("HOME") : "/") : p));
}
void App::navigate_local_up() {
    if (local_path_.has_parent_path() && local_path_ != local_path_.parent_path())
        refresh_local(local_path_.parent_path());
}
void App::navigate_remote(uint32_t h, uint32_t s, const std::string& name) {
    remote_nav_stack_.push_back({h, s, name});
    refresh_remote();
}
void App::navigate_remote_up() {
    if (remote_nav_stack_.size() > 1) {
        remote_nav_stack_.pop_back();
        refresh_remote();
    }
}
void App::navigate_remote_to(size_t idx) {
    if (idx < remote_nav_stack_.size()) {
        remote_nav_stack_.resize(idx+1);
        refresh_remote();
    }
}
void App::set_active_storage(uint32_t id) {
    std::string label;
    {
        std::lock_guard lock(device_mtx_);
        for (auto& [sid,si]: storages_) if (sid==id) { label=si.description; break; }
        active_storage_ = id;
    }
    remote_nav_stack_ = {{mtp::ROOT_PARENT, id, label.empty()?std::format("{:04X}",id):label}};
    refresh_remote();
}
void App::request_refresh_remote() { refresh_remote(); }
void App::do_upload(const std::string& src) {
    fs::path p(src);
    std::error_code ec;
    auto sz = fs::file_size(p, ec);
    if (!ec) start_upload(p, sz);
}
void App::do_download(uint32_t h, uint32_t s, const std::string& fn, uint64_t sz) {
    start_download(h, s, fn, sz);
}
void App::do_cancel(const std::string& id) { if (engine_) engine_->cancel(id); }
void App::do_cancel_all() { if (engine_) engine_->cancel_all(); }
void App::do_delete(uint32_t h) {
    try { std::lock_guard lock(device_mtx_); if (ops_) ops_->delete_object(h); }
    catch (...) {}
    refresh_remote();
}
void App::do_create_folder(const std::string& name) {
    if (remote_nav_stack_.empty()) return;
    const auto& top = remote_nav_stack_.back();
    try { std::lock_guard lock(device_mtx_); if (ops_) ops_->create_directory(top.storage_id, top.handle, name); }
    catch (...) {}
    refresh_remote();
}
void App::reconnect() { on_device_disconnected(); }

} // namespace heziMTP
