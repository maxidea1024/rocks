/* Rocks interface to libjpeg-turbo 3.2.1 (Docs/Design/AssetSystem section 12.11). libjpeg reports errors with
   longjmp; these functions keep setjmp and longjmp inside C so that no C++ frame is ever skipped, and they turn the
   outcome into a plain result code. Memory comes from the allocator bound to the current thread (jmemrocks.c). No
   message is printed. */
#ifndef ROCKS_JPEG_H
#define ROCKS_JPEG_H

#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Allocation functions the memory back end calls (jpeg_get_small and the like). */
typedef struct RocksJpegAllocator {
  void* (*allocate)(size_t bytes);
  void (*release)(void* address);
} RocksJpegAllocator;

/* Binds allocator to the current thread and returns the previous binding (null for none). */
const RocksJpegAllocator* RocksJpegBindAllocator(const RocksJpegAllocator* allocator);

/* Outcome of a call. Error is any libjpeg error exit (damaged or unsupported stream, failed allocation - the caller
   tells them apart), Warning is a recoverable-data warning (treated as damage), ScanLimit is more progressive scans
   than allowed. */
typedef enum RocksJpegResult {
  ROCKS_JPEG_OK = 0,
  ROCKS_JPEG_ERROR = 1,
  ROCKS_JPEG_WARNING = 2,
  ROCKS_JPEG_SCAN_LIMIT = 3
} RocksJpegResult;

/* Header facts read by jpeg_read_header (require_image TRUE). colorSpace is the libjpeg J_COLOR_SPACE value. */
typedef struct RocksJpegInfo {
  unsigned width;
  unsigned height;
  int components;
  int colorSpace;
  int precision;
  int lossless;
  int progressive;
} RocksJpegInfo;

/* Reads the header of data into info. */
RocksJpegResult RocksJpegReadInfo(const unsigned char* data, size_t size, RocksJpegInfo* info);

/* Decodes data into pixels (height rows of rowBytes): grayscale writes one byte per texel, otherwise four (R, G, B,
   255). maxScans limits progressive scans. The integer slow IDCT (JDCT_ISLOW) and the libjpeg default upsampling and
   block smoothing are used. */
RocksJpegResult RocksJpegDecode(const unsigned char* data, size_t size, int grayscale, unsigned maxScans,
                                unsigned char* pixels, size_t rowBytes);

/* Libjpeg colour space values the caller compares against. */
enum {
  ROCKS_JPEG_COLOR_GRAYSCALE = 1,
  ROCKS_JPEG_COLOR_RGB = 2,
  ROCKS_JPEG_COLOR_YCBCR = 3
};

#ifdef __cplusplus
}
#endif

#endif /* ROCKS_JPEG_H */
