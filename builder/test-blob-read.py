#!/usr/bin/env python3
"""Compile the actual Switch blob callbacks against host SQLite and zstd.

Run in kartpad-dawn from /project. Extracts only the callbacks to avoid linking
Dawn/GPU code; their database, pending writes and decoded cache are exercised.
"""
from pathlib import Path
import subprocess
import tempfile

root = Path(__file__).resolve().parents[1]
source = (root / "ref/melee-pc/extern/aurora/lib/webgpu/gpu_cache.cpp").read_text()
start = source.index("static uint64_t s_blobHits", source.index("#if defined(__SWITCH__)\nstatic uint64_t"))
end = source.index("\n#else\n// BLOBCACHE diagnostics", start)
callbacks = source[start:end]
start = source.index("void store_to_cache(")
store = source[start:source.index("\nvoid cache_prune()", start)]
preamble = r'''
#include <cassert>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <mutex>
#include <string>
#include <vector>
#include <sqlite3.h>
#include <zstd.h>
#define XXH_INLINE_ALL
#include <xxhash.h>
#include "blob_read_cache.hpp"
struct Logger {
  template<class... T> void error(const char*, T&&...) {}
  template<class... T> void info(const char*, T&&...) {}
} Log;
sqlite3* db;
sqlite3_stmt* load_stmt;
std::mutex cache_mutex;
melee_nx::BlobReadCache read_cache(4096, 8);
std::vector<XXH128_hash_t> cache_keys_used;
struct PendingCacheWrite {
  XXH128_hash_t key;
  std::vector<uint8_t> stored;
  uint64_t originalSize = 0;
  int compressed = 0;
};
std::vector<PendingCacheWrite> pending_writes;
std::vector<uint8_t> compress_buffer;
size_t pending_write_bytes = 0;
constexpr size_t MaxPendingCacheWrites = 64, MaxPendingCacheBytes = 4*1024*1024;
bool cache_init() { return true; }
int check(int result) { assert(result == SQLITE_OK); return result; }
bool flush_pending_writes_locked() { assert(false && "test unexpectedly filled write batch"); return false; }
const PendingCacheWrite* find_pending_write(const XXH128_hash_t& key) {
  for (auto it = pending_writes.rbegin(); it != pending_writes.rend(); ++it)
    if (it->key.low64 == key.low64 && it->key.high64 == key.high64) return &*it;
  return nullptr;
}
'''
tests = r'''
void row(const std::string& key, const std::string& data, bool compress, int declared = -1) {
  auto hash = XXH128(key.data(), key.size(), 0);
  std::vector<char> stored(data.begin(), data.end());
  if (compress) {
    stored.resize(ZSTD_compressBound(data.size()));
    const auto count = ZSTD_compress(stored.data(), stored.size(), data.data(), data.size(), 1);
    assert(!ZSTD_isError(count)); stored.resize(count);
  }
  sqlite3_stmt* stmt;
  check(sqlite3_prepare_v2(db, "REPLACE INTO cache VALUES(?,?,?,?)", -1, &stmt, nullptr));
  check(sqlite3_bind_blob(stmt, 1, &hash, sizeof(hash), SQLITE_TRANSIENT));
  check(sqlite3_bind_blob(stmt, 2, stored.data(), stored.size(), SQLITE_TRANSIENT));
  check(sqlite3_bind_int(stmt, 3, declared < 0 ? data.size() : declared));
  check(sqlite3_bind_int(stmt, 4, compress));
  assert(sqlite3_step(stmt) == SQLITE_DONE);
  sqlite3_finalize(stmt);
}
size_t size(const std::string& key) { return load_from_cache(key.data(), key.size(), nullptr, 0, nullptr); }
std::string get(const std::string& key, size_t n) {
  std::string out(n, '?');
  assert(load_from_cache(key.data(), key.size(), out.data(), n, nullptr) == n);
  return out;
}
int main() {
  check(sqlite3_open(":memory:", &db));
  check(sqlite3_exec(db, "CREATE TABLE cache(key BLOB PRIMARY KEY,value BLOB,size INTEGER,compressed INTEGER)", nullptr,nullptr,nullptr));
  check(sqlite3_prepare_v2(db, "SELECT value,size,compressed FROM cache WHERE key=?", -1, &load_stmt,nullptr));
  for (bool compressed : {false,true}) {
    read_cache.clear();
    const std::string data(1000, compressed ? 'z' : 'r');
    row("key",data,compressed);
    const auto reads = s_blobSqlReads;
    assert(size("key") == data.size());
    assert(get("key",data.size()) == data);
    assert(s_blobSqlReads == reads+1); // Size and copy used one SELECT.
    char wrong[7]; std::memset(wrong,'?',sizeof(wrong));
    assert(load_from_cache("key",3,wrong,sizeof(wrong),nullptr) == data.size());
    assert(std::string(wrong,sizeof(wrong)) == "???????");
  }
  assert(size("missing") == 0);
  read_cache.clear();
  row("badraw","tiny",false,100);
  row("badzstd","tiny",true,100);
  assert(size("badraw") == 0 && size("badzstd") == 0);
  const std::string large(8192,'L');
  row("large",large,true);
  const auto reads = s_blobSqlReads;
  assert(size("large") == large.size());
  assert(get("large",large.size()) == large);
  assert(s_blobSqlReads == reads+2); // Oversized values use the bounded fallback.
  assert(read_cache.bytes() <= 4096);
  row("replace","old",false);
  assert(size("replace") == 3);
  const std::string replacement(800,'n');
  store_to_cache("replace",7,replacement.data(),replacement.size(),nullptr);
  assert(size("replace") == replacement.size());
  assert(get("replace",replacement.size()) == replacement);
  store_to_cache("replace",7,large.data(),large.size(),nullptr);
  assert(size("replace") == large.size());
  assert(get("replace",large.size()) == large);
  sqlite3_finalize(load_stmt); sqlite3_close(db);
  std::puts("Actual blob callbacks: SQLite raw/zstd size-copy, malformed data, oversized fallback and pending replacement passed");
}
'''
with tempfile.TemporaryDirectory(prefix="melee-blob-test-") as tmp:
    cpp = Path(tmp) / "test.cpp"
    cpp.write_text(preamble + callbacks + store + tests)
    exe = Path(tmp) / "test"
    command = ["clang++-19", "-std=c++20", "-O1", "-g", "-fsanitize=address,undefined",
               "-D__SWITCH__", "-DAURORA_CACHE_USE_ZSTD", "-I" + str(root / "switch/src"),
               "-I" + str(root / "build/switch/_deps/sqlite3-src"),
               "-I" + str(root / "build/switch/_deps/xxhash-src"),
               "-idirafter", "/opt/devkitpro/portlibs/switch/include", str(cpp),
               "-l:libsqlite3.so.0", "-l:libzstd.so.1", "-o", str(exe)]
    subprocess.run(command, check=True)
    subprocess.run([str(exe)], check=True)
