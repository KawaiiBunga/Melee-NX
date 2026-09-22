#pragma once

#include <cstddef>
#include <cstdint>
#include <cstring>
#include <list>
#include <vector>
#include <utility>

namespace melee_nx {
// Caller serializes access. Full Dawn keys are checked even if hashes collide.
// The byte budget includes keys and decoded data; the entry cap bounds metadata.
class BlobReadCache {
    struct Entry {
        uint64_t low, high;
        std::vector<uint8_t> key, data;
    };
    std::list<Entry> entries_;
    size_t bytes_ = 0, budget_, limit_;
    uint64_t evictions_ = 0;

  public:
    explicit BlobReadCache(size_t budget = 16 * 1024 * 1024, size_t limit = 512)
        : budget_(budget), limit_(limit) {}
    bool accepts(size_t keySize, size_t dataSize) const {
        return limit_ && keySize <= budget_ && dataSize && dataSize <= budget_ - keySize;
    }
    const std::vector<uint8_t>* find(uint64_t low, uint64_t high, const void* key, size_t size) {
        for (auto it = entries_.begin(); it != entries_.end(); ++it) {
            if (it->low == low && it->high == high && it->key.size() == size &&
                (!size || std::memcmp(it->key.data(), key, size) == 0)) {
                entries_.splice(entries_.begin(), entries_, it);
                return &entries_.front().data;
            }
        }
        return nullptr;
    }
    // Persistence uses this hash as its key, so replacement invalidates all
    // matching hashes, including oversized replacements that cannot be cached.
    void invalidate(uint64_t low, uint64_t high) {
        for (auto it = entries_.begin(); it != entries_.end();) {
            if (it->low == low && it->high == high) {
                bytes_ -= it->key.size() + it->data.size();
                it = entries_.erase(it);
            } else
                ++it;
        }
    }
    template <class Fill>
    const std::vector<uint8_t>* fill(uint64_t low, uint64_t high, const void* key, size_t keySize,
                                     size_t dataSize, Fill&& decode) {
        invalidate(low, high);
        if (!accepts(keySize, dataSize))
            return nullptr;
        const size_t needed = keySize + dataSize;
        while (entries_.size() >= limit_ || bytes_ > budget_ - needed) {
            bytes_ -= entries_.back().key.size() + entries_.back().data.size();
            entries_.pop_back();
            ++evictions_;
        }
        Entry entry{low, high, std::vector<uint8_t>(keySize), std::vector<uint8_t>(dataSize)};
        if (keySize)
            std::memcpy(entry.key.data(), key, keySize);
        if (!decode(entry.data.data(), dataSize))
            return nullptr;
        entries_.push_front(std::move(entry));
        bytes_ += needed;
        return &entries_.front().data;
    }
    void clear() {
        entries_.clear();
        bytes_ = 0;
    }
    size_t bytes() const { return bytes_; }
    size_t size() const { return entries_.size(); }
    uint64_t evictions() const { return evictions_; }
};
} // namespace melee_nx
