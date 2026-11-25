/* Rocks platform layer for libtiff 4.7.2 (Docs/Design/AssetSystem section 12.10), in place of tif_unix.c and tif_win32.c. See
   tif_rocks.h. The global message handlers stay null: callers receive messages through TIFFOpenOptionsSetErrorHandlerExtR and
   TIFFOpenOptionsSetWarningHandlerExtR, and nothing is printed. */
#include "tiffiop.h"

#include "tif_rocks.h"

#include <string.h>

static _Thread_local const RocksTiffAllocator* rocksTiffAllocator = NULL;

/* Every block starts with a header that records the allocator table that made it, so _TIFFfree and
   _TIFFrealloc work even when the binding has changed (or is gone) by the time the block is
   released. The header size keeps the user area 16-byte aligned. */
#define ROCKS_TIFF_HEADER_BYTES 16

typedef struct RocksTiffBlock {
    const RocksTiffAllocator* allocator;
} RocksTiffBlock;

static void* rocksTiffUserArea(void* block)
{
    return (unsigned char*)block + ROCKS_TIFF_HEADER_BYTES;
}

static RocksTiffBlock* rocksTiffHeader(void* p)
{
    return (RocksTiffBlock*)((unsigned char*)p - ROCKS_TIFF_HEADER_BYTES);
}

static void* rocksTiffAllocate(size_t bytes)
{
    if (rocksTiffAllocator == NULL || bytes > (size_t)TIFF_TMSIZE_T_MAX - ROCKS_TIFF_HEADER_BYTES)
        return NULL;

    RocksTiffBlock* block = (RocksTiffBlock*)rocksTiffAllocator->allocate(bytes + ROCKS_TIFF_HEADER_BYTES);
    if (block == NULL)
        return NULL;

    block->allocator = rocksTiffAllocator;
    return rocksTiffUserArea(block);
}

const RocksTiffAllocator* RocksTiffBindAllocator(const RocksTiffAllocator* allocator)
{
    const RocksTiffAllocator* previous = rocksTiffAllocator;
    rocksTiffAllocator = allocator;
    return previous;
}

void* _TIFFmalloc(tmsize_t s)
{
    /* Like tif_unix.c, a zero size returns null. */
    if (s <= 0)
        return NULL;

    return rocksTiffAllocate((size_t)s);
}

void* _TIFFcalloc(tmsize_t nmemb, tmsize_t siz)
{
    if (nmemb <= 0 || siz <= 0)
        return NULL;

    if (nmemb > TIFF_TMSIZE_T_MAX / siz)
        return NULL;

    void* p = rocksTiffAllocate((size_t)(nmemb * siz));
    if (p != NULL)
        memset(p, 0, (size_t)(nmemb * siz));

    return p;
}

void _TIFFfree(void* p)
{
    if (p == NULL)
        return;

    RocksTiffBlock* block = rocksTiffHeader(p);
    block->allocator->release(block);
}

void* _TIFFrealloc(void* p, tmsize_t s)
{
    if (s < 0 || (size_t)s > (size_t)TIFF_TMSIZE_T_MAX - ROCKS_TIFF_HEADER_BYTES)
        return NULL;

    if (p == NULL)
        return rocksTiffAllocate((size_t)s);

    /* The block keeps its allocator: reallocate through the table that made it. */
    RocksTiffBlock* block = rocksTiffHeader(p);
    const RocksTiffAllocator* allocator = block->allocator;
    RocksTiffBlock* resized =
        (RocksTiffBlock*)allocator->reallocate(block, (size_t)s + ROCKS_TIFF_HEADER_BYTES);
    if (resized == NULL)
        return NULL;

    resized->allocator = allocator;
    return rocksTiffUserArea(resized);
}

void _TIFFmemset(void* p, int v, tmsize_t c) { memset(p, v, (size_t)c); }

void _TIFFmemcpy(void* d, const void* s, tmsize_t c) { memcpy(d, s, (size_t)c); }

int _TIFFmemcmp(const void* p1, const void* p2, tmsize_t c) { return memcmp(p1, p2, (size_t)c); }

TIFFErrorHandler _TIFFwarningHandler = NULL;

TIFFErrorHandler _TIFFerrorHandler = NULL;
