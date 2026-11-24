#include "include/EverythingCore.h"
#include "IndexStore.hpp"
#include "Scanner.hpp"
#include "FSEventsWatcher.hpp"
#include "QueryEngine.hpp"

#include <atomic>
#include <condition_variable>
#include <cstdlib>
#include <cstring>
#include <dirent.h>
#include <mutex>
#include <pwd.h>
#include <string>
#include <thread>
#include <unistd.h>
#include <vector>

struct SEEngine {
    std::string db_path;
    se::IndexStore store;
    se::FSEventsWatcher watcher;

    std::vector<std::string> roots;
    std::atomic<SEEngineState> state{SE_STATE_IDLE};
    std::atomic<uint64_t> scan_duration_ms{0};
    std::atomic<bool> loaded_from_cache{false};
    std::atomic<bool> cancel_scan{false};

    // Callbacks
    std::mutex cb_mtx;
    void *cb_ctx = nullptr;
    SEStatusCallback status_cb = nullptr;
    SEIndexChangedCallback index_changed_cb = nullptr;

    // Background indexing thread
    std::thread index_thread;
    std::mutex ready_mtx;
    std::condition_variable ready_cv;

    // Latest query results (protected by result_mtx for fast UI table reading)
    std::atomic<uint64_t> query_gen{0};
    mutable std::shared_mutex result_mtx;
    std::vector<uint32_t> result_ids;
    std::vector<std::string> highlight_terms;
    bool highlight_match_case = false;

    void emit_status() {
        SEStatusCallback cb = nullptr;
        void *ctx = nullptr;
        {
            std::lock_guard lk(cb_mtx);
            cb = status_cb;
            ctx = cb_ctx;
        }
        if (cb) {
            cb(ctx, get_stats());
        }
    }

    void emit_index_changed() {
        SEIndexChangedCallback cb = nullptr;
        void *ctx = nullptr;
        {
            std::lock_guard lk(cb_mtx);
            cb = index_changed_cb;
            ctx = cb_ctx;
        }
        if (cb) {
            cb(ctx);
        }
    }

    SEEngineStats get_stats() const {
        SEEngineStats s{};
        s.state = state.load(std::memory_order_acquire);
        s.scan_duration_ms = scan_duration_ms.load(std::memory_order_relaxed);
        s.fsevents_active = watcher.is_active();
        s.loaded_from_cache = loaded_from_cache.load(std::memory_order_relaxed);
        {
            std::shared_lock lock(store.mutex);
            s.total_files = store.total_files;
            s.total_folders = store.total_folders;
            s.total_entries = store.total_files + store.total_folders;
            s.last_event_id = store.last_event_id;
            s.memory_bytes = store.memory_bytes_unlocked();
        }
        return s;
    }

    void stop_indexing() {
        cancel_scan.store(true, std::memory_order_release);
        watcher.stop();
        if (index_thread.joinable()) {
            index_thread.join();
        }
        cancel_scan.store(false, std::memory_order_release);
    }

    void start_indexing(bool force_rebuild) {
        stop_indexing();

        {
            std::lock_guard lk(ready_mtx);
            state.store(force_rebuild ? SE_STATE_INDEXING : SE_STATE_LOADING_DB, std::memory_order_release);
        }
        emit_status();

        index_thread = std::thread([this, force_rebuild]() {
            bool loaded = false;
            if (!force_rebuild && !db_path.empty()) {
                auto t0 = std::chrono::steady_clock::now();
                loaded = store.load_from_disk(db_path);
                if (loaded) {
                    auto ms = static_cast<uint64_t>(
                        std::chrono::duration_cast<std::chrono::milliseconds>(
                            std::chrono::steady_clock::now() - t0
                        ).count()
                    );
                    scan_duration_ms.store(ms, std::memory_order_relaxed);
                    loaded_from_cache.store(true, std::memory_order_relaxed);
                }
            }

            if (!loaded && !cancel_scan.load(std::memory_order_acquire)) {
                loaded_from_cache.store(false, std::memory_order_relaxed);
                state.store(SE_STATE_INDEXING, std::memory_order_release);
                emit_status();

                uint64_t pre_scan_event_id = se::FSEventsWatcher::current_system_event_id();
                uint64_t ms = se::ParallelScanner::scan_into(
                    store,
                    roots,
                    cancel_scan,
                    [this](uint64_t /*f*/, uint64_t /*d*/) {
                        emit_status();
                    }
                );

                if (cancel_scan.load(std::memory_order_acquire)) {
                    {
                        std::lock_guard lk(ready_mtx);
                        state.store(SE_STATE_IDLE, std::memory_order_release);
                    }
                    ready_cv.notify_all();
                    return;
                }

                {
                    std::unique_lock lock(store.mutex);
                    store.last_event_id = pre_scan_event_id;
                }
                scan_duration_ms.store(ms, std::memory_order_relaxed);

                // Persist snapshot to disk right after scan completes
                if (!db_path.empty()) {
                    store.save_to_disk(db_path);
                }
            }

            // Start live FSEvents watcher catching up from saved event ID!
            uint64_t since_id = 0;
            {
                std::shared_lock lock(store.mutex);
                since_id = store.last_event_id;
            }

            watcher.start(&store, roots, since_id, [this](uint64_t /*ev_id*/) {
                emit_status();
                emit_index_changed();
            });

            {
                std::lock_guard lk(ready_mtx);
                state.store(SE_STATE_READY, std::memory_order_release);
            }
            emit_status();
            emit_index_changed();
            ready_cv.notify_all();
        });
    }
};

extern "C" {

SEEngine *se_create(const char *db_path) {
    auto *eng = new SEEngine();
    if (db_path) {
        eng->db_path = db_path;
    }
    return eng;
}

void se_destroy(SEEngine *engine) {
    if (!engine) return;
    engine->stop_indexing();
    if (!engine->db_path.empty() && engine->store.entries.size() > 0) {
        engine->store.save_to_disk(engine->db_path);
    }
    delete engine;
}

void se_set_callbacks(SEEngine *engine,
                      void *ctx,
                      SEStatusCallback status_cb,
                      SEIndexChangedCallback index_changed_cb) {
    if (!engine) return;
    std::lock_guard lk(engine->cb_mtx);
    engine->cb_ctx = ctx;
    engine->status_cb = status_cb;
    engine->index_changed_cb = index_changed_cb;
}

void se_start(SEEngine *engine, const char **roots, size_t root_count, bool force_rebuild) {
    if (!engine) return;
    engine->roots.clear();
    if (roots && root_count > 0) {
        for (size_t i = 0; i < root_count; ++i) {
            if (roots[i] && roots[i][0] != '\0') {
                engine->roots.emplace_back(roots[i]);
            }
        }
    }
    if (engine->roots.empty()) {
        engine->roots.emplace_back("/");
    }
    engine->start_indexing(force_rebuild);
}

void se_rebuild(SEEngine *engine) {
    if (!engine) return;
    engine->start_indexing(true);
}

void se_stop(SEEngine *engine) {
    if (!engine) return;
    engine->stop_indexing();
}

bool se_save_db(SEEngine *engine) {
    if (!engine || engine->db_path.empty()) return false;
    return engine->store.save_to_disk(engine->db_path);
}

void se_wait_ready(SEEngine *engine) {
    if (!engine) return;
    std::unique_lock lk(engine->ready_mtx);
    engine->ready_cv.wait(lk, [&]() {
        auto st = engine->state.load(std::memory_order_acquire);
        return st == SE_STATE_READY || st == SE_STATE_IDLE;
    });
}

SEEngineStats se_get_stats(SEEngine *engine) {
    if (!engine) return SEEngineStats{};
    return engine->get_stats();
}

SEQuerySummary se_search(SEEngine *engine,
                         const char *query,
                         SEQueryOptions options,
                         uint64_t *out_generation) {
    SEQuerySummary sum{};
    if (!engine) return sum;

    uint64_t gen = engine->query_gen.fetch_add(1, std::memory_order_acq_rel) + 1;
    if (out_generation) *out_generation = gen;

    std::string_view qsv = query ? std::string_view(query) : std::string_view();
    auto res = se::QueryEngine::execute(engine->store, qsv, options, engine->query_gen, gen);

    if (res.cancelled) {
        sum.generation = 0;
        return sum;
    }

    {
        std::unique_lock rlock(engine->result_mtx);
        if (engine->query_gen.load(std::memory_order_acquire) == gen) {
            engine->result_ids = std::move(res.matches);
            engine->highlight_terms = std::move(res.highlight_terms);
            engine->highlight_match_case = options.match_case;
            sum.total_matches = engine->result_ids.size();
            sum.total_size = res.total_size;
            sum.query_time_us = res.elapsed_us;
            sum.generation = gen;
        }
    }
    return sum;
}

uint64_t se_result_count(SEEngine *engine) {
    if (!engine) return 0;
    std::shared_lock rlock(engine->result_mtx);
    return engine->result_ids.size();
}

bool se_result_get_info(SEEngine *engine, uint64_t row, SEEntryInfo *out_info) {
    if (!engine || !out_info) return false;
    std::shared_lock rlock(engine->result_mtx);
    if (row >= engine->result_ids.size()) return false;
    uint32_t eid = engine->result_ids[row];

    std::shared_lock slock(engine->store.mutex);
    if (eid >= engine->store.entries.size()) return false;
    const se::Entry &e = engine->store.entries[eid];
    out_info->entry_id = eid;
    out_info->size = e.size;
    out_info->mtime = e.mtime;
    out_info->ctime = e.ctime;
    out_info->is_dir = e.is_dir() ? 1 : 0;
    out_info->is_hidden = e.is_hidden() ? 1 : 0;
    out_info->is_symlink = e.is_symlink() ? 1 : 0;
    out_info->is_dataless = e.is_dataless() ? 1 : 0;
    return true;
}

static size_t copy_sv(std::string_view sv, char *buf, size_t buf_size) {
    if (!buf || buf_size == 0) return 0;
    size_t n = sv.size() < (buf_size - 1) ? sv.size() : (buf_size - 1);
    if (n > 0) std::memcpy(buf, sv.data(), n);
    buf[n] = '\0';
    return n;
}

size_t se_result_get_name(SEEngine *engine, uint64_t row, char *buf, size_t buf_size) {
    if (!engine) return 0;
    std::shared_lock rlock(engine->result_mtx);
    if (row >= engine->result_ids.size()) return 0;
    uint32_t eid = engine->result_ids[row];
    std::shared_lock slock(engine->store.mutex);
    if (eid >= engine->store.entries.size()) return 0;
    return copy_sv(engine->store.name_of(engine->store.entries[eid]), buf, buf_size);
}

size_t se_result_get_parent_path(SEEngine *engine, uint64_t row, char *buf, size_t buf_size) {
    if (!engine) return 0;
    std::shared_lock rlock(engine->result_mtx);
    if (row >= engine->result_ids.size()) return 0;
    uint32_t eid = engine->result_ids[row];
    std::shared_lock slock(engine->store.mutex);
    if (eid >= engine->store.entries.size()) return 0;
    std::string p;
    engine->store.build_parent_path(eid, p);
    return copy_sv(p, buf, buf_size);
}

size_t se_result_get_full_path(SEEngine *engine, uint64_t row, char *buf, size_t buf_size) {
    if (!engine) return 0;
    std::shared_lock rlock(engine->result_mtx);
    if (row >= engine->result_ids.size()) return 0;
    uint32_t eid = engine->result_ids[row];
    std::shared_lock slock(engine->store.mutex);
    if (eid >= engine->store.entries.size()) return 0;
    std::string p;
    engine->store.build_full_path(eid, p);
    return copy_sv(p, buf, buf_size);
}

size_t se_result_get_extension(SEEngine *engine, uint64_t row, char *buf, size_t buf_size) {
    if (!engine) return 0;
    std::shared_lock rlock(engine->result_mtx);
    if (row >= engine->result_ids.size()) return 0;
    uint32_t eid = engine->result_ids[row];
    std::shared_lock slock(engine->store.mutex);
    if (eid >= engine->store.entries.size()) return 0;
    return copy_sv(engine->store.ext_of(engine->store.entries[eid]), buf, buf_size);
}

size_t se_result_get_highlights(SEEngine *engine,
                                uint64_t row,
                                bool in_path,
                                SEHighlightRange *out_ranges,
                                size_t max_ranges) {
    if (!engine || !out_ranges || max_ranges == 0) return 0;
    std::shared_lock rlock(engine->result_mtx);
    if (row >= engine->result_ids.size() || engine->highlight_terms.empty()) return 0;
    uint32_t eid = engine->result_ids[row];
    const auto &terms = engine->highlight_terms;
    bool match_case = engine->highlight_match_case;

    std::shared_lock slock(engine->store.mutex);
    if (eid >= engine->store.entries.size()) return 0;

    std::string text;
    if (in_path) {
        engine->store.build_parent_path(eid, text);
        if (!match_case) text = se::to_lower_ascii(text);
    } else {
        std::string_view sv = match_case
            ? engine->store.name_of(engine->store.entries[eid])
            : engine->store.lower_name_of(engine->store.entries[eid]);
        text.assign(sv.data(), sv.size());
    }

    size_t count = 0;
    for (const auto &raw_term : terms) {
        if (count >= max_ranges) break;
        std::string needle = match_case ? raw_term : se::to_lower_ascii(raw_term);
        if (needle.empty()) continue;
        size_t pos = 0;
        while (count < max_ranges && (pos = text.find(needle, pos)) != std::string::npos) {
            out_ranges[count].start = static_cast<uint32_t>(pos);
            out_ranges[count].length = static_cast<uint32_t>(needle.size());
            count++;
            pos += needle.size();
        }
    }
    return count;
}

bool se_has_full_disk_access(void) {
    // Check readability of TCC-protected locations that require Full Disk Access
    const char *home = std::getenv("HOME");
    if (!home) {
        struct passwd *pw = getpwuid(getuid());
        if (pw) home = pw->pw_dir;
    }
    if (!home) return false;

    const char *probes[] = {
        "/Library/Application Support/com.apple.TCC/TCC.db",
    };
    for (const char *p : probes) {
        int fd = ::open(p, O_RDONLY | O_CLOEXEC);
        if (fd >= 0) {
            ::close(fd);
            return true;
        }
    }

    std::string safari_dir = std::string(home) + "/Library/Safari";
    DIR *d = ::opendir(safari_dir.c_str());
    if (d) {
        ::closedir(d);
        return true;
    }
    std::string mail_dir = std::string(home) + "/Library/Mail";
    d = ::opendir(mail_dir.c_str());
    if (d) {
        ::closedir(d);
        return true;
    }
    return false;
}

} // extern "C"
