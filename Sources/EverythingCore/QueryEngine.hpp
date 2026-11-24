#pragma once

#include "IndexStore.hpp"
#include "include/EverythingCore.h"

#include <algorithm>
#include <atomic>
#include <cctype>
#include <chrono>
#include <cmath>
#include <ctime>
#include <memory>
#include <regex>
#include <string>
#include <string_view>
#include <thread>
#include <unordered_set>
#include <vector>

namespace se {

// Built-in filter preset extensions (matching Voidtools Everything defaults + Code)
inline const std::unordered_set<std::string> &preset_extensions(SEFilterPreset preset) {
    static const std::unordered_set<std::string> empty_set;
    static const std::unordered_set<std::string> audio_set = {
        "aac","ac3","aif","aifc","aiff","au","cda","dts","fla","flac","it","m1a","m2a","m3u",
        "m4a","m4b","m4p","mid","midi","mka","mod","mp2","mp3","mpa","ogg","opus","ra","rmi",
        "snd","wav","wma","wv"
    };
    static const std::unordered_set<std::string> compressed_set = {
        "7z","ace","arj","bz2","cab","dmg","gz","gzip","iso","jar","lha","lz","lzma","rar",
        "r00","r01","tar","tbz","tbz2","tgz","txz","xz","z","zip","zst"
    };
    static const std::unordered_set<std::string> doc_set = {
        "c","chm","cpp","csv","cxx","doc","docm","docx","dot","dotm","dotx","epub","h","hpp",
        "htm","html","hxx","ini","java","key","log","lua","md","markdown","numbers","odp","ods",
        "odt","pages","pdf","pot","potm","potx","ppam","pps","ppsm","ppsx","ppt","pptm","pptx",
        "ps","rtf","tex","txt","vsd","wpd","wps","xls","xlsb","xlsm","xlsx","xlt","xml","yaml","yml"
    };
    static const std::unordered_set<std::string> exec_set = {
        "app","bat","cmd","com","command","dmg","exe","msi","pkg","scr","sh","zsh","bash"
    };
    static const std::unordered_set<std::string> pic_set = {
        "ani","avif","bmp","cr2","cr3","dng","gif","heic","heif","ico","jpe","jpeg","jpg",
        "nef","pcx","png","psd","raw","svg","tga","tif","tiff","webp","xcf"
    };
    static const std::unordered_set<std::string> video_set = {
        "3g2","3gp","3gp2","3gpp","amr","amv","asf","avi","bdmv","bik","d2v","divx","drc",
        "dsa","dsm","dss","dsv","evo","f4v","flc","fli","flic","flv","hdmov","ifo","ivf","m1v",
        "m2p","m2t","m2ts","m2v","m4b","m4p","m4v","mkv","mp2v","mp4","mp4v","mpe","mpeg","mpg",
        "mpls","mpv2","mpv4","mov","mts","ogm","ogv","pss","pva","qt","ram","ratdvd","rm","rmm",
        "rmvb","roq","rpm","smil","smk","swf","tp","tpr","ts","vob","vp6","webm","wm","wmp","wmv"
    };
    static const std::unordered_set<std::string> code_set = {
        "c","cc","cpp","cs","css","cxx","dart","go","h","hh","hpp","html","hxx","java","js",
        "json","jsx","kt","kts","lua","m","mm","php","pl","py","r","rb","rs","scala","scss",
        "sh","sql","swift","toml","ts","tsx","vue","xml","yaml","yml","zig"
    };

    switch (preset) {
        case SE_FILTER_AUDIO:      return audio_set;
        case SE_FILTER_COMPRESSED: return compressed_set;
        case SE_FILTER_DOCUMENT:   return doc_set;
        case SE_FILTER_EXECUTABLE: return exec_set;
        case SE_FILTER_PICTURE:    return pic_set;
        case SE_FILTER_VIDEO:      return video_set;
        case SE_FILTER_CODE:       return code_set;
        default:                   return empty_set;
    }
}

static inline std::string to_lower_ascii(std::string_view s) {
    std::string out;
    out.resize(s.size());
    for (size_t i = 0; i < s.size(); ++i) out[i] = ascii_lower(s[i]);
    return out;
}

static inline bool is_word_char(char c) {
    return std::isalnum(static_cast<unsigned char>(c)) || c == '_';
}

// Fast glob wildcard matching (* and ?)
static bool wildcard_match(std::string_view pattern, std::string_view text) {
    size_t p = 0, t = 0;
    size_t star_p = std::string_view::npos;
    size_t star_t = 0;

    while (t < text.size()) {
        if (p < pattern.size() && (pattern[p] == '?' || pattern[p] == text[t])) {
            ++p;
            ++t;
        } else if (p < pattern.size() && pattern[p] == '*') {
            star_p = p++;
            star_t = t;
        } else if (star_p != std::string_view::npos) {
            p = star_p + 1;
            t = ++star_t;
        } else {
            return false;
        }
    }
    while (p < pattern.size() && pattern[p] == '*') ++p;
    return p == pattern.size();
}

static bool whole_word_find(std::string_view haystack, std::string_view needle) {
    if (needle.empty()) return true;
    size_t pos = 0;
    while ((pos = haystack.find(needle, pos)) != std::string_view::npos) {
        bool left_ok = (pos == 0) || !is_word_char(haystack[pos - 1]);
        size_t end = pos + needle.size();
        bool right_ok = (end == haystack.size()) || !is_word_char(haystack[end]);
        if (left_ok && right_ok) return true;
        ++pos;
    }
    return false;
}

enum class TermKind {
    SUBSTRING,
    WILDCARD,
    REGEX,
    STARTWITH,
    ENDWITH,
    EXT_LIST,
    ONLY_FILES,
    ONLY_FOLDERS,
    SIZE_RANGE,
    DATE_MOD_RANGE,
    DATE_CR_RANGE,
    PARENT_EXACT,
    LEN_RANGE,
    DEPTH_RANGE,
    EMPTY_ONLY
};

struct QueryTerm {
    TermKind kind = TermKind::SUBSTRING;
    bool negated = false;
    bool match_case = false;
    bool match_path = false;
    bool whole_word = false;

    std::string raw_text;
    std::string lower_text;
    std::vector<std::string> ext_list; // lowercased
    std::shared_ptr<std::regex> regex_obj;

    int64_t range_min = INT64_MIN;
    int64_t range_max = INT64_MAX;
    bool has_min = false;
    bool has_max = false;
};

// DNF-like expression tree: OR of AND-groups, with recursive subgroup support via <...>
struct QueryNode {
    enum class Op { TERM, AND, OR } op = Op::TERM;
    bool negated = false;
    QueryTerm term;
    std::vector<QueryNode> children;
};

class QueryParser {
public:
    static QueryNode parse(std::string_view input,
                           const SEQueryOptions &opts,
                           std::vector<std::string> &out_highlight_terms) {
        out_highlight_terms.clear();
        auto tokens = tokenize(input);
        size_t pos = 0;
        return parse_or(tokens, pos, opts, out_highlight_terms);
    }

private:
    struct Token {
        enum Type { WORD, PIPE, LANGLE, RANGLE } type = WORD;
        std::string text;
        bool was_quoted = false;
    };

    static std::vector<Token> tokenize(std::string_view s) {
        std::vector<Token> out;
        size_t i = 0;
        while (i < s.size()) {
            while (i < s.size() && std::isspace(static_cast<unsigned char>(s[i]))) ++i;
            if (i >= s.size()) break;

            if (s[i] == '|') {
                out.push_back({Token::PIPE, "|", false});
                ++i;
                continue;
            }
            if (s[i] == '<') {
                out.push_back({Token::LANGLE, "<", false});
                ++i;
                continue;
            }
            if (s[i] == '>') {
                out.push_back({Token::RANGLE, ">", false});
                ++i;
                continue;
            }

            // Read a word, respecting quotes inside tokens (e.g. path:"My Folder" or "foo bar")
            std::string tok;
            bool quoted = false;
            while (i < s.size()) {
                char c = s[i];
                if (c == '"') {
                    quoted = true;
                    ++i;
                    while (i < s.size() && s[i] != '"') {
                        tok.push_back(s[i++]);
                    }
                    if (i < s.size() && s[i] == '"') ++i;
                } else if (std::isspace(static_cast<unsigned char>(c)) || c == '|' || c == '<' || c == '>') {
                    // Allow '>' and '<' inside functions like size:>10mb or dm:<2026-01-01
                    if ((c == '>' || c == '<') && !tok.empty() && tok.find(':') != std::string::npos) {
                        tok.push_back(c);
                        ++i;
                    } else {
                        break;
                    }
                } else {
                    tok.push_back(c);
                    ++i;
                }
            }
            if (!tok.empty()) {
                out.push_back({Token::WORD, std::move(tok), quoted});
            }
        }
        return out;
    }

    static QueryNode parse_or(const std::vector<Token> &toks,
                              size_t &pos,
                              const SEQueryOptions &opts,
                              std::vector<std::string> &hl) {
        QueryNode left = parse_and(toks, pos, opts, hl);
        std::vector<QueryNode> branches;
        branches.push_back(std::move(left));
        while (pos < toks.size() && toks[pos].type == Token::PIPE) {
            ++pos;
            branches.push_back(parse_and(toks, pos, opts, hl));
        }
        if (branches.size() == 1) return std::move(branches[0]);
        QueryNode node;
        node.op = QueryNode::Op::OR;
        node.children = std::move(branches);
        return node;
    }

    static QueryNode parse_and(const std::vector<Token> &toks,
                               size_t &pos,
                               const SEQueryOptions &opts,
                               std::vector<std::string> &hl) {
        std::vector<QueryNode> parts;
        while (pos < toks.size() && toks[pos].type != Token::PIPE && toks[pos].type != Token::RANGLE) {
            if (toks[pos].type == Token::LANGLE) {
                ++pos;
                parts.push_back(parse_or(toks, pos, opts, hl));
                if (pos < toks.size() && toks[pos].type == Token::RANGLE) ++pos;
            } else if (toks[pos].type == Token::WORD) {
                if (toks[pos].text == "!" && pos + 1 < toks.size() && toks[pos + 1].type == Token::LANGLE) {
                    pos += 2;
                    QueryNode sub = parse_or(toks, pos, opts, hl);
                    if (pos < toks.size() && toks[pos].type == Token::RANGLE) ++pos;
                    sub.negated = !sub.negated;
                    parts.push_back(std::move(sub));
                } else {
                    parts.push_back(parse_term(toks[pos], opts, hl));
                    ++pos;
                }
            } else {
                ++pos;
            }
        }
        if (parts.empty()) {
            QueryNode empty_and;
            empty_and.op = QueryNode::Op::AND;
            return empty_and;
        }
        if (parts.size() == 1) return std::move(parts[0]);
        QueryNode node;
        node.op = QueryNode::Op::AND;
        node.children = std::move(parts);
        return node;
    }

    static int64_t parse_size_bytes(std::string_view sv) {
        std::string s = to_lower_ascii(sv);
        if (s.empty()) return 0;
        double val = 0;
        size_t idx = 0;
        try {
            val = std::stod(s, &idx);
        } catch (...) {
            return 0;
        }
        std::string unit = s.substr(idx);
        double mult = 1.0;
        if (unit == "k" || unit == "kb" || unit == "kib") mult = 1024.0;
        else if (unit == "m" || unit == "mb" || unit == "mib") mult = 1024.0 * 1024.0;
        else if (unit == "g" || unit == "gb" || unit == "gib") mult = 1024.0 * 1024.0 * 1024.0;
        else if (unit == "t" || unit == "tb" || unit == "tib") mult = 1024.0 * 1024.0 * 1024.0 * 1024.0;
        return static_cast<int64_t>(val * mult);
    }

    static void parse_numeric_range(std::string_view val, bool is_size, QueryTerm &t) {
        std::string s = to_lower_ascii(val);
        auto parse_v = [&](std::string_view p) -> int64_t {
            if (is_size) return parse_size_bytes(p);
            try { return std::stoll(std::string(p)); } catch (...) { return 0; }
        };

        if (is_size) {
            if (s == "empty") { t.has_min = true; t.has_max = true; t.range_min = 0; t.range_max = 0; return; }
            if (s == "tiny") { t.has_min = true; t.has_max = true; t.range_min = 0; t.range_max = 10240; return; }
            if (s == "small") { t.has_min = true; t.has_max = true; t.range_min = 10240; t.range_max = 102400; return; }
            if (s == "medium") { t.has_min = true; t.has_max = true; t.range_min = 102400; t.range_max = 1048576; return; }
            if (s == "large") { t.has_min = true; t.has_max = true; t.range_min = 1048576; t.range_max = 16777216; return; }
            if (s == "huge") { t.has_min = true; t.has_max = true; t.range_min = 16777216; t.range_max = 134217728; return; }
            if (s == "gigantic") { t.has_min = true; t.range_min = 134217728; return; }
        }

        size_t dots = s.find("..");
        if (dots != std::string::npos) {
            std::string lo = s.substr(0, dots);
            std::string hi = s.substr(dots + 2);
            if (!lo.empty()) { t.has_min = true; t.range_min = parse_v(lo); }
            if (!hi.empty()) { t.has_max = true; t.range_max = parse_v(hi); }
            return;
        }
        if (s.rfind(">=", 0) == 0) { t.has_min = true; t.range_min = parse_v(s.substr(2)); }
        else if (s.rfind("<=", 0) == 0) { t.has_max = true; t.range_max = parse_v(s.substr(2)); }
        else if (s.rfind(">", 0) == 0) { t.has_min = true; t.range_min = parse_v(s.substr(1)) + 1; }
        else if (s.rfind("<", 0) == 0) { t.has_max = true; t.range_max = parse_v(s.substr(1)) - 1; }
        else {
            int64_t exact = parse_v(s);
            t.has_min = true; t.has_max = true;
            t.range_min = exact; t.range_max = exact;
        }
    }

    static int64_t parse_date_epoch(std::string_view sv, bool end_of_day) {
        // Supports YYYY-MM-DD, YYYY-MM, YYYY
        int y = 0, m = 1, d = 1;
        std::string s(sv);
        int parsed = std::sscanf(s.c_str(), "%d-%d-%d", &y, &m, &d);
        if (parsed < 1 || y < 1970) return 0;
        struct tm tmv{};
        tmv.tm_year = y - 1900;
        tmv.tm_mon = (parsed >= 2 ? m : (end_of_day ? 12 : 1)) - 1;
        tmv.tm_mday = (parsed >= 3 ? d : (end_of_day ? 31 : 1));
        tmv.tm_hour = end_of_day ? 23 : 0;
        tmv.tm_min = end_of_day ? 59 : 0;
        tmv.tm_sec = end_of_day ? 59 : 0;
        tmv.tm_isdst = -1;
        return static_cast<int64_t>(std::mktime(&tmv));
    }

    static void parse_date_range(std::string_view val, QueryTerm &t) {
        std::string s = to_lower_ascii(val);
        time_t now = std::time(nullptr);
        struct tm lt{};
        localtime_r(&now, &lt);

        auto start_of_today = [&]() -> int64_t {
            struct tm d = lt;
            d.tm_hour = 0; d.tm_min = 0; d.tm_sec = 0; d.tm_isdst = -1;
            return static_cast<int64_t>(std::mktime(&d));
        };

        if (s == "today") {
            t.has_min = true; t.range_min = start_of_today();
            return;
        }
        if (s == "yesterday") {
            int64_t tod = start_of_today();
            t.has_min = true; t.range_min = tod - 86400;
            t.has_max = true; t.range_max = tod - 1;
            return;
        }
        if (s == "thisweek" || s == "last7days") {
            t.has_min = true; t.range_min = start_of_today() - 7 * 86400;
            return;
        }
        if (s == "thismonth") {
            struct tm d = lt;
            d.tm_mday = 1; d.tm_hour = 0; d.tm_min = 0; d.tm_sec = 0; d.tm_isdst = -1;
            t.has_min = true; t.range_min = static_cast<int64_t>(std::mktime(&d));
            return;
        }
        if (s == "thisyear") {
            struct tm d = lt;
            d.tm_mon = 0; d.tm_mday = 1; d.tm_hour = 0; d.tm_min = 0; d.tm_sec = 0; d.tm_isdst = -1;
            t.has_min = true; t.range_min = static_cast<int64_t>(std::mktime(&d));
            return;
        }
        if (s.rfind("last", 0) == 0 && s.size() > 5) {
            // e.g. last24hours, last30days, last10mins
            int n = 0;
            char unit[16] = {0};
            if (std::sscanf(s.c_str() + 4, "%d%15s", &n, unit) == 2 && n > 0) {
                std::string u(unit);
                int64_t mult = 86400;
                if (u.rfind("min", 0) == 0) mult = 60;
                else if (u.rfind("hour", 0) == 0 || u == "h") mult = 3600;
                else if (u.rfind("day", 0) == 0 || u == "d") mult = 86400;
                else if (u.rfind("week", 0) == 0 || u == "w") mult = 7 * 86400;
                else if (u.rfind("month", 0) == 0) mult = 30 * 86400;
                t.has_min = true;
                t.range_min = static_cast<int64_t>(now) - static_cast<int64_t>(n) * mult;
                return;
            }
        }

        size_t dots = s.find("..");
        if (dots != std::string::npos) {
            std::string lo = s.substr(0, dots);
            std::string hi = s.substr(dots + 2);
            if (!lo.empty()) { t.has_min = true; t.range_min = parse_date_epoch(lo, false); }
            if (!hi.empty()) { t.has_max = true; t.range_max = parse_date_epoch(hi, true); }
            return;
        }
        if (s.rfind(">=", 0) == 0) { t.has_min = true; t.range_min = parse_date_epoch(s.substr(2), false); }
        else if (s.rfind("<=", 0) == 0) { t.has_max = true; t.range_max = parse_date_epoch(s.substr(2), true); }
        else if (s.rfind(">", 0) == 0) { t.has_min = true; t.range_min = parse_date_epoch(s.substr(1), true) + 1; }
        else if (s.rfind("<", 0) == 0) { t.has_max = true; t.range_max = parse_date_epoch(s.substr(1), false) - 1; }
        else {
            t.has_min = true; t.range_min = parse_date_epoch(s, false);
            t.has_max = true; t.range_max = parse_date_epoch(s, true);
        }
    }

    static QueryNode parse_term(const Token &tok,
                                const SEQueryOptions &opts,
                                std::vector<std::string> &hl) {
        QueryNode node;
        node.op = QueryNode::Op::TERM;
        QueryTerm &t = node.term;
        t.match_case = opts.match_case;
        t.match_path = opts.match_path;
        t.whole_word = opts.match_whole_word;

        std::string_view sv = tok.text;
        if (!tok.was_quoted && !sv.empty() && sv[0] == '!' && sv.size() > 1) {
            t.negated = true;
            sv.remove_prefix(1);
        }

        // Parse Everything modifiers & functions (can chain modifiers like case:path:foo)
        bool force_regex = opts.match_regex;
        bool explicit_func = false;

        while (!tok.was_quoted) {
            size_t colon = sv.find(':');
            if (colon == std::string_view::npos) break;
            std::string prefix = to_lower_ascii(sv.substr(0, colon));
            std::string_view rest = sv.substr(colon + 1);

            if (prefix == "case") { t.match_case = true; sv = rest; }
            else if (prefix == "nocase") { t.match_case = false; sv = rest; }
            else if (prefix == "path") { t.match_path = true; sv = rest; }
            else if (prefix == "nopath") { t.match_path = false; sv = rest; }
            else if (prefix == "ww" || prefix == "wholeword") { t.whole_word = true; sv = rest; }
            else if (prefix == "noww") { t.whole_word = false; sv = rest; }
            else if (prefix == "regex") { force_regex = true; sv = rest; }
            else if (prefix == "noregex") { force_regex = false; sv = rest; }
            else if (prefix == "file" || prefix == "files") {
                if (rest.empty()) {
                    t.kind = TermKind::ONLY_FILES;
                    explicit_func = true;
                    break;
                }
                // file:foo -> ONLY_FILES AND foo
                QueryNode and_node;
                and_node.op = QueryNode::Op::AND;
                and_node.negated = t.negated;
                QueryNode fnode;
                fnode.op = QueryNode::Op::TERM;
                fnode.term.kind = TermKind::ONLY_FILES;
                Token sub_tok{Token::WORD, std::string(rest), false};
                and_node.children.push_back(std::move(fnode));
                and_node.children.push_back(parse_term(sub_tok, opts, hl));
                return and_node;
            }
            else if (prefix == "folder" || prefix == "folders" || prefix == "dir") {
                if (rest.empty()) {
                    t.kind = TermKind::ONLY_FOLDERS;
                    explicit_func = true;
                    break;
                }
                QueryNode and_node;
                and_node.op = QueryNode::Op::AND;
                and_node.negated = t.negated;
                QueryNode dnode;
                dnode.op = QueryNode::Op::TERM;
                dnode.term.kind = TermKind::ONLY_FOLDERS;
                Token sub_tok{Token::WORD, std::string(rest), false};
                and_node.children.push_back(std::move(dnode));
                and_node.children.push_back(parse_term(sub_tok, opts, hl));
                return and_node;
            }
            else if (prefix == "ext") {
                t.kind = TermKind::EXT_LIST;
                std::string lower_rest = to_lower_ascii(rest);
                size_t start = 0;
                while (start <= lower_rest.size()) {
                    size_t semi = lower_rest.find(';', start);
                    std::string item = (semi == std::string::npos)
                        ? lower_rest.substr(start)
                        : lower_rest.substr(start, semi - start);
                    if (!item.empty() && item[0] == '.') item.erase(0, 1);
                    if (!item.empty()) t.ext_list.push_back(std::move(item));
                    if (semi == std::string::npos) break;
                    start = semi + 1;
                }
                explicit_func = true;
                break;
            }
            else if (prefix == "size") {
                t.kind = TermKind::SIZE_RANGE;
                parse_numeric_range(rest, true, t);
                explicit_func = true;
                break;
            }
            else if (prefix == "dm" || prefix == "datemodified") {
                t.kind = TermKind::DATE_MOD_RANGE;
                parse_date_range(rest, t);
                explicit_func = true;
                break;
            }
            else if (prefix == "dc" || prefix == "datecreated") {
                t.kind = TermKind::DATE_CR_RANGE;
                parse_date_range(rest, t);
                explicit_func = true;
                break;
            }
            else if (prefix == "parent") {
                t.kind = TermKind::PARENT_EXACT;
                std::string p(rest);
                while (p.size() > 1 && p.back() == '/') p.pop_back();
                t.raw_text = p;
                t.lower_text = to_lower_ascii(p);
                explicit_func = true;
                break;
            }
            else if (prefix == "startwith") {
                t.kind = TermKind::STARTWITH;
                t.raw_text = std::string(rest);
                t.lower_text = to_lower_ascii(rest);
                if (!t.negated && !t.raw_text.empty()) hl.push_back(t.raw_text);
                explicit_func = true;
                break;
            }
            else if (prefix == "endwith") {
                t.kind = TermKind::ENDWITH;
                t.raw_text = std::string(rest);
                t.lower_text = to_lower_ascii(rest);
                if (!t.negated && !t.raw_text.empty()) hl.push_back(t.raw_text);
                explicit_func = true;
                break;
            }
            else if (prefix == "len") {
                t.kind = TermKind::LEN_RANGE;
                parse_numeric_range(rest, false, t);
                explicit_func = true;
                break;
            }
            else if (prefix == "depth" || prefix == "parents") {
                t.kind = TermKind::DEPTH_RANGE;
                parse_numeric_range(rest, false, t);
                explicit_func = true;
                break;
            }
            else if (prefix == "empty") {
                t.kind = TermKind::EMPTY_ONLY;
                explicit_func = true;
                break;
            }
            else {
                // Unknown colon prefix -> treat entire token as literal filename/path search
                break;
            }
        }

        if (!explicit_func) {
            // Everything rule: if term contains '/', automatically match against full path!
            if (sv.find('/') != std::string_view::npos) {
                t.match_path = true;
            }

            t.raw_text.assign(sv.data(), sv.size());
            t.lower_text = to_lower_ascii(sv);

            if (force_regex) {
                t.kind = TermKind::REGEX;
                try {
                    auto flags = std::regex::ECMAScript | std::regex::optimize;
                    if (!t.match_case) flags |= std::regex::icase;
                    t.regex_obj = std::make_shared<std::regex>(t.raw_text, flags);
                } catch (...) {
                    // Invalid regex while user is still typing -> match nothing gracefully
                    t.regex_obj.reset();
                }
            } else if (!tok.was_quoted && (sv.find('*') != std::string_view::npos || sv.find('?') != std::string_view::npos)) {
                t.kind = TermKind::WILDCARD;
            } else {
                t.kind = TermKind::SUBSTRING;
                if (!t.negated && !t.raw_text.empty()) {
                    hl.push_back(t.raw_text);
                }
            }
        }

        return node;
    }
};

class QueryEngine {
public:
    static bool eval_node(const QueryNode &node,
                          const IndexStore &store,
                          uint32_t idx,
                          const Entry &e,
                          std::string &scratch_path,
                          bool &has_path,
                          std::string &scratch_lower_path,
                          bool &has_lower_path) {
        bool res = true;
        switch (node.op) {
            case QueryNode::Op::TERM:
                res = eval_term(node.term, store, idx, e, scratch_path, has_path, scratch_lower_path, has_lower_path);
                break;
            case QueryNode::Op::AND:
                res = true;
                for (const auto &ch : node.children) {
                    if (!eval_node(ch, store, idx, e, scratch_path, has_path, scratch_lower_path, has_lower_path)) {
                        res = false;
                        break;
                    }
                }
                break;
            case QueryNode::Op::OR:
                res = false;
                for (const auto &ch : node.children) {
                    if (eval_node(ch, store, idx, e, scratch_path, has_path, scratch_lower_path, has_lower_path)) {
                        res = true;
                        break;
                    }
                }
                break;
        }
        return node.negated ? !res : res;
    }

    static bool eval_term(const QueryTerm &t,
                          const IndexStore &store,
                          uint32_t idx,
                          const Entry &e,
                          std::string &scratch_path,
                          bool &has_path,
                          std::string &scratch_lower_path,
                          bool &has_lower_path) {
        bool ok = false;
        switch (t.kind) {
            case TermKind::ONLY_FILES:
                ok = !e.is_dir();
                break;
            case TermKind::ONLY_FOLDERS:
                ok = e.is_dir();
                break;
            case TermKind::EMPTY_ONLY:
                ok = e.is_dir() ? (e.first_child == INVALID_IDX) : (e.size == 0);
                break;
            case TermKind::EXT_LIST: {
                if (!e.is_dir() && e.ext_offset != NO_EXT) {
                    std::string_view ext = store.lower_ext_of(e);
                    for (const auto &wanted : t.ext_list) {
                        if (ext == wanted) { ok = true; break; }
                    }
                }
                break;
            }
            case TermKind::SIZE_RANGE: {
                int64_t sz = static_cast<int64_t>(e.size);
                ok = (!t.has_min || sz >= t.range_min) && (!t.has_max || sz <= t.range_max);
                break;
            }
            case TermKind::DATE_MOD_RANGE:
                ok = (!t.has_min || e.mtime >= t.range_min) && (!t.has_max || e.mtime <= t.range_max);
                break;
            case TermKind::DATE_CR_RANGE:
                ok = (!t.has_min || e.ctime >= t.range_min) && (!t.has_max || e.ctime <= t.range_max);
                break;
            case TermKind::LEN_RANGE: {
                int64_t len = static_cast<int64_t>(e.name_len);
                ok = (!t.has_min || len >= t.range_min) && (!t.has_max || len <= t.range_max);
                break;
            }
            case TermKind::DEPTH_RANGE: {
                int64_t d = static_cast<int64_t>(e.depth);
                ok = (!t.has_min || d >= t.range_min) && (!t.has_max || d <= t.range_max);
                break;
            }
            case TermKind::PARENT_EXACT: {
                std::string parent_p;
                store.build_parent_path(idx, parent_p);
                if (t.match_case) {
                    ok = (parent_p == t.raw_text);
                } else {
                    ok = (to_lower_ascii(parent_p) == t.lower_text);
                }
                break;
            }
            case TermKind::SUBSTRING:
            case TermKind::WILDCARD:
            case TermKind::REGEX:
            case TermKind::STARTWITH:
            case TermKind::ENDWITH: {
                std::string_view target;
                const std::string &needle = t.match_case ? t.raw_text : t.lower_text;
                if (t.match_path) {
                    if (t.match_case) {
                        if (!has_path) { store.build_full_path(idx, scratch_path); has_path = true; }
                        target = scratch_path;
                    } else {
                        if (!has_lower_path) { store.build_lower_full_path(idx, scratch_lower_path); has_lower_path = true; }
                        target = scratch_lower_path;
                    }
                } else {
                    target = t.match_case ? store.name_of(e) : store.lower_name_of(e);
                }

                if (t.kind == TermKind::SUBSTRING) {
                    if (needle.empty()) {
                        ok = true;
                    } else if (t.whole_word) {
                        ok = whole_word_find(target, needle);
                    } else {
                        ok = (target.find(needle) != std::string_view::npos);
                    }
                } else if (t.kind == TermKind::STARTWITH) {
                    ok = target.size() >= needle.size() && target.substr(0, needle.size()) == needle;
                } else if (t.kind == TermKind::ENDWITH) {
                    ok = target.size() >= needle.size() && target.substr(target.size() - needle.size()) == needle;
                } else if (t.kind == TermKind::WILDCARD) {
                    ok = wildcard_match(needle, target);
                } else if (t.kind == TermKind::REGEX) {
                    if (t.regex_obj) {
                        ok = std::regex_search(target.begin(), target.end(), *t.regex_obj);
                    } else {
                        ok = false;
                    }
                }
                break;
            }
        }
        return t.negated ? !ok : ok;
    }

    static bool matches_preset(const IndexStore &store, const Entry &e, SEFilterPreset preset) {
        if (preset == SE_FILTER_EVERYTHING) return true;
        if (preset == SE_FILTER_FOLDER) return e.is_dir();
        if (e.is_dir()) {
            // macOS .app bundles are directories! Treat .app as executable under Executable filter
            if (preset == SE_FILTER_EXECUTABLE) {
                std::string_view nm = store.lower_name_of(e);
                return nm.size() > 4 && nm.substr(nm.size() - 4) == ".app";
            }
            return false;
        }
        if (e.ext_offset == NO_EXT) return false;
        const auto &exts = preset_extensions(preset);
        std::string ext(store.lower_ext_of(e));
        return exts.find(ext) != exts.end();
    }

    struct SearchOutput {
        std::vector<uint32_t> matches;
        uint64_t total_size = 0;
        uint64_t elapsed_us = 0;
        std::vector<std::string> highlight_terms;
        bool cancelled = false;
    };

    static SearchOutput execute(const IndexStore &store,
                                std::string_view query,
                                const SEQueryOptions &opts,
                                const std::atomic<uint64_t> &latest_gen,
                                uint64_t my_gen) {
        auto t0 = std::chrono::steady_clock::now();
        SearchOutput out;

        std::shared_lock lock(store.mutex);
        size_t n = store.entries.size();
        if (n == 0) return out;

        // Trim whitespace
        while (!query.empty() && std::isspace(static_cast<unsigned char>(query.front()))) query.remove_prefix(1);
        while (!query.empty() && std::isspace(static_cast<unsigned char>(query.back()))) query.remove_suffix(1);

        bool empty_query = query.empty();
        QueryNode root_node;
        if (!empty_query) {
            root_node = QueryParser::parse(query, opts, out.highlight_terms);
        }

        // Parallel chunked scan for large indexes (> 80k entries)
        unsigned num_threads = (n >= 80'000) ? std::min(8u, std::max(1u, std::thread::hardware_concurrency())) : 1u;
        std::vector<std::vector<uint32_t>> thread_results(num_threads);
        std::vector<uint64_t> thread_sizes(num_threads, 0);
        std::atomic<bool> was_cancelled{false};

        auto run_chunk = [&](unsigned tid) {
            size_t chunk = (n + num_threads - 1) / num_threads;
            size_t start = tid * chunk;
            size_t end = std::min(n, start + chunk);
            if (start >= end) return;

            auto &local = thread_results[tid];
            local.reserve(empty_query ? (end - start) : 2048);
            uint64_t local_sz = 0;

            std::string scratch_path;
            std::string scratch_lower_path;
            scratch_path.reserve(256);
            scratch_lower_path.reserve(256);

            for (size_t i = start; i < end; ++i) {
                if ((i & 0x3FFF) == 0 && latest_gen.load(std::memory_order_relaxed) != my_gen) {
                    was_cancelled.store(true, std::memory_order_relaxed);
                    return;
                }
                const Entry &e = store.entries[i];
                if (e.is_deleted() || e.parent_idx == INVALID_IDX) continue;
                if (opts.filter_preset != SE_FILTER_EVERYTHING && !matches_preset(store, e, opts.filter_preset)) {
                    continue;
                }

                if (empty_query) {
                    local.push_back(static_cast<uint32_t>(i));
                    if (!e.is_dir()) local_sz += e.size;
                } else {
                    bool has_p = false, has_lp = false;
                    if (eval_node(root_node, store, static_cast<uint32_t>(i), e, scratch_path, has_p, scratch_lower_path, has_lp)) {
                        local.push_back(static_cast<uint32_t>(i));
                        if (!e.is_dir()) local_sz += e.size;
                    }
                }
            }
            thread_sizes[tid] = local_sz;
        };

        if (num_threads == 1) {
            run_chunk(0);
        } else {
            std::vector<std::thread> threads;
            threads.reserve(num_threads);
            for (unsigned t = 0; t < num_threads; ++t) {
                threads.emplace_back(run_chunk, t);
            }
            for (auto &th : threads) th.join();
        }

        if (was_cancelled.load(std::memory_order_relaxed) || latest_gen.load(std::memory_order_relaxed) != my_gen) {
            out.cancelled = true;
            return out;
        }

        size_t total_count = 0;
        uint64_t total_sz = 0;
        for (unsigned t = 0; t < num_threads; ++t) {
            total_count += thread_results[t].size();
            total_sz += thread_sizes[t];
        }

        out.matches.reserve(total_count);
        for (unsigned t = 0; t < num_threads; ++t) {
            out.matches.insert(out.matches.end(), thread_results[t].begin(), thread_results[t].end());
        }
        out.total_size = total_sz;

        // Sort results (skip full path string sort if > 300k items to keep UI instantaneous; sort fast columns always)
        sort_results(store, out.matches, opts, latest_gen, my_gen);

        if (latest_gen.load(std::memory_order_relaxed) != my_gen) {
            out.cancelled = true;
            return out;
        }

        auto t1 = std::chrono::steady_clock::now();
        out.elapsed_us = static_cast<uint64_t>(
            std::chrono::duration_cast<std::chrono::microseconds>(t1 - t0).count()
        );
        return out;
    }

    static void sort_results(const IndexStore &store,
                             std::vector<uint32_t> &ids,
                             const SEQueryOptions &opts,
                             const std::atomic<uint64_t> &latest_gen,
                             uint64_t my_gen) {
        if (ids.size() <= 1) return;
        const bool asc = opts.sort_ascending;
        const bool folders_first = opts.folders_first;

        auto cmp_folder_prefix = [&](const Entry &a, const Entry &b, int &res) -> bool {
            if (!folders_first) return false;
            bool da = a.is_dir();
            bool db = b.is_dir();
            if (da != db) {
                res = da ? -1 : 1;
                return true;
            }
            return false;
        };

        // For very large unfiltered result sets (> 400k rows), partial_sort top 100k or fast numeric sort
        if (opts.sort_column == SE_SORT_PATH && ids.size() > 250'000) {
            // Avoid constructing millions of temporary path strings on empty query
            return;
        }

        switch (opts.sort_column) {
            case SE_SORT_NAME: {
                // Fast ASCII-lower lexicographic comparison directly in arena (zero allocations)
                std::sort(ids.begin(), ids.end(), [&](uint32_t ia, uint32_t ib) {
                    const Entry &a = store.entries[ia];
                    const Entry &b = store.entries[ib];
                    int r = 0;
                    if (cmp_folder_prefix(a, b, r)) return r < 0;
                    auto na = store.lower_name_of(a);
                    auto nb = store.lower_name_of(b);
                    int c = na.compare(nb);
                    if (c == 0) c = (ia < ib) ? -1 : 1;
                    return asc ? (c < 0) : (c > 0);
                });
                break;
            }
            case SE_SORT_SIZE: {
                std::sort(ids.begin(), ids.end(), [&](uint32_t ia, uint32_t ib) {
                    const Entry &a = store.entries[ia];
                    const Entry &b = store.entries[ib];
                    int r = 0;
                    if (cmp_folder_prefix(a, b, r)) return r < 0;
                    if (a.size != b.size) return asc ? (a.size < b.size) : (a.size > b.size);
                    return store.lower_name_of(a) < store.lower_name_of(b);
                });
                break;
            }
            case SE_SORT_DATE_MODIFIED: {
                std::sort(ids.begin(), ids.end(), [&](uint32_t ia, uint32_t ib) {
                    const Entry &a = store.entries[ia];
                    const Entry &b = store.entries[ib];
                    int r = 0;
                    if (cmp_folder_prefix(a, b, r)) return r < 0;
                    if (a.mtime != b.mtime) return asc ? (a.mtime < b.mtime) : (a.mtime > b.mtime);
                    return store.lower_name_of(a) < store.lower_name_of(b);
                });
                break;
            }
            case SE_SORT_DATE_CREATED: {
                std::sort(ids.begin(), ids.end(), [&](uint32_t ia, uint32_t ib) {
                    const Entry &a = store.entries[ia];
                    const Entry &b = store.entries[ib];
                    int r = 0;
                    if (cmp_folder_prefix(a, b, r)) return r < 0;
                    if (a.ctime != b.ctime) return asc ? (a.ctime < b.ctime) : (a.ctime > b.ctime);
                    return store.lower_name_of(a) < store.lower_name_of(b);
                });
                break;
            }
            case SE_SORT_EXTENSION: {
                std::sort(ids.begin(), ids.end(), [&](uint32_t ia, uint32_t ib) {
                    const Entry &a = store.entries[ia];
                    const Entry &b = store.entries[ib];
                    int r = 0;
                    if (cmp_folder_prefix(a, b, r)) return r < 0;
                    auto ea = store.lower_ext_of(a);
                    auto eb = store.lower_ext_of(b);
                    int c = ea.compare(eb);
                    if (c == 0) c = store.lower_name_of(a).compare(store.lower_name_of(b));
                    return asc ? (c < 0) : (c > 0);
                });
                break;
            }
            case SE_SORT_PATH: {
                // Pre-cache parent paths by parent_idx (shared by siblings -> huge speedup!)
                std::unordered_map<uint32_t, std::string> pcache;
                pcache.reserve(std::min<size_t>(ids.size(), 65536));
                auto get_p = [&](uint32_t pidx) -> const std::string & {
                    auto it = pcache.find(pidx);
                    if (it != pcache.end()) return it->second;
                    std::string s;
                    if (pidx != INVALID_IDX) store.build_lower_full_path(pidx, s);
                    return pcache.emplace(pidx, std::move(s)).first->second;
                };
                for (uint32_t id : ids) {
                    if (latest_gen.load(std::memory_order_relaxed) != my_gen) return;
                    get_p(store.entries[id].parent_idx);
                }
                std::sort(ids.begin(), ids.end(), [&](uint32_t ia, uint32_t ib) {
                    const Entry &a = store.entries[ia];
                    const Entry &b = store.entries[ib];
                    int r = 0;
                    if (cmp_folder_prefix(a, b, r)) return r < 0;
                    const auto &pa = pcache[a.parent_idx];
                    const auto &pb = pcache[b.parent_idx];
                    int c = pa.compare(pb);
                    if (c == 0) c = store.lower_name_of(a).compare(store.lower_name_of(b));
                    return asc ? (c < 0) : (c > 0);
                });
                break;
            }
        }
    }
};

} // namespace se
