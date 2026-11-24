#ifndef EVERYTHING_CORE_H
#define EVERYTHING_CORE_H

#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct SEEngine SEEngine;

typedef enum {
    SE_STATE_IDLE = 0,
    SE_STATE_LOADING_DB = 1,
    SE_STATE_INDEXING = 2,
    SE_STATE_UPDATING = 3,
    SE_STATE_READY = 4
} SEEngineState;

typedef enum {
    SE_SORT_NAME = 0,
    SE_SORT_PATH = 1,
    SE_SORT_SIZE = 2,
    SE_SORT_DATE_MODIFIED = 3,
    SE_SORT_DATE_CREATED = 4,
    SE_SORT_EXTENSION = 5
} SESortColumn;

typedef enum {
    SE_FILTER_EVERYTHING = 0,
    SE_FILTER_AUDIO = 1,
    SE_FILTER_COMPRESSED = 2,
    SE_FILTER_DOCUMENT = 3,
    SE_FILTER_EXECUTABLE = 4,
    SE_FILTER_FOLDER = 5,
    SE_FILTER_PICTURE = 6,
    SE_FILTER_VIDEO = 7,
    SE_FILTER_CODE = 8
} SEFilterPreset;

typedef struct {
    bool match_case;
    bool match_whole_word;
    bool match_path;
    bool match_regex;
    SEFilterPreset filter_preset;
    SESortColumn sort_column;
    bool sort_ascending;
    bool folders_first;
} SEQueryOptions;

typedef struct {
    SEEngineState state;
    uint64_t total_entries;
    uint64_t total_files;
    uint64_t total_folders;
    uint64_t scan_duration_ms;
    uint64_t last_event_id;
    uint64_t memory_bytes;
    bool fsevents_active;
    bool loaded_from_cache;
} SEEngineStats;

typedef struct {
    uint32_t entry_id;
    uint64_t size;
    int64_t mtime;
    int64_t ctime;
    uint8_t is_dir;
    uint8_t is_hidden;
    uint8_t is_symlink;
    uint8_t is_dataless;
} SEEntryInfo;

typedef struct {
    uint32_t start;
    uint32_t length;
} SEHighlightRange;

typedef struct {
    uint64_t total_matches;
    uint64_t total_size;
    uint64_t query_time_us;
    uint64_t generation;
} SEQuerySummary;

typedef void (*SEStatusCallback)(void *ctx, SEEngineStats stats);
typedef void (*SEIndexChangedCallback)(void *ctx);

// Lifecycle
SEEngine *se_create(const char *db_path);
void se_destroy(SEEngine *engine);

// Callbacks (invoked on background threads; caller dispatches to main queue)
void se_set_callbacks(SEEngine *engine,
                      void *ctx,
                      SEStatusCallback status_cb,
                      SEIndexChangedCallback index_changed_cb);

// Indexing & Persistence
// Starts loading persisted DB (if available) + FSEvents catch-up, or full parallel scan.
void se_start(SEEngine *engine, const char **roots, size_t root_count, bool force_rebuild);
void se_rebuild(SEEngine *engine);
void se_stop(SEEngine *engine);
bool se_save_db(SEEngine *engine);

// Synchronous wait helper (mainly for CLI/testing)
void se_wait_ready(SEEngine *engine);

// Status
SEEngineStats se_get_stats(SEEngine *engine);

// Search (thread-safe; cancels prior running search automatically via generation counter)
SEQuerySummary se_search(SEEngine *engine,
                         const char *query,
                         SEQueryOptions options,
                         uint64_t *out_generation);

// Fast Result Access (reads from the latest completed query result set)
uint64_t se_result_count(SEEngine *engine);
bool se_result_get_info(SEEngine *engine, uint64_t row, SEEntryInfo *out_info);

// Copies UTF-8 strings into caller buffer (null-terminated). Returns byte length written (excluding NUL).
size_t se_result_get_name(SEEngine *engine, uint64_t row, char *buf, size_t buf_size);
size_t se_result_get_parent_path(SEEngine *engine, uint64_t row, char *buf, size_t buf_size);
size_t se_result_get_full_path(SEEngine *engine, uint64_t row, char *buf, size_t buf_size);
size_t se_result_get_extension(SEEngine *engine, uint64_t row, char *buf, size_t buf_size);

// Computes match highlight ranges (UTF-8 byte offsets) in name or full path for current query
size_t se_result_get_highlights(SEEngine *engine,
                                uint64_t row,
                                bool in_path,
                                SEHighlightRange *out_ranges,
                                size_t max_ranges);

// Helper: check if Full Disk Access appears granted
bool se_has_full_disk_access(void);

#ifdef __cplusplus
}
#endif

#endif // EVERYTHING_CORE_H
