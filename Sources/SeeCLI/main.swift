import Foundation
import EverythingCore

let args = CommandLine.arguments
var rootPath = "/"
var query = ""
var limit = 25
var forceRebuild = false
var dbPath = (NSHomeDirectory() as NSString).appendingPathComponent("Library/Application Support/SeeEverything/cli-bench.db")

var i = 1
while i < args.count {
    switch args[i] {
    case "--root":
        if i + 1 < args.count { rootPath = args[i + 1]; i += 1 }
    case "--limit", "-n":
        if i + 1 < args.count { limit = Int(args[i + 1]) ?? 25; i += 1 }
    case "--rebuild":
        forceRebuild = true
    case "--db":
        if i + 1 < args.count { dbPath = args[i + 1]; i += 1 }
    case "--no-db":
        dbPath = ""
    default:
        query = query.isEmpty ? args[i] : "\(query) \(args[i])"
    }
    i += 1
}

if !dbPath.isEmpty {
    let dir = (dbPath as NSString).deletingLastPathComponent
    try? FileManager.default.createDirectory(atPath: dir, withIntermediateDirectories: true)
}

guard let engine = se_create(dbPath.isEmpty ? nil : dbPath) else {
    fputs("Failed to create engine\n", stderr)
    exit(1)
}
defer { se_destroy(engine) }

let fda = se_has_full_disk_access()
print("Full Disk Access: \(fda ? "YES" : "NO")")
print("Indexing root: \(rootPath)\(forceRebuild ? " (forced rebuild)" : "")...")

rootPath.withCString { cRoot in
    var ptr: UnsafePointer<CChar>? = cRoot
    se_start(engine, &ptr, 1, forceRebuild)
}
se_wait_ready(engine)

let stats = se_get_stats(engine)
let memMB = Double(stats.memory_bytes) / (1024.0 * 1024.0)
print(String(format: "Indexed %llu objects (%llu files, %llu folders) in %llu ms (%@) — RAM: %.1f MB — FSEvents: %@",
             stats.total_entries, stats.total_files, stats.total_folders,
             stats.scan_duration_ms,
             stats.loaded_from_cache ? "loaded from cache" : "cold getattrlistbulk scan",
             memMB,
             stats.fsevents_active ? "active" : "inactive"))

var opts = SEQueryOptions(
    match_case: false,
    match_whole_word: false,
    match_path: false,
    match_regex: false,
    filter_preset: SE_FILTER_EVERYTHING,
    sort_column: SE_SORT_NAME,
    sort_ascending: true,
    folders_first: true
)

var gen: UInt64 = 0
let summary = query.withCString { qPtr in
    se_search(engine, qPtr, opts, &gen)
}

print(String(format: "Query \"%@\" -> %llu matches in %.2f ms (%.2f MB total)",
             query, summary.total_matches,
             Double(summary.query_time_us) / 1000.0,
             Double(summary.total_size) / (1024.0 * 1024.0)))

var buf = [CChar](repeating: 0, count: 4096)
let showCount = min(Int(summary.total_matches), limit)
for row in 0..<UInt64(showCount) {
    let n = se_result_get_full_path(engine, row, &buf, buf.count)
    let pathStr = String(cString: buf)
    var info = SEEntryInfo()
    se_result_get_info(engine, row, &info)
    let tag = info.is_dir != 0 ? "[DIR] " : "      "
    let sz = info.is_dir != 0 ? "" : " (\(info.size) B)"
    if n > 0 {
        print("  \(tag)\(pathStr)\(sz)")
    }
}
