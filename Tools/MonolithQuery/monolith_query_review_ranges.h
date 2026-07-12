#pragma once

// Review-context changed-path and line-range helpers.

static std::vector<std::string> collect_changed_paths(const Args& args) {
    std::vector<std::string> paths = args.positional;
    for (const std::string& key : {std::string("changed_paths"), std::string("paths")}) {
        std::vector<std::string> values = string_list_option(args, key);
        paths.insert(paths.end(), values.begin(), values.end());
    }
    return paths;
}

// RX-1.1: unified-diff parsing (port of code_review_graph/changes.py
// _parse_unified_diff). Pure text; no VCS shell-out — the caller produces
// the diff (parent RX-1 contract; CRG incremental.py is the caller's job).
static std::map<std::string, std::vector<std::pair<int,int>>>
parse_unified_diff_ranges(const std::string& diff_text) {
    std::map<std::string, std::vector<std::pair<int,int>>> ranges;
    std::stringstream ss(diff_text);
    std::string line, current;
    bool have_file = false;
    bool prev_minus_header = false;
    while (std::getline(ss, line)) {
        if (!line.empty() && line.back() == '\r') line.pop_back();
        // Accept git-style ("+++ b/path") and plain ("+++ path") unified-diff
        // new-file headers so non-git producers (e.g. `p4 -du`) keep RX-1.1
        // line precision instead of dropping every hunk range. A bare "+++ "
        // can also be added file content, so a plain header is only honored
        // when the previous line was the matching "--- " header.
        const bool git_new = line.rfind("+++ b/", 0) == 0;
        const bool plain_new = !git_new && line.rfind("+++ ", 0) == 0 && prev_minus_header;
        if (git_new || plain_new) {
            current = line.substr(git_new ? 6 : 4);
            size_t tab = current.find('\t');
            if (tab != std::string::npos) current = current.substr(0, tab);
            while (!current.empty() && current.front() == ' ') current.erase(current.begin());
            while (!current.empty() && (current.back() == ' ' || current.back() == '\r')) current.pop_back();
            if (current.rfind("b/", 0) == 0) current = current.substr(2);
            std::replace(current.begin(), current.end(), '\\', '/');
            if (current == "/dev/null") { current.clear(); have_file = false; prev_minus_header = false; continue; }
            have_file = !current.empty();
            prev_minus_header = false;
            continue;
        }
        prev_minus_header = line.rfind("--- ", 0) == 0;
        if (!have_file || line.rfind("@@ ", 0) != 0) continue;
        size_t plus = line.find('+');
        if (plus == std::string::npos || plus + 1 >= line.size()) continue;
        size_t i = plus + 1;
        auto read_int = [&]() -> int {
            int v = 0; bool any = false;
            while (i < line.size() && line[i] >= '0' && line[i] <= '9') {
                v = v * 10 + (line[i] - '0'); ++i; any = true;
            }
            return any ? v : -1;
        };
        int start = read_int();
        if (start <= 0) continue;
        int count = 1;
        if (i < line.size() && line[i] == ',') {
            ++i;
            int p = read_int();
            count = (p >= 0) ? p : 1;
        }
        int end = (count == 0) ? start : (start + count - 1);
        ranges[current].push_back({start, end});
    }
    return ranges;
}

// VCS-agnostic line ranges from --diff-file=PATH, --diff-stdin, and/or
// --ranges=path:start-end[,path2:s-e]. Read-only, no shell-out.
static std::map<std::string, std::vector<std::pair<int,int>>>
collect_changed_ranges(const Args& args) {
    std::map<std::string, std::vector<std::pair<int,int>>> ranges;
    std::string diff_text = args.opt("diff_text");
    std::string df = args.opt("diff_file");
    if (!df.empty()) {
        std::ifstream f(df, std::ios::binary);
        if (f) { std::stringstream b; b << f.rdbuf(); diff_text = b.str(); }
    }
    if (args.opt_bool("diff_stdin", false)) {
        std::stringstream b; b << std::cin.rdbuf(); diff_text += b.str();
    }
    if (!diff_text.empty()) ranges = parse_unified_diff_ranges(diff_text);

    if (args.options.count("changed_ranges"))
        args.require_mcp_type("changed_ranges", {"array"}, "an array");
    std::string changed_ranges_json = args.raw_opt("changed_ranges");
    if (!changed_ranges_json.empty()) {
        try {
            json entries = json::parse(changed_ranges_json);
            if (!entries.is_array())
                die("--changed-ranges must be a JSON array");
            for (const json& entry : entries) {
                if (!entry.is_object())
                    die("--changed-ranges entries must be objects");
                std::string path = entry.value("path", "");
                std::replace(path.begin(), path.end(), '\\', '/');
                if (path.empty() || !entry.contains("ranges") || !entry["ranges"].is_array())
                    die("--changed-ranges entries require path and ranges");
                for (const json& span : entry["ranges"]) {
                    if (!span.is_array() || span.size() != 2
                        || !span[0].is_number_integer() || !span[1].is_number_integer())
                        die("--changed-ranges spans must be [start,end] integer pairs");
                    int start = span[0].get<int>();
                    int end = span[1].get<int>();
                    if (start <= 0 || end < start)
                        die("--changed-ranges spans require start > 0 and end >= start");
                    ranges[path].push_back({start, end});
                }
            }
        } catch (const QueryFatal&) {
            throw;
        } catch (const std::exception&) {
            die("--changed-ranges must be valid JSON: [{path,ranges:[[start,end]]}]");
        }
    }

    std::string spec = args.opt("ranges");
    if (!spec.empty()) {
        std::stringstream ss(spec);
        std::string token;
        while (std::getline(ss, token, ',')) {
            size_t colon = token.rfind(':');
            if (colon == std::string::npos) continue;
            std::string path = token.substr(0, colon);
            std::string span = token.substr(colon + 1);
            size_t dash = span.find('-');
            if (dash == std::string::npos) continue;
            try {
                int s = std::stoi(span.substr(0, dash));
                int e = std::stoi(span.substr(dash + 1));
                std::replace(path.begin(), path.end(), '\\', '/');
                if (s > 0 && e >= s && !path.empty()) ranges[path].push_back({s, e});
            } catch (...) {}
        }
    }
    return ranges;
}

// ============================================================
// Path utilities
// ============================================================

static std::string short_path(const std::string& full_path) {
    static const char* markers[] = {
        "Engine\\Source\\", "Engine/Source/",
        "Engine\\Shaders\\", "Engine/Shaders/"
    };
    for (auto m : markers) {
        auto idx = full_path.find(m);
        if (idx != std::string::npos)
            return full_path.substr(idx);
    }
    return full_path;
}
