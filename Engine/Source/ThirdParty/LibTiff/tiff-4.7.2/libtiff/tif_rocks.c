/* Rocks platform layer for libtiff 4.7.2 (Docs/Design/AssetSystem section 12.10), in place of tif_unix.c and tif_win32.c. See
   tif_rocks.h. The global message handlers stay null: callers receive messages through TIFFOpenOptionsSetErrorHandlerExtR and
   TIFFOpenOptionsSetWarningHandlerExtR, and nothing is printed. */
#include "tiffiop.h"

#include "tif_rocks.h"

#include <string.h>

static _Thread_local const RocksTiffAllocator* rocksTiffAllocator = NULL;

const RocksTiffAllocator* RocksTiffBindAllocator(const RocksTiffAllocator* allocator)
{
    const RocksTiffAllocator* previous = rocksTiffAllocator;
    rocksTiffAllocator = allocator;
    return previous;
}

void* _TIFFmalloc(tmsize_t s)
{
    /* Like tif_unix.c, a zero size returns null. */
    if (s <= 0 || rocksTiffAllocator == NULL)
        return NULL;

    return rocksTiffAllocator->allocate((size_t)s);
}

void* _TIFFcalloc(tmsize_t nmemb, tmsize_t siz)
{
    if (nmemb <= 0 || siz <= 0 || rocksTiffAllocator == NULL)
        return NULL;

    if (nmemb > TIFF_TMSIZE_T_MAX / siz)
        return NULL;

    void* p = rocksTiffAllocator->allocate((size_t)(nmemb * siz));
    if (p != NULL)
        memset(p, 0, (size_t)(nmemb * siz));

    return p;
}

void _TIFFfree(void* p)
{
    if (p != NULL && rocksTiffAllocator != NULL)
        rocksTiffAllocator->release(p);
}

void* _TIFFrealloc(void* p, tmsize_t s)
{
    if (s < 0 || rocksTiffAllocator == NULL)
        return NULL;

    return rocksTiffAllocator->reallocate(p, (size_t)s);
}

void _TIFFmemset(void* p, int v, tmsize_t c) { memset(p, v, (size_t)c); }

void _TIFFmemcpy(void* d, const void* s, tmsize_t c) { memcpy(d, s, (size_t)c); }

int _TIFFmemcmp(const void* p1, const void* p2, tmsize_t c) { return memcmp(p1, p2, (size_t)c); }

TIFFErrorHandler _TIFFwarningHandler = NULL;

TIFFErrorHandler _TIFFerrorHandler = NULL;
