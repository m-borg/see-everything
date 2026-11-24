#pragma once

#include "IndexStore.hpp"

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <deque>
#include <fcntl.h>
#include <functional>
#include <mutex>
#include <string>
#include <string_view>
#include <sys/attr.h>
#include <sys/stat.h>
#include <sys/vnode.h>
#include <thread>
#include <unistd.h>
#include <unordered_map>
#include <vector>

namespace se {

struct ScannedChild {
    std::string name;
    uint64_t size = 0;
    int64_t mtime = 0;
    int64_t ctime = 0;
    uint8_t flags = 0;
};

// APFS & macOS virtual/duplicate path exclusions so we never double-index firmlinks
// or hang on virtual filesystems (/dev, Time Machine snapshots, SSV data mount).
inline bool should_skip_path(std::string_view full_path) {
    if (full_path == "/dev" ||
        full_path == "/.vol" ||
        full_path == "/net" ||
        full_path == "/home" ||
        full_path == "/System/Volumes/Data" ||
        full_path == "/System/Volumes/VM" ||
        full_path == "/System/Volumes/Preboot" ||
        full_path == "/System/Volumes/Update" ||
        full_path == "/System/Volumes/Hardware" ||
        full_path == "/System/Volumes/xarts" ||
        full_path == "/System/Volumes/iSCPreboot" ||
        full_path == "/System/Volumes/Recovery" ||
        full_path == "/System/Volumes/FieldService" ||
        full_path == "/System/Volumes/FieldServiceDiagnostic" ||
        full_path == "/System/Volumes/FieldServiceRepair" ||
        full_path == "/System/Volumes/BaseSystem" ||
        full_path == "/.Spotlight-V100" ||
        full_path == "/.fseventsd" ||
        full_path == "/.MobileBackups" ||
        full_path == "/private/var/vm") {
        return true;
    }
    if (full_path.rfind("/Volumes/com.apple.TimeMachine.", 0) == 0) {
        return true;
    }
    if (full_path.rfind("/System/Volumes/Update/", 0) == 0) {
        return true;
    }
    return false;
}

// Fast single-directory listing via macOS getattrlistbulk().
// Does NOT open files and does NOT materialize iCloud/FileProvider dataless files.
inline bool scan_directory_bulk(const char *dir_path, std::vector<ScannedChild> &out_children) {
    out_children.clear();
    int fd = ::open(dir_path, O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC);
    if (fd < 0) return false;

    struct attrlist alist{};
    alist.bitmapcount = ATTR_BIT_MAP_COUNT;
    alist.commonattr = ATTR_CMN_RETURNED_ATTRS
                     | ATTR_CMN_NAME
                     | ATTR_CMN_OBJTYPE
                     | ATTR_CMN_CRTIME
                     | ATTR_CMN_MODTIME
                     | ATTR_CMN_FLAGS;
    alist.fileattr = ATTR_FILE_DATALENGTH;

    constexpr size_t BUF_SIZE = 64 * 1024;
    alignas(8) char buf[BUF_SIZE];

    while (true) {
        int count = ::getattrlistbulk(fd, &alist, buf, BUF_SIZE, 0);
        if (count <= 0) break;

        char *ptr = buf;
        for (int i = 0; i < count; ++i) {
            uint32_t entry_len = 0;
            std::memcpy(&entry_len, ptr, sizeof(uint32_t));
            if (entry_len == 0) break;

            char *field = ptr + sizeof(uint32_t);
            char *entry_end = ptr + entry_len;

            attribute_set_t ret{};
            std::memcpy(&ret, field, sizeof(attribute_set_t));
            field += sizeof(attribute_set_t);

            std::string_view name_sv;
            if (ret.commonattr & ATTR_CMN_NAME) {
                attrreference_t ref{};
                std::memcpy(&ref, field, sizeof(attrreference_t));
                const char *name_ptr = field + ref.attr_dataoffset;
                size_t name_len = ref.attr_length;
                if (name_len > 0 && name_ptr + name_len <= entry_end) {
                    // getattrlistbulk includes trailing NUL in attr_length
                    while (name_len > 0 && name_ptr[name_len - 1] == '\0') {
                        --name_len;
                    }
                    name_sv = std::string_view(name_ptr, name_len);
                }
                field += sizeof(attrreference_t);
            }

            fsobj_type_t obj_type = VNON;
            if (ret.commonattr & ATTR_CMN_OBJTYPE) {
                std::memcpy(&obj_type, field, sizeof(fsobj_type_t));
                field += sizeof(fsobj_type_t);
            }

            int64_t ctime_sec = 0;
            if (ret.commonattr & ATTR_CMN_CRTIME) {
                struct timespec ts{};
                std::memcpy(&ts, field, sizeof(ts));
                ctime_sec = static_cast<int64_t>(ts.tv_sec);
                field += sizeof(ts);
            }

            int64_t mtime_sec = 0;
            if (ret.commonattr & ATTR_CMN_MODTIME) {
                struct timespec ts{};
                std::memcpy(&ts, field, sizeof(ts));
                mtime_sec = static_cast<int64_t>(ts.tv_sec);
                field += sizeof(ts);
            }

            uint32_t bsd_flags = 0;
            if (ret.commonattr & ATTR_CMN_FLAGS) {
                std::memcpy(&bsd_flags, field, sizeof(uint32_t));
                field += sizeof(uint32_t);
            }

            uint64_t file_size = 0;
            if (ret.fileattr & ATTR_FILE_DATALENGTH) {
                off_t sz = 0;
                std::memcpy(&sz, field, sizeof(off_t));
                if (sz > 0) file_size = static_cast<uint64_t>(sz);
                field += sizeof(off_t);
            }

            if (!name_sv.empty() && name_sv != "." && name_sv != "..") {
                ScannedChild sc;
                sc.name.assign(name_sv.data(), name_sv.size());
                sc.size = file_size;
                sc.mtime = mtime_sec;
                sc.ctime = ctime_sec;
                uint8_t fl = FLAG_NONE;
                if (obj_type == VDIR) fl |= FLAG_DIR;
                if (obj_type == VLNK) fl |= FLAG_SYMLINK;
                if ((bsd_flags & UF_HIDDEN) || (!name_sv.empty() && name_sv[0] == '.')) {
                    fl |= FLAG_HIDDEN;
                }
                if (bsd_flags & SF_DATALESS) fl |= FLAG_DATALESS;
                sc.flags = fl;
                out_children.push_back(std::move(sc));
            }

            ptr += entry_len;
        }
    }

    ::close(fd);
    return true;
}

class ParallelScanner {
public:
    struct Progress {
        uint64_t files = 0;
        uint64_t folders = 0;
    };

    // Scans all given roots in parallel using getattrlistbulk() and populates `out_store`.
    // Returns elapsed milliseconds.
    static uint64_t scan_into(IndexStore &out_store,
                              const std::vector<std::string> &roots,
                              std::atomic<bool> &cancel_flag,
                              const std::function<void(uint64_t files, uint64_t folders)> &progress_cb) {
        auto t0 = std::chrono::steady_clock::now();

        struct WorkItem {
            uint32_t dir_idx;
            std::string path;
        };

        // Initialize fresh index
        {
            std::unique_lock lock(out_store.mutex);
            out_store.clear_unlocked();
            out_store.reserve_unlocked(500'000);
        }

        std::mutex q_mtx;
        std::condition_variable q_cv;
        std::deque<WorkItem> queue;
        std::atomic<size_t> active_workers{0};
        std::atomic<bool> done{false};

        // Add root entries
        {
            std::unique_lock lock(out_store.mutex);
            for (const auto &raw_root : roots) {
                std::string r = raw_root;
                while (r.size() > 1 && r.back() == '/') r.pop_back();
                struct stat st{};
                if (::lstat(r.c_str(), &st) != 0 || !S_ISDIR(st.st_mode)) continue;
                uint32_t ridx = out_store.add_entry_unlocked(
                    INVALID_IDX, r, 0,
                    static_cast<int64_t>(st.st_mtimespec.tv_sec),
                    static_cast<int64_t>(st.st_birthtimespec.tv_sec),
                    FLAG_DIR
                );
                out_store.root_indices.push_back(ridx);
                queue.push_back(WorkItem{ridx, r});
            }
        }

        unsigned num_threads = std::thread::hardware_concurrency();
        if (num_threads < 4) num_threads = 4;
        if (num_threads > 12) num_threads = 12;

        std::mutex merge_mtx;
        std::atomic<uint64_t> total_files_atomic{0};
        std::atomic<uint64_t> total_folders_atomic{out_store.root_indices.size()};
        std::atomic<uint64_t> last_report_ms{0};

        auto worker_fn = [&]() {
            std::vector<ScannedChild> children;
            children.reserve(256);
            std::vector<WorkItem> next_dirs;
            next_dirs.reserve(64);
            std::string child_path;
            child_path.reserve(512);

            while (!cancel_flag.load(std::memory_order_relaxed)) {
                WorkItem item;
                {
                    std::unique_lock lk(q_mtx);
                    q_cv.wait(lk, [&]() {
                        return !queue.empty() || done.load(std::memory_order_acquire) || cancel_flag.load(std::memory_order_relaxed);
                    });
                    if (cancel_flag.load(std::memory_order_relaxed)) break;
                    if (queue.empty()) {
                        if (done.load(std::memory_order_acquire)) break;
                        continue;
                    }
                    // Pop from front (BFS keeps queue wide for parallelism)
                    item = std::move(queue.front());
                    queue.pop_front();
                    active_workers.fetch_add(1, std::memory_order_acq_rel);
                }

                // Scan directory with zero locks held!
                bool ok = scan_directory_bulk(item.path.c_str(), children);
                next_dirs.clear();

                if (ok && !children.empty()) {
                    uint64_t local_files = 0;
                    uint64_t local_folders = 0;

                    {
                        std::lock_guard mlock(merge_mtx);
                        std::unique_lock slock(out_store.mutex);
                        for (const auto &ch : children) {
                            // Build child full path for exclusion / recursion check
                            child_path.clear();
                            if (item.path == "/") {
                                child_path.push_back('/');
                                child_path.append(ch.name);
                            } else {
                                child_path.append(item.path);
                                child_path.push_back('/');
                                child_path.append(ch.name);
                            }

                            if ((ch.flags & FLAG_DIR) && should_skip_path(child_path)) {
                                continue;
                            }

                            uint32_t cidx = out_store.add_entry_unlocked(
                                item.dir_idx, ch.name, ch.size, ch.mtime, ch.ctime, ch.flags
                            );
                            if (ch.flags & FLAG_DIR) {
                                local_folders++;
                                // Never follow symlinks when recursing
                                if (!(ch.flags & FLAG_SYMLINK)) {
                                    next_dirs.push_back(WorkItem{cidx, child_path});
                                }
                            } else {
                                local_files++;
                            }
                        }
                    }

                    uint64_t f_now = total_files_atomic.fetch_add(local_files, std::memory_order_relaxed) + local_files;
                    uint64_t d_now = total_folders_atomic.fetch_add(local_folders, std::memory_order_relaxed) + local_folders;

                    if (progress_cb) {
                        auto now_ms = static_cast<uint64_t>(
                            std::chrono::duration_cast<std::chrono::milliseconds>(
                                std::chrono::steady_clock::now() - t0
                            ).count()
                        );
                        uint64_t prev = last_report_ms.load(std::memory_order_relaxed);
                        if (now_ms >= prev + 120 && last_report_ms.compare_exchange_strong(prev, now_ms)) {
                            progress_cb(f_now, d_now);
                        }
                    }
                }

                {
                    std::lock_guard lk(q_mtx);
                    for (auto &nd : next_dirs) {
                        queue.push_back(std::move(nd));
                    }
                    size_t remaining_active = active_workers.fetch_sub(1, std::memory_order_acq_rel) - 1;
                    if (queue.empty() && remaining_active == 0) {
                        done.store(true, std::memory_order_release);
                        q_cv.notify_all();
                    } else if (!next_dirs.empty()) {
                        q_cv.notify_all();
                    }
                }
            }
        };

        std::vector<std::thread> workers;
        workers.reserve(num_threads);
        for (unsigned i = 0; i < num_threads; ++i) {
            workers.emplace_back(worker_fn);
        }
        for (auto &t : workers) {
            t.join();
        }

        if (!cancel_flag.load(std::memory_order_relaxed)) {
            std::unique_lock lock(out_store.mutex);
            out_store.entries.shrink_to_fit();
            out_store.name_arena.shrink_to_fit();
            out_store.lower_arena.shrink_to_fit();
        }

        auto t1 = std::chrono::steady_clock::now();
        return static_cast<uint64_t>(
            std::chrono::duration_cast<std::chrono::milliseconds>(t1 - t0).count()
        );
    }

    // Re-syncs a single directory (and optionally recursively adds new subdirectories)
    // directly into `store`. Called by FSEvents live updates.
    // Returns true if anything changed in the index.
    static bool sync_directory(IndexStore &store, const std::string &dir_path, bool recursive) {
        if (should_skip_path(dir_path)) return false;

        std::vector<ScannedChild> disk_children;
        bool dir_exists = scan_directory_bulk(dir_path.c_str(), disk_children);

        std::vector<std::string> new_subdirs_to_scan;
        bool changed = false;

        {
            std::unique_lock lock(store.mutex);
            uint32_t dir_idx = store.resolve_path_unlocked(dir_path);

            if (!dir_exists) {
                // Directory was deleted on disk -> tombstone subtree
                if (dir_idx != INVALID_IDX) {
                    store.delete_subtree_unlocked(dir_idx);
                    return true;
                }
                return false;
            }

            // If directory itself doesn't exist in index yet, try to ensure parent chain exists
            if (dir_idx == INVALID_IDX) {
                dir_idx = ensure_dir_chain_unlocked(store, dir_path);
                if (dir_idx == INVALID_IDX) return false;
                changed = true;
            }

            // Build map of existing direct children: name -> entry_id
            std::unordered_map<std::string_view, uint32_t> existing;
            uint32_t cur = store.entries[dir_idx].first_child;
            while (cur != INVALID_IDX) {
                const Entry &e = store.entries[cur];
                if (!e.is_deleted()) {
                    existing.emplace(store.name_of(e), cur);
                }
                cur = e.next_sibling;
            }

            // Diff disk_children against existing
            for (const auto &sc : disk_children) {
                std::string child_full = (dir_path == "/") ? ("/" + sc.name) : (dir_path + "/" + sc.name);
                if ((sc.flags & FLAG_DIR) && should_skip_path(child_full)) continue;

                auto it = existing.find(sc.name);
                if (it != existing.end()) {
                    uint32_t eidx = it->second;
                    Entry &e = store.entries[eidx];
                    bool type_changed = (e.is_dir() != ((sc.flags & FLAG_DIR) != 0));
                    if (type_changed) {
                        store.delete_subtree_unlocked(eidx);
                        uint32_t nidx = store.add_entry_unlocked(dir_idx, sc.name, sc.size, sc.mtime, sc.ctime, sc.flags);
                        (void)nidx;
                        changed = true;
                        if ((sc.flags & FLAG_DIR) && !(sc.flags & FLAG_SYMLINK)) {
                            new_subdirs_to_scan.push_back(std::move(child_full));
                        }
                    } else {
                        if (e.size != sc.size || e.mtime != sc.mtime || e.flags != sc.flags) {
                            e.size = sc.size;
                            e.mtime = sc.mtime;
                            e.ctime = sc.ctime;
                            e.flags = sc.flags;
                            changed = true;
                        }
                        if (recursive && (sc.flags & FLAG_DIR) && !(sc.flags & FLAG_SYMLINK)) {
                            new_subdirs_to_scan.push_back(std::move(child_full));
                        }
                    }
                    existing.erase(it);
                } else {
                    store.add_entry_unlocked(dir_idx, sc.name, sc.size, sc.mtime, sc.ctime, sc.flags);
                    changed = true;
                    if ((sc.flags & FLAG_DIR) && !(sc.flags & FLAG_SYMLINK)) {
                        new_subdirs_to_scan.push_back(std::move(child_full));
                    }
                }
            }

            // Anything left in `existing` was deleted on disk
            for (const auto &kv : existing) {
                store.delete_subtree_unlocked(kv.second);
                changed = true;
            }
        }

        // Recursively scan newly created directories (or all subdirs if MustScanSubDirs)
        for (const auto &sub : new_subdirs_to_scan) {
            if (sync_directory(store, sub, true)) {
                changed = true;
            }
        }
        return changed;
    }

private:
    static uint32_t ensure_dir_chain_unlocked(IndexStore &store, std::string_view path) {
        if (path.empty() || path == "/") {
            return store.resolve_path_unlocked("/");
        }
        while (path.size() > 1 && path.back() == '/') path.remove_suffix(1);
        size_t slash = path.rfind('/');
        if (slash == std::string_view::npos) return INVALID_IDX;

        std::string_view parent_sv = (slash == 0) ? "/" : path.substr(0, slash);
        std::string_view leaf_sv = path.substr(slash + 1);

        uint32_t parent_idx = store.resolve_path_unlocked(parent_sv);
        if (parent_idx == INVALID_IDX) {
            parent_idx = ensure_dir_chain_unlocked(store, parent_sv);
            if (parent_idx == INVALID_IDX) return INVALID_IDX;
        }
        uint32_t existing = store.find_child_unlocked(parent_idx, leaf_sv);
        if (existing != INVALID_IDX) return existing;

        struct stat st{};
        std::string path_str(path);
        if (::lstat(path_str.c_str(), &st) != 0 || !S_ISDIR(st.st_mode)) return INVALID_IDX;
        uint8_t fl = FLAG_DIR;
        if ((st.st_flags & UF_HIDDEN) || (!leaf_sv.empty() && leaf_sv[0] == '.')) fl |= FLAG_HIDDEN;
        return store.add_entry_unlocked(
            parent_idx, leaf_sv, 0,
            static_cast<int64_t>(st.st_mtimespec.tv_sec),
            static_cast<int64_t>(st.st_birthtimespec.tv_sec),
            fl
        );
    }
};

} // namespace se
