#pragma once

#include <algorithm>
#include <cstddef>
#include <memory>
#include <new>
#include <type_traits>
#include <utility>
#include <vector>

namespace kd {

// Deferred native commands keep their captures alive until the command buffer
// is released, including across repeated submissions. Store captures in stable
// pages instead of allocating each non-trivial std::function separately.
class CommandArena {
  static constexpr size_t kAlignment = alignof(std::max_align_t);
  static constexpr size_t kPageBytes = 64 * 1024;
  static constexpr size_t aligned(size_t n) {
    return (n + kAlignment - 1) & ~(kAlignment - 1);
  }
  struct alignas(std::max_align_t) Entry {
    void (*invoke)(void*);
    void (*destroy)(void*) noexcept;
    size_t bytes;
  };
  struct Page {
    std::unique_ptr<std::byte[]> data;
    size_t used = 0, capacity;
    explicit Page(size_t n) : data(new std::byte[n]), capacity(n) {}
  };
  std::vector<Page> pages;
  size_t count = 0;

public:
  CommandArena() = default;
  CommandArena(const CommandArena&) = delete;
  CommandArena& operator=(const CommandArena&) = delete;
  ~CommandArena() {
    for (auto& page : pages)
      for (size_t off = 0; off < page.used;) {
        auto* entry = reinterpret_cast<Entry*>(page.data.get() + off);
        entry->destroy(entry + 1);
        off += entry->bytes;
      }
  }
  template <class F> void emplace_back(F&& callable) {
    using T = std::decay_t<F>;
    static_assert(alignof(T) <= kAlignment, "over-aligned command capture");
    static_assert(std::is_nothrow_destructible_v<T>);
    const size_t bytes = sizeof(Entry) + aligned(sizeof(T));
    if (pages.empty() || pages.back().capacity - pages.back().used < bytes)
      pages.emplace_back(std::max(kPageBytes, bytes));
    auto& page = pages.back();
    auto* entry = reinterpret_cast<Entry*>(page.data.get() + page.used);
    // Commit only after construction succeeds. A throwing capture leaves the
    // earlier commands intact and contributes no destructor or invocation.
    new (entry + 1) T(std::forward<F>(callable));
    new (entry) Entry{
        [](void* p) { (*static_cast<T*>(p))(); },
        [](void* p) noexcept { static_cast<T*>(p)->~T(); }, bytes};
    page.used += bytes;
    ++count;
  }
  void run() {
    for (auto& page : pages)
      for (size_t off = 0; off < page.used;) {
        auto* entry = reinterpret_cast<Entry*>(page.data.get() + off);
        entry->invoke(entry + 1);
        off += entry->bytes;
      }
  }
  size_t size() const { return count; }
};

} // namespace kd
