/* Rocks platform layer for libtiff 4.7.2 (Docs/Design/AssetSystem section 12.10). tif_rocks.c replaces the upstream platform
   files tif_unix.c and tif_win32.c: it defines the memory entry points, the global message handlers (none) and no file open
   functions, so the only way in is TIFFClientOpenExt with caller procedures. Allocation goes to the allocator the caller binds
   to the current thread; without a binding every allocation fails (no CRT fallback). The binding must cover every libtiff call
   from TIFFClientOpenExt to TIFFClose on that thread, because _TIFFfree also uses it. */
#ifndef TIF_ROCKS_H
#define TIF_ROCKS_H

#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Allocation functions libtiff calls through _TIFFmalloc, _TIFFcalloc, _TIFFrealloc and _TIFFfree. reallocate follows C realloc
   (a null address allocates, failure keeps the old block). */
typedef struct RocksTiffAllocator {
    void* (*allocate)(size_t bytes);
    void* (*reallocate)(void* address, size_t bytes);
    void (*release)(void* address);
} RocksTiffAllocator;

/* Binds allocator to the current thread and returns the previous binding (null for none). Pass the previous binding back to
   restore it. */
const RocksTiffAllocator* RocksTiffBindAllocator(const RocksTiffAllocator* allocator);

#ifdef __cplusplus
}
#endif

#endif /* TIF_ROCKS_H */
