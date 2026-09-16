// SPDX-License-Identifier: GPL-3.0-only

#include "canvas.h"

#include <cstdlib>
#include <cstring>

namespace nb {

namespace {
void* default_alloc(size_t n) { return std::malloc(n); }
void default_free(void* p) { std::free(p); }

AllocFn g_alloc = &default_alloc;
FreeFn g_free = &default_free;
}  // namespace

void canvas_set_allocator(AllocFn alloc, FreeFn free_fn) {
    g_alloc = alloc ? alloc : &default_alloc;
    g_free = free_fn ? free_fn : &default_free;
}

Canvas16 canvas_alloc(uint16_t w, uint16_t h) {
    Canvas16 c;
    const size_t n = static_cast<size_t>(w) * h;
    if (w == 0 || h == 0 || n == 0) return c;
    c.px = static_cast<uint16_t*>(g_alloc(n * sizeof(uint16_t)));
    if (!c.px) return c;
    std::memset(c.px, 0, n * sizeof(uint16_t));
    c.w = w;
    c.h = h;
    return c;
}

void canvas_free(Canvas16& c) {
    if (c.px) g_free(c.px);
    c.px = nullptr;
    c.w = c.h = 0;
}

}  // namespace nb
