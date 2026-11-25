/* Rocks memory manager back end for libjpeg-turbo 3.2.1 (Docs/Design/AssetSystem section 12.11), in place of
   jmemnobs.c. Allocation goes to the allocator the caller binds to the current thread (RocksJpegBindAllocator in
   rocksjpeg.h); without a binding every allocation fails (no CRT fallback). Each block records the allocator table
   that made it, so freeing does not depend on the binding. client_data is not used because libtiff's old JPEG codec
   stores its own pointer there. Like jmemnobs.c, jpeg_mem_available reports everything requested as available (the
   image size is already limited by the caller) and there is no backing store. */
#define JPEG_INTERNALS
#include "jinclude.h"
#include "jpeglib.h"
#include "jmemsys.h"
#include "rocksjpeg.h"

#include <stdint.h>

static _Thread_local const RocksJpegAllocator* rocksJpegAllocator = NULL;

/* The header keeps the user area 16-byte aligned. */
#define ROCKS_JPEG_HEADER_BYTES 16

typedef struct RocksJpegBlock {
  const RocksJpegAllocator* allocator;
} RocksJpegBlock;

const RocksJpegAllocator* RocksJpegBindAllocator(const RocksJpegAllocator* allocator)
{
  const RocksJpegAllocator* previous = rocksJpegAllocator;
  rocksJpegAllocator = allocator;
  return previous;
}

static void* rocksJpegAllocate(size_t bytes)
{
  if (rocksJpegAllocator == NULL || bytes > SIZE_MAX - ROCKS_JPEG_HEADER_BYTES)
    return NULL;

  RocksJpegBlock* block = (RocksJpegBlock*)rocksJpegAllocator->allocate(bytes + ROCKS_JPEG_HEADER_BYTES);
  if (block == NULL)
    return NULL;

  block->allocator = rocksJpegAllocator;
  return (unsigned char*)block + ROCKS_JPEG_HEADER_BYTES;
}

static void rocksJpegRelease(void* object)
{
  if (object == NULL)
    return;

  RocksJpegBlock* block = (RocksJpegBlock*)((unsigned char*)object - ROCKS_JPEG_HEADER_BYTES);
  block->allocator->release(block);
}

GLOBAL(void*)
jpeg_get_small(j_common_ptr cinfo, size_t sizeofobject)
{
  (void)cinfo;
  return rocksJpegAllocate(sizeofobject);
}

GLOBAL(void)
jpeg_free_small(j_common_ptr cinfo, void* object, size_t sizeofobject)
{
  (void)cinfo;
  (void)sizeofobject;
  rocksJpegRelease(object);
}

GLOBAL(void*)
jpeg_get_large(j_common_ptr cinfo, size_t sizeofobject)
{
  (void)cinfo;
  return rocksJpegAllocate(sizeofobject);
}

GLOBAL(void)
jpeg_free_large(j_common_ptr cinfo, void* object, size_t sizeofobject)
{
  (void)cinfo;
  (void)sizeofobject;
  rocksJpegRelease(object);
}

GLOBAL(size_t)
jpeg_mem_available(j_common_ptr cinfo, size_t min_bytes_needed, size_t max_bytes_needed,
                   size_t already_allocated)
{
  (void)cinfo;
  (void)min_bytes_needed;
  (void)already_allocated;
  return max_bytes_needed;
}

GLOBAL(void)
jpeg_open_backing_store(j_common_ptr cinfo, backing_store_ptr info, long total_bytes_needed)
{
  (void)info;
  (void)total_bytes_needed;
  ERREXIT(cinfo, JERR_NO_BACKING_STORE);
}

GLOBAL(long)
jpeg_mem_init(j_common_ptr cinfo)
{
  (void)cinfo;
  return 0;
}

GLOBAL(void)
jpeg_mem_term(j_common_ptr cinfo)
{
  (void)cinfo;
}
