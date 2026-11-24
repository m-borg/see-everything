#pragma once

#include "IndexStore.hpp"
#include "Scanner.hpp"

#include <CoreServices/CoreServices.h>
#include <atomic>
#include <condition_variable>
#include <dispatch/dispatch.h>
#include <functional>
#include <mutex>
#include <string>
#include <thread>
#include <unordered_map>
#include <vector>

namespace se {

class FSEventsWatcher {
public:
    using ChangeCallback = std::function<void(uint64_t latest_event_id)>;

    FSEventsWatcher() = default;
    ~FSEventsWatcher() { stop(); }

    static uint64_t current_system_event_id() {
        return static_cast<uint64_t>(FSEventsGetCurrentEventId());
    }

    bool start(IndexStore *store,
               const std::vector<std::string> &roots,
               uint64_t since_event_id,
               ChangeCallback on_change) {
        stop();
        store_ = store;
        on_change_ = std::move(on_change);
        running_.store(true, std::memory_order_release);

        // Start background sync worker thread (coalesces directory syncs cleanly off the dispatch queue)
        worker_ = std::thread([this]() { worker_loop(); });

        CFMutableArrayRef cf_paths = CFArrayCreateMutable(kCFAllocatorDefault, 0, &kCFTypeArrayCallBacks);
        for (const auto &r : roots) {
            CFStringRef s = CFStringCreateWithCString(kCFAllocatorDefault, r.c_str(), kCFStringEncodingUTF8);
            if (s) {
                CFArrayAppendValue(cf_paths, s);
                CFRelease(s);
            }
        }

        FSEventStreamContext ctx{};
        ctx.info = this;

        FSEventStreamEventId start_id = (since_event_id == 0)
            ? kFSEventStreamEventIdSinceNow
            : static_cast<FSEventStreamEventId>(since_event_id);

        FSEventStreamCreateFlags flags = kFSEventStreamCreateFlagNoDefer
                                       | kFSEventStreamCreateFlagWatchRoot
                                       | kFSEventStreamCreateFlagFileEvents;

        stream_ = FSEventStreamCreate(
            kCFAllocatorDefault,
            &FSEventsWatcher::fs_callback,
            &ctx,
            cf_paths,
            start_id,
            0.15, // 150ms coalescing latency — snappy yet avoids thrashing during builds
            flags
        );
        CFRelease(cf_paths);

        if (!stream_) {
            stop();
            return false;
        }

        queue_ = dispatch_queue_create("com.seeeverything.fsevents", DISPATCH_QUEUE_SERIAL);
        FSEventStreamSetDispatchQueue(stream_, queue_);
        if (!FSEventStreamStart(stream_)) {
            stop();
            return false;
        }

        active_.store(true, std::memory_order_release);
        return true;
    }

    void stop() {
        active_.store(false, std::memory_order_release);
        if (stream_) {
            FSEventStreamStop(stream_);
            FSEventStreamInvalidate(stream_);
            FSEventStreamRelease(stream_);
            stream_ = nullptr;
        }
        if (queue_) {
            dispatch_release(queue_);
            queue_ = nullptr;
        }
        running_.store(false, std::memory_order_release);
        pending_cv_.notify_all();
        if (worker_.joinable()) {
            worker_.join();
        }
        std::lock_guard lk(pending_mtx_);
        pending_dirs_.clear();
    }

    bool is_active() const {
        return active_.load(std::memory_order_acquire);
    }

private:
    static void fs_callback(ConstFSEventStreamRef /*streamRef*/,
                            void *clientCallBackInfo,
                            size_t numEvents,
                            void *eventPaths,
                            const FSEventStreamEventFlags eventFlags[],
                            const FSEventStreamEventId eventIds[]) {
        auto *self = static_cast<FSEventsWatcher *>(clientCallBackInfo);
        if (!self || !self->running_.load(std::memory_order_acquire)) return;

        char **paths = static_cast<char **>(eventPaths);
        uint64_t max_id = 0;

        {
            std::lock_guard lk(self->pending_mtx_);
            for (size_t i = 0; i < numEvents; ++i) {
                if (eventIds[i] > max_id) max_id = eventIds[i];

                FSEventStreamEventFlags fl = eventFlags[i];
                if (fl & kFSEventStreamEventFlagHistoryDone) {
                    continue;
                }

                std::string p(paths[i]);
                while (p.size() > 1 && p.back() == '/') p.pop_back();
                if (should_skip_path(p)) continue;

                // Determine directory to rescan:
                // If it's a file event, rescan its parent directory.
                // If it's a directory event or MustScanSubDirs, rescan the directory (recursively if MustScanSubDirs).
                bool is_file_event = (fl & kFSEventStreamEventFlagItemIsFile) != 0;
                bool must_recurse = (fl & (kFSEventStreamEventFlagMustScanSubDirs |
                                           kFSEventStreamEventFlagUserDropped |
                                           kFSEventStreamEventFlagKernelDropped)) != 0;

                std::string target_dir = p;
                if (is_file_event && !must_recurse) {
                    size_t slash = p.rfind('/');
                    if (slash != std::string::npos) {
                        target_dir = (slash == 0) ? "/" : p.substr(0, slash);
                    }
                } else if (!is_file_event && (fl & (kFSEventStreamEventFlagItemRemoved | kFSEventStreamEventFlagItemRenamed))) {
                    // For removed/renamed directories, rescan parent too so deletion/rename is caught cleanly
                    size_t slash = p.rfind('/');
                    if (slash != std::string::npos) {
                        std::string parent_dir = (slash == 0) ? "/" : p.substr(0, slash);
                        if (!should_skip_path(parent_dir)) {
                            self->pending_dirs_[parent_dir] = self->pending_dirs_[parent_dir] || false;
                        }
                    }
                }

                if (!should_skip_path(target_dir)) {
                    self->pending_dirs_[target_dir] = self->pending_dirs_[target_dir] || must_recurse;
                }
            }
            if (max_id > self->latest_event_id_) {
                self->latest_event_id_ = max_id;
            }
        }
        self->pending_cv_.notify_one();
    }

    void worker_loop() {
        while (running_.load(std::memory_order_acquire)) {
            std::unordered_map<std::string, bool> batch;
            uint64_t ev_id = 0;
            {
                std::unique_lock lk(pending_mtx_);
                pending_cv_.wait(lk, [this]() {
                    return !pending_dirs_.empty() || !running_.load(std::memory_order_acquire);
                });
                if (!running_.load(std::memory_order_acquire)) break;
                batch.swap(pending_dirs_);
                ev_id = latest_event_id_;
            }

            bool any_changed = false;
            for (const auto &[dir, recurse] : batch) {
                if (!running_.load(std::memory_order_acquire)) break;
                if (ParallelScanner::sync_directory(*store_, dir, recurse)) {
                    any_changed = true;
                }
            }

            if (ev_id > 0) {
                std::unique_lock lock(store_->mutex);
                if (ev_id > store_->last_event_id) {
                    store_->last_event_id = ev_id;
                }
            }

            if (any_changed && on_change_) {
                on_change_(ev_id);
            }
        }
    }

    IndexStore *store_ = nullptr;
    ChangeCallback on_change_;
    FSEventStreamRef stream_ = nullptr;
    dispatch_queue_t queue_ = nullptr;
    std::thread worker_;
    std::atomic<bool> running_{false};
    std::atomic<bool> active_{false};

    std::mutex pending_mtx_;
    std::condition_variable pending_cv_;
    std::unordered_map<std::string, bool> pending_dirs_;
    uint64_t latest_event_id_ = 0;
};

} // namespace se
