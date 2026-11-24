#pragma once

#include <atomic>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <shared_mutex>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

namespace se {

static constexpr uint32_t INVALID_IDX = 0xFFFFFFFFu;
static constexpr uint16_t NO_EXT = 0xFFFFu;

enum EntryFlags : uint8_t {
    FLAG_NONE     = 0,
    FLAG_DIR      = 1 << 0,
    FLAG_HIDDEN   = 1 << 1,
    FLAG_SYMLINK  = 1 << 2,
    FLAG_DATALESS = 1 << 3,
    FLAG_DELETED  = 1 << 4,
};

struct Entry {
    uint32_t parent_idx   = INVALID_IDX;
    uint32_t first_child  = INVALID_IDX;
    uint32_t next_sibling = INVALID_IDX;
    uint32_t name_offset  = 0;
    uint16_t name_len     = 0;
    uint16_t ext_offset   = NO_EXT; // relative to name_offset (points after '.')
    uint8_t  flags        = 0;
    uint8_t  depth        = 0;
    uint16_t reserved     = 0;
    uint64_t size         = 0;
    int64_t  mtime        = 0;
    int64_t  ctime        = 0;

    inline bool is_dir() const { return (flags & FLAG_DIR) != 0; }
    inline bool is_hidden() const { return (flags & FLAG_HIDDEN) != 0; }
    inline bool is_symlink() const { return (flags & FLAG_SYMLINK) != 0; }
    inline bool is_dataless() const { return (flags & FLAG_DATALESS) != 0; }
    inline bool is_deleted() const { return (flags & FLAG_DELETED) != 0; }
};

static inline char ascii_lower(char c) {
    return (c >= 'A' && c <= 'Z') ? static_cast<char>(c + 32) : c;
}

static inline uint16_t find_ext_offset(std::string_view name, bool is_dir) {
    if (is_dir || name.size() < 2) return NO_EXT;
    size_t pos = name.rfind('.');
    if (pos == std::string_view::npos || pos == 0 || pos + 1 >= name.size()) {
        return NO_EXT;
    }
    if (pos + 1 > 0xFFFE) return NO_EXT;
    return static_cast<uint16_t>(pos + 1);
}

struct DBHeader {
    char magic[8];        // "SEEVDB01"
    uint32_t version;
    uint32_t reserved;
    uint64_t entry_count;
    uint64_t arena_bytes;
    uint64_t total_files;
    uint64_t total_folders;
    uint64_t last_event_id;
    uint64_t root_count;
};

class IndexStore {
public:
    mutable std::shared_mutex mutex;

    std::vector<Entry> entries;
    std::vector<char> name_arena;       // original UTF-8 bytes
    std::vector<char> lower_arena;      // ASCII-lowercased shadow bytes (same offsets!)
    std::vector<uint32_t> root_indices; // entry indices of indexed roots

    uint64_t total_files = 0;
    uint64_t total_folders = 0;
    uint64_t deleted_count = 0;
    uint64_t last_event_id = 0;

    void clear_unlocked() {
        entries.clear();
        name_arena.clear();
        lower_arena.clear();
        root_indices.clear();
        total_files = 0;
        total_folders = 0;
        deleted_count = 0;
    }

    void reserve_unlocked(size_t approx_entries) {
        entries.reserve(approx_entries);
        name_arena.reserve(approx_entries * 24);
        lower_arena.reserve(approx_entries * 24);
    }

    uint32_t add_entry_unlocked(uint32_t parent_idx,
                                std::string_view name,
                                uint64_t size,
                                int64_t mtime,
                                int64_t ctime,
                                uint8_t flags) {
        uint32_t idx = static_cast<uint32_t>(entries.size());
        size_t clamped_len = name.size() > 65535 ? 65535 : name.size();
        uint32_t offset = static_cast<uint32_t>(name_arena.size());

        name_arena.insert(name_arena.end(), name.data(), name.data() + clamped_len);
        lower_arena.resize(offset + clamped_len);
        for (size_t i = 0; i < clamped_len; ++i) {
            lower_arena[offset + i] = ascii_lower(name[i]);
        }

        Entry e;
        e.parent_idx = parent_idx;
        e.name_offset = offset;
        e.name_len = static_cast<uint16_t>(clamped_len);
        e.ext_offset = find_ext_offset(std::string_view(name.data(), clamped_len), (flags & FLAG_DIR) != 0);
        e.flags = flags;
        e.size = size;
        e.mtime = mtime;
        e.ctime = ctime;
        if (parent_idx != INVALID_IDX && parent_idx < entries.size()) {
            uint8_t pd = entries[parent_idx].depth;
            e.depth = pd < 255 ? static_cast<uint8_t>(pd + 1) : 255;
            e.next_sibling = entries[parent_idx].first_child;
            entries[parent_idx].first_child = idx;
        } else {
            e.depth = 0;
        }

        entries.push_back(e);
        if (flags & FLAG_DIR) {
            total_folders++;
        } else {
            total_files++;
        }
        return idx;
    }

    inline std::string_view name_of(const Entry &e) const {
        return std::string_view(name_arena.data() + e.name_offset, e.name_len);
    }

    inline std::string_view lower_name_of(const Entry &e) const {
        return std::string_view(lower_arena.data() + e.name_offset, e.name_len);
    }

    inline std::string_view ext_of(const Entry &e) const {
        if (e.ext_offset == NO_EXT || e.ext_offset >= e.name_len) return {};
        return std::string_view(name_arena.data() + e.name_offset + e.ext_offset,
                                e.name_len - e.ext_offset);
    }

    inline std::string_view lower_ext_of(const Entry &e) const {
        if (e.ext_offset == NO_EXT || e.ext_offset >= e.name_len) return {};
        return std::string_view(lower_arena.data() + e.name_offset + e.ext_offset,
                                e.name_len - e.ext_offset);
    }

    // Reconstructs parent path into out (e.g., "/Users/brg/Desktop" or "/").
    void build_parent_path(uint32_t idx, std::string &out) const {
        out.clear();
        if (idx >= entries.size()) return;
        uint32_t cur = entries[idx].parent_idx;
        if (cur == INVALID_IDX) {
            return;
        }
        // Collect ancestor indices up to root
        uint32_t chain[128];
        int count = 0;
        while (cur != INVALID_IDX && cur < entries.size() && count < 128) {
            chain[count++] = cur;
            cur = entries[cur].parent_idx;
        }
        for (int i = count - 1; i >= 0; --i) {
            std::string_view seg = name_of(entries[chain[i]]);
            if (seg == "/") {
                if (out.empty()) out.push_back('/');
            } else {
                if (out.empty() || out.back() != '/') out.push_back('/');
                out.append(seg.data(), seg.size());
            }
        }
        if (out.empty()) out.push_back('/');
    }

    void build_full_path(uint32_t idx, std::string &out) const {
        out.clear();
        if (idx >= entries.size()) return;
        uint32_t chain[128];
        int count = 0;
        uint32_t cur = idx;
        while (cur != INVALID_IDX && cur < entries.size() && count < 128) {
            chain[count++] = cur;
            cur = entries[cur].parent_idx;
        }
        for (int i = count - 1; i >= 0; --i) {
            std::string_view seg = name_of(entries[chain[i]]);
            if (seg == "/") {
                if (out.empty()) out.push_back('/');
            } else {
                if (out.empty() || out.back() != '/') out.push_back('/');
                out.append(seg.data(), seg.size());
            }
        }
        if (out.empty()) out.push_back('/');
    }

    // Fast lower-case full path builder for path queries
    void build_lower_full_path(uint32_t idx, std::string &out) const {
        out.clear();
        if (idx >= entries.size()) return;
        uint32_t chain[128];
        int count = 0;
        uint32_t cur = idx;
        while (cur != INVALID_IDX && cur < entries.size() && count < 128) {
            chain[count++] = cur;
            cur = entries[cur].parent_idx;
        }
        for (int i = count - 1; i >= 0; --i) {
            std::string_view seg = lower_name_of(entries[chain[i]]);
            if (seg == "/") {
                if (out.empty()) out.push_back('/');
            } else {
                if (out.empty() || out.back() != '/') out.push_back('/');
                out.append(seg.data(), seg.size());
            }
        }
        if (out.empty()) out.push_back('/');
    }

    // Resolve an absolute path (e.g. "/Users/brg/Desktop") to an entry index, or INVALID_IDX
    uint32_t resolve_path_unlocked(std::string_view path) const {
        if (path.empty() || root_indices.empty()) return INVALID_IDX;
        // Strip trailing slash unless path == "/"
        while (path.size() > 1 && path.back() == '/') {
            path.remove_suffix(1);
        }

        // Find matching root (prefer longest matching root)
        uint32_t best_root = INVALID_IDX;
        size_t best_len = 0;
        for (uint32_t ridx : root_indices) {
            if (ridx >= entries.size() || entries[ridx].is_deleted()) continue;
            std::string_view rname = name_of(entries[ridx]);
            if (rname == "/") {
                if (path[0] == '/' && 1 > best_len) {
                    best_root = ridx;
                    best_len = 1;
                }
            } else if (path.size() >= rname.size() && path.substr(0, rname.size()) == rname) {
                if (path.size() == rname.size() || path[rname.size()] == '/') {
                    if (rname.size() > best_len) {
                        best_root = ridx;
                        best_len = rname.size();
                    }
                }
            }
        }
        if (best_root == INVALID_IDX) return INVALID_IDX;
        if (path.size() == best_len) return best_root;

        std::string_view rem = path.substr(best_len);
        if (!rem.empty() && rem.front() == '/') rem.remove_prefix(1);

        uint32_t cur = best_root;
        while (!rem.empty()) {
            size_t slash = rem.find('/');
            std::string_view seg = (slash == std::string_view::npos) ? rem : rem.substr(0, slash);
            if (!seg.empty()) {
                uint32_t child = find_child_unlocked(cur, seg);
                if (child == INVALID_IDX) return INVALID_IDX;
                cur = child;
            }
            if (slash == std::string_view::npos) break;
            rem.remove_prefix(slash + 1);
        }
        return cur;
    }

    uint32_t find_child_unlocked(uint32_t parent_idx, std::string_view seg) const {
        if (parent_idx >= entries.size()) return INVALID_IDX;
        uint32_t cur = entries[parent_idx].first_child;
        while (cur != INVALID_IDX) {
            const Entry &e = entries[cur];
            if (!e.is_deleted() && name_of(e) == seg) {
                return cur;
            }
            cur = e.next_sibling;
        }
        return INVALID_IDX;
    }

    // Mark entry and all descendants as deleted, unlinking from parent's child list
    void delete_subtree_unlocked(uint32_t idx) {
        if (idx >= entries.size() || entries[idx].is_deleted()) return;

        // Unlink from parent's intrusive linked list
        uint32_t parent = entries[idx].parent_idx;
        if (parent != INVALID_IDX && parent < entries.size()) {
            uint32_t *link = &entries[parent].first_child;
            while (*link != INVALID_IDX) {
                if (*link == idx) {
                    *link = entries[idx].next_sibling;
                    break;
                }
                link = &entries[*link].next_sibling;
            }
        }

        // Iterative DFS to tombstone entire subtree
        std::vector<uint32_t> stack;
        stack.push_back(idx);
        while (!stack.empty()) {
            uint32_t cur = stack.back();
            stack.pop_back();
            Entry &e = entries[cur];
            if (e.is_deleted()) continue;
            e.flags |= FLAG_DELETED;
            deleted_count++;
            if (e.is_dir()) {
                if (total_folders > 0) total_folders--;
                uint32_t ch = e.first_child;
                while (ch != INVALID_IDX) {
                    stack.push_back(ch);
                    ch = entries[ch].next_sibling;
                }
                e.first_child = INVALID_IDX;
            } else {
                if (total_files > 0) total_files--;
            }
        }
    }

    uint64_t memory_bytes_unlocked() const {
        return entries.capacity() * sizeof(Entry)
             + name_arena.capacity()
             + lower_arena.capacity()
             + root_indices.capacity() * sizeof(uint32_t);
    }

    bool save_to_disk(const std::string &path) const {
        if (path.empty()) return false;
        std::shared_lock lock(mutex);
        std::string tmp_path = path + ".tmp";
        std::ofstream out(tmp_path, std::ios::binary | std::ios::trunc);
        if (!out) return false;

        DBHeader hdr{};
        std::memcpy(hdr.magic, "SEEVDB01", 8);
        hdr.version = 1;
        hdr.entry_count = entries.size();
        hdr.arena_bytes = name_arena.size();
        hdr.total_files = total_files;
        hdr.total_folders = total_folders;
        hdr.last_event_id = last_event_id;
        hdr.root_count = root_indices.size();

        out.write(reinterpret_cast<const char *>(&hdr), sizeof(hdr));
        if (!root_indices.empty()) {
            out.write(reinterpret_cast<const char *>(root_indices.data()),
                      root_indices.size() * sizeof(uint32_t));
        }
        if (!entries.empty()) {
            out.write(reinterpret_cast<const char *>(entries.data()),
                      entries.size() * sizeof(Entry));
        }
        if (!name_arena.empty()) {
            out.write(name_arena.data(), static_cast<std::streamsize>(name_arena.size()));
        }
        out.flush();
        bool ok = out.good();
        out.close();
        if (!ok) {
            std::remove(tmp_path.c_str());
            return false;
        }
        return std::rename(tmp_path.c_str(), path.c_str()) == 0;
    }

    bool load_from_disk(const std::string &path) {
        if (path.empty()) return false;
        std::ifstream in(path, std::ios::binary);
        if (!in) return false;

        DBHeader hdr{};
        in.read(reinterpret_cast<char *>(&hdr), sizeof(hdr));
        if (!in || std::memcmp(hdr.magic, "SEEVDB01", 8) != 0 || hdr.version != 1) {
            return false;
        }
        if (hdr.entry_count == 0 || hdr.entry_count > 50'000'000ULL || hdr.arena_bytes > 2'000'000'000ULL) {
            return false;
        }

        std::vector<uint32_t> tmp_roots(hdr.root_count);
        std::vector<Entry> tmp_entries(hdr.entry_count);
        std::vector<char> tmp_names(hdr.arena_bytes);

        if (hdr.root_count > 0) {
            in.read(reinterpret_cast<char *>(tmp_roots.data()), hdr.root_count * sizeof(uint32_t));
        }
        in.read(reinterpret_cast<char *>(tmp_entries.data()), hdr.entry_count * sizeof(Entry));
        if (hdr.arena_bytes > 0) {
            in.read(tmp_names.data(), static_cast<std::streamsize>(hdr.arena_bytes));
        }
        if (!in) return false;

        // Rebuild lowercase shadow arena quickly (single linear scan, ~20ms for 100MB)
        std::vector<char> tmp_lower(hdr.arena_bytes);
        for (size_t i = 0; i < hdr.arena_bytes; ++i) {
            tmp_lower[i] = ascii_lower(tmp_names[i]);
        }

        std::unique_lock lock(mutex);
        root_indices = std::move(tmp_roots);
        entries = std::move(tmp_entries);
        name_arena = std::move(tmp_names);
        lower_arena = std::move(tmp_lower);
        total_files = hdr.total_files;
        total_folders = hdr.total_folders;
        last_event_id = hdr.last_event_id;
        deleted_count = 0;
        for (const auto &e : entries) {
            if (e.is_deleted()) deleted_count++;
        }
        return true;
    }
};

} // namespace se
