// Rocks glue that builds lz4.c and lz4hc.c of LZ4 1.11.1 as one translation unit. This is an
// approach upstream supports. When lz4.c defines LZ4_SRC_INCLUDED, lz4hc.c does not include lz4.c
// again, so the -Wunused-function suppression branch used only when lz4hc.c is built alone is
// inactive. Build definitions (LZ4_STATIC_LINKING_ONLY_DISABLE_MEMORY_ALLOCATION and others) follow
// ThirdParty.json. lz4frame.c is not part of this amalgamation (it reuses lz4.c's allocation
// macros, which the definitions above block; see the manifest notes).
#include "lz4.c"
#include "lz4hc.c"

// Functions called by lz4frame.c's default allocation path (when there is no user allocator)
// (LZ4_USER_MEMORY_FUNCTIONS, an upstream configuration point). Rocks creates every LZ4F context
// with Allocator-backed allocation functions, so this path is never used. A call that accidentally
// takes the default allocation path (LZ4F_compressFrame, the non-_advanced creation functions) gets
// an allocation failure (LZ4F_ERROR_allocation_failed) instead of the CRT. lz4.c does not use these
// functions because its DISABLE_MEMORY_ALLOCATION branch comes first.
void* LZ4_malloc(size_t size);
void* LZ4_calloc(size_t count, size_t size);
void LZ4_free(void* address);

void* LZ4_malloc(size_t size) {
    (void)size;
    return NULL;
}

void* LZ4_calloc(size_t count, size_t size) {
    (void)count;
    (void)size;
    return NULL;
}

void LZ4_free(void* address) {
    // LZ4F_free returns immediately for NULL, and the two functions above never return an address,
    // so no address ever reaches this point.
    (void)address;
}
