#pragma once

// Review-context changed-path and line-range helpers.

static std::vector<std::string> collect_changed_paths(const Args& args) {
    std::vector<std::string> paths = args.positional;
    std::string csv = args.opt("changed_paths");
    if (!csv.empty()) {
        std::stringstream ss(csv);
        std::string path;
        while (std::getline(ss, path, ',')) {
            if (!path.empty()) paths.push_back(path);
        }
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
    std::string diff_text;
    std::string df = args.opt("diff_file");
    if (!df.empty()) {
        std::ifstream f(df, std::ios::binary);
        if (f) { std::stringstream b; b << f.rdbuf(); diff_text = b.str(); }
    }
    if (args.options.count("diff_stdin")) {
        std::stringstream b; b << std::cin.rdbuf(); diff_text += b.str();
    }
    if (!diff_text.empty()) ranges = parse_unified_diff_ranges(diff_text);

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
