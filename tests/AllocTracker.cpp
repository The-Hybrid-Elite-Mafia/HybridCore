// Global operator new/delete replacement that counts heap allocations while an
// AllocScope is active. Used to prove the audio path never allocates.
#include "TestHarness.hpp"

#include <cstdlib>
#include <new>

#if defined(_WIN32)
#include <malloc.h>
#endif

namespace {

void note() noexcept {
    if (hc::test::gTrackAllocs.load(std::memory_order_relaxed))
        hc::test::gAllocCount.fetch_add(1, std::memory_order_relaxed);
}

void* allocPlain(std::size_t n) {
    note();
    if (void* p = std::malloc(n ? n : 1)) return p;
    throw std::bad_alloc{};
}

void* allocAligned(std::size_t n, std::align_val_t al) {
    note();
    const auto a = static_cast<std::size_t>(al);
#if defined(_WIN32)
    if (void* p = ::_aligned_malloc(n ? n : 1, a)) return p;
#else
    const std::size_t rounded = ((n ? n : 1) + a - 1) / a * a;
    if (void* p = std::aligned_alloc(a, rounded)) return p;
#endif
    throw std::bad_alloc{};
}

void freeAligned(void* p) noexcept {
#if defined(_WIN32)
    ::_aligned_free(p);
#else
    std::free(p);
#endif
}

} // namespace

void* operator new(std::size_t n) { return allocPlain(n); }
void* operator new[](std::size_t n) { return allocPlain(n); }
void* operator new(std::size_t n, const std::nothrow_t&) noexcept {
    try { return allocPlain(n); } catch (...) { return nullptr; }
}
void* operator new[](std::size_t n, const std::nothrow_t&) noexcept {
    try { return allocPlain(n); } catch (...) { return nullptr; }
}
void* operator new(std::size_t n, std::align_val_t a) { return allocAligned(n, a); }
void* operator new[](std::size_t n, std::align_val_t a) { return allocAligned(n, a); }
void* operator new(std::size_t n, std::align_val_t a, const std::nothrow_t&) noexcept {
    try { return allocAligned(n, a); } catch (...) { return nullptr; }
}
void* operator new[](std::size_t n, std::align_val_t a, const std::nothrow_t&) noexcept {
    try { return allocAligned(n, a); } catch (...) { return nullptr; }
}

void operator delete(void* p) noexcept { std::free(p); }
void operator delete[](void* p) noexcept { std::free(p); }
void operator delete(void* p, std::size_t) noexcept { std::free(p); }
void operator delete[](void* p, std::size_t) noexcept { std::free(p); }
void operator delete(void* p, const std::nothrow_t&) noexcept { std::free(p); }
void operator delete[](void* p, const std::nothrow_t&) noexcept { std::free(p); }
void operator delete(void* p, std::align_val_t) noexcept { freeAligned(p); }
void operator delete[](void* p, std::align_val_t) noexcept { freeAligned(p); }
void operator delete(void* p, std::size_t, std::align_val_t) noexcept { freeAligned(p); }
void operator delete[](void* p, std::size_t, std::align_val_t) noexcept { freeAligned(p); }
void operator delete(void* p, std::align_val_t, const std::nothrow_t&) noexcept { freeAligned(p); }
void operator delete[](void* p, std::align_val_t, const std::nothrow_t&) noexcept { freeAligned(p); }
