#pragma once
// Unpacks the respack-embedded files into `dir`, once per build: a `.respack-stamp` file holds the content hash
// of what was last extracted, and a matching stamp skips the whole thing (no rewrite of every file on each boot).
// Each file goes to a temp name and is renamed into place (atomic: a crash never leaves a half-written file), and
// gets a fixed mtime so the static-file ETags (mtime+size) stay the same across launches and updates.
#include "resources.hpp"

#include <cstdio>
#include <filesystem>
#include <fstream>
#include <string>
#include <fcntl.h>
#include <sys/stat.h>

inline void extract_resources(const std::filesystem::path& dir) {
    namespace fs = std::filesystem;
    const fs::path stamp = dir / ".respack-stamp";
    {
        std::ifstream in(stamp);
        std::string cur;
        if (in >> cur && cur == kEmbeddedStamp) return;
    }
    const timespec fixed[2] = {{1577836800, 0}, {1577836800, 0}};   // 2020-01-01 UTC
    for (const auto& f : kEmbeddedFiles) {
        fs::path dest = dir / f.path;
        fs::create_directories(dest.parent_path());
        fs::path tmp = dest;
        tmp += ".respack-tmp";
        {
            std::ofstream out(tmp, std::ios::binary | std::ios::trunc);
            out.write(reinterpret_cast<const char*>(f.data), static_cast<std::streamsize>(f.size));
        }
        ::utimensat(AT_FDCWD, tmp.c_str(), fixed, 0);
        std::error_code ec;
        fs::rename(tmp, dest, ec);
    }
    std::ofstream(stamp, std::ios::trunc) << kEmbeddedStamp << "\n";   // last: an interrupted run redoes everything
}
