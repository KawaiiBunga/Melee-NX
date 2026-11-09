// Minimal .zip reader for mod downloads. See zip_extract.h.
#include "data/zip_extract.h"

#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <filesystem>
#include <vector>

#include <zlib.h>

#include "data/fsutil.h"

namespace mkw::zipx {
namespace {

namespace fs = std::filesystem;

struct Entry {
    std::string name;
    uint16_t method = 0;
    uint32_t compressed = 0, size = 0, localOffset = 0;
};

uint32_t Le(const std::vector<uint8_t>& b, size_t o, int n) {
    uint32_t v = 0;
    for (int i = n - 1; i >= 0; --i) v = (v << 8) | (o + i < b.size() ? b[o + i] : 0);
    return v;
}

bool ReadAt(std::FILE* f, long off, size_t n, std::vector<uint8_t>& out) {
    out.resize(n);
    return std::fseek(f, off, SEEK_SET) == 0 && std::fread(out.data(), 1, n, f) == n;
}

bool SafeName(const std::string& n) {
    if (n.empty() || n[0] == '/' || n.find('\\') != std::string::npos || n.find(':') != std::string::npos) return false;
    size_t start = 0;
    while (start <= n.size()) {
        const size_t slash = n.find('/', start);
        const std::string part = n.substr(start, slash == std::string::npos ? std::string::npos : slash - start);
        if (part == "..") return false;
        if (slash == std::string::npos) break;
        start = slash + 1;
    }
    return true;
}

bool Inflate(const std::vector<uint8_t>& in, std::vector<uint8_t>& out) {
    z_stream z{};
    if (inflateInit2(&z, -MAX_WBITS) != Z_OK) return false;
    z.next_in = const_cast<Bytef*>(in.data());
    z.avail_in = static_cast<uInt>(in.size());
    z.next_out = out.data();
    z.avail_out = static_cast<uInt>(out.size());
    const int r = inflate(&z, Z_FINISH);
    inflateEnd(&z);
    return r == Z_STREAM_END && z.total_out == out.size();
}

}  // namespace

bool Extract(const std::string& zip, const std::string& dest, std::string& error) {
    std::error_code ec;
    if (fs::exists(dest, ec)) {
        error = "destination exists";
        return false;
    }
    std::FILE* f = std::fopen(zip.c_str(), "rb");
    if (!f) {
        error = "can't open";
        return false;
    }
    std::fseek(f, 0, SEEK_END);
    const long size = std::ftell(f);
    // The end-of-central-directory record is in the last 64 KiB + 22 bytes.
    const long tail = std::min<long>(size, 65557);
    std::vector<uint8_t> t;
    std::vector<Entry> entries;
    bool ok = tail >= 22 && ReadAt(f, size - tail, static_cast<size_t>(tail), t);
    long eocd = -1;
    for (long i = tail - 22; ok && i >= 0; --i)
        if (Le(t, i, 4) == 0x06054b50) {
            eocd = i;
            break;
        }
    if (eocd < 0) {
        error = "not a zip file";
        ok = false;
    }
    if (ok) {
        const uint32_t count = Le(t, eocd + 10, 2), cdSize = Le(t, eocd + 12, 4), cdOff = Le(t, eocd + 16, 4);
        std::vector<uint8_t> cd;
        if (count == 0xFFFF || cdOff == 0xFFFFFFFF || !ReadAt(f, cdOff, cdSize, cd)) {
            error = "unsupported zip (Zip64)";
            ok = false;
        }
        for (size_t o = 0, i = 0; ok && i < count; ++i) {
            if (Le(cd, o, 4) != 0x02014b50) {
                error = "damaged zip";
                ok = false;
                break;
            }
            Entry e;
            const uint16_t flags = static_cast<uint16_t>(Le(cd, o + 8, 2));
            e.method = static_cast<uint16_t>(Le(cd, o + 10, 2));
            e.compressed = Le(cd, o + 20, 4);
            e.size = Le(cd, o + 24, 4);
            const uint32_t nameLen = Le(cd, o + 28, 2), extraLen = Le(cd, o + 30, 2), commentLen = Le(cd, o + 32, 2);
            e.localOffset = Le(cd, o + 42, 4);
            if (o + 46 + nameLen > cd.size()) {
                error = "damaged zip";
                ok = false;
                break;
            }
            e.name.assign(reinterpret_cast<const char*>(&cd[o + 46]), nameLen);
            o += 46 + nameLen + extraLen + commentLen;
            if (flags & 1) {
                error = "encrypted zip";
                ok = false;
            } else if (e.size == 0xFFFFFFFF || e.compressed == 0xFFFFFFFF || e.localOffset == 0xFFFFFFFF) {
                error = "unsupported zip (Zip64)";
                ok = false;
            } else if (!SafeName(e.name)) {
                error = "unsafe path in zip: " + e.name;
                ok = false;
            } else if (e.name.back() != '/') {
                entries.push_back(e);
            }
        }
    }
    // One folder wrapping everything: unwrap it.
    std::string strip;
    if (ok && !entries.empty()) {
        const size_t slash = entries[0].name.find('/');
        if (slash != std::string::npos) {
            strip = entries[0].name.substr(0, slash + 1);
            for (const auto& e : entries)
                if (e.name.compare(0, strip.size(), strip) != 0) {
                    strip.clear();
                    break;
                }
        }
    }
    const std::string part = dest + ".part";
    fs::remove_all(part, ec);
    for (const auto& e : entries) {
        if (!ok) break;
        std::vector<uint8_t> local, data, out;
        if (!ReadAt(f, static_cast<long>(e.localOffset), 30, local) || Le(local, 0, 4) != 0x04034b50) {
            error = "damaged zip";
            ok = false;
            break;
        }
        const long dataOff = static_cast<long>(e.localOffset) + 30 + static_cast<long>(Le(local, 26, 2) + Le(local, 28, 2));
        if (!ReadAt(f, dataOff, e.compressed, data)) {
            error = "damaged zip";
            ok = false;
            break;
        }
        if (e.method == 0) {
            out = std::move(data);
        } else if (e.method == 8) {
            out.resize(e.size);
            if (!Inflate(data, out)) {
                error = "can't decompress " + e.name;
                ok = false;
                break;
            }
        } else {
            error = "unsupported compression in " + e.name + " (only Store and Deflate)";
            ok = false;
            break;
        }
        const std::string path = part + "/" + e.name.substr(strip.size());
        fsx::MakeDirs(fs::path(path).parent_path());
        std::FILE* o = std::fopen(path.c_str(), "wb");
        ok = o && std::fwrite(out.data(), 1, out.size(), o) == out.size();
        if (o) ok = std::fclose(o) == 0 && ok;
        if (!ok) error = "can't write " + path;
    }
    std::fclose(f);
    if (ok && entries.empty()) {
        error = "empty zip";
        ok = false;
    }
    if (ok) {
        fs::rename(part, dest, ec);
        ok = !ec;
        if (!ok) error = "can't rename the extracted folder";
    }
    if (!ok) fs::remove_all(part, ec);
    return ok;
}

}  // namespace mkw::zipx
