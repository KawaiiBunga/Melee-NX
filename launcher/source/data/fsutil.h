// File helpers that work on Horizon's "sdmc:/" devoptab paths.
//
// libstdc++'s std::filesystem treats "sdmc:" as a relative first component, so
// create_directories tries to mkdir("sdmc:") and fails, and copy_file fails on the console.
// These use plain POSIX/stdio calls instead. Both return an error_code (empty on success).
#pragma once

#include <cerrno>
#include <cstdio>
#include <filesystem>
#include <string>
#include <system_error>
#include <vector>

#include <sys/stat.h>

namespace mkw::fsx {

// One directory level. Horizon and POSIX take a mode; the Windows host build does not.
inline int MkdirOne(const char* path) {
#ifdef _WIN32
    return ::mkdir(path);  // MinGW: sys/stat.h maps mkdir to the one-argument form
#else
    return ::mkdir(path, 0777);
#endif
}

// mkdir -p. The device prefix ("sdmc:") is never created.
inline std::error_code MakeDirs(const std::filesystem::path& dir) {
    const std::string p = dir.generic_string();
    const auto rootEnd = p.find(":/");
    size_t pos = rootEnd == std::string::npos ? 0 : rootEnd + 2;
    while (pos <= p.size()) {
        const auto slash = p.find('/', pos);
        const std::string part = p.substr(0, slash == std::string::npos ? p.size() : slash);
        if (!part.empty() && part.back() != ':' && MkdirOne(part.c_str()) != 0 && errno != EEXIST) {
            return {errno, std::generic_category()};
        }
        if (slash == std::string::npos) break;
        pos = slash + 1;
    }
    return {};
}

// Copies `src` over `dst` with stdio. `dst` is truncated first; a failed copy leaves it partial,
// so callers copy to a ".part" name and rename.
inline std::error_code CopyFile(const std::filesystem::path& src, const std::filesystem::path& dst) {
    std::FILE* in = std::fopen(src.generic_string().c_str(), "rb");
    if (!in) return {errno ? errno : EIO, std::generic_category()};
    std::FILE* out = std::fopen(dst.generic_string().c_str(), "wb");
    if (!out) {
        const int e = errno ? errno : EIO;
        std::fclose(in);
        return {e, std::generic_category()};
    }
    std::vector<char> buf(1 << 20);
    bool ok = true;
    for (size_t n; (n = std::fread(buf.data(), 1, buf.size(), in)) > 0;) {
        if (std::fwrite(buf.data(), 1, n, out) != n) {
            ok = false;
            break;
        }
    }
    ok = ok && !std::ferror(in);
    std::fclose(in);
    ok = (std::fclose(out) == 0) && ok;
    return ok ? std::error_code{} : std::error_code{EIO, std::generic_category()};
}

// Calls fn(path, relativePath) for every regular file under `dir` ("a/b.szs"). Builds the relative
// path itself: std::filesystem::relative fails on "sdmc:/" paths. An entry that errors is skipped
// instead of ending the walk.
template <class Fn>
void ForEachFile(const std::string& dir, Fn&& fn, const std::string& rel = std::string()) {
    std::error_code ec;
    for (std::filesystem::directory_iterator it(dir, ec), end; !ec && it != end; it.increment(ec)) {
        std::error_code entry;
        const std::string name = it->path().filename().string();
        const std::string r = rel.empty() ? name : rel + "/" + name;
        if (it->is_directory(entry))
            ForEachFile(dir + "/" + name, fn, r);
        else if (it->is_regular_file(entry))
            fn(dir + "/" + name, r);
    }
}

}  // namespace mkw::fsx
