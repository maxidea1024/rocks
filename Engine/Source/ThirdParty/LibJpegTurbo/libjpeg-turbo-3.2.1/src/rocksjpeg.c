/* Rocks interface to libjpeg-turbo 3.2.1 (Docs/Design/AssetSystem section 12.11). See rocksjpeg.h. */
#define JPEG_INTERNALS
#include "jinclude.h"
#include "jpeglib.h"
#include "jpegint.h"
#include "rocksjpeg.h"

#include <setjmp.h>

typedef struct RocksJpegError {
  struct jpeg_error_mgr manager;
  jmp_buf jump;
  RocksJpegResult result;
} RocksJpegError;

typedef struct RocksJpegProgress {
  struct jpeg_progress_mgr manager;
  RocksJpegError* error;
  unsigned maxScans;
} RocksJpegProgress;

static void rocksJpegErrorExit(j_common_ptr cinfo)
{
  RocksJpegError* error = (RocksJpegError*)cinfo->err;
  error->result = ROCKS_JPEG_ERROR;
  longjmp(error->jump, 1);
}

/* Level -1 is a warning about damaged data that libjpeg recovered from: it stops the decode (UE TurboJPEG fails the
   same file). Trace messages (level >= 0) are ignored. */
static void rocksJpegEmitMessage(j_common_ptr cinfo, int msg_level)
{
  if (msg_level < 0) {
    RocksJpegError* error = (RocksJpegError*)cinfo->err;
    error->result = ROCKS_JPEG_WARNING;
    longjmp(error->jump, 1);
  }
}

static void rocksJpegOutputMessage(j_common_ptr cinfo)
{
  (void)cinfo;
}

/* Limits progressive scans like TurboJPEG TJPARAM_SCANLIMIT. */
static void rocksJpegProgressMonitor(j_common_ptr cinfo)
{
  RocksJpegProgress* progress = (RocksJpegProgress*)cinfo->progress;
  j_decompress_ptr dinfo = (j_decompress_ptr)cinfo;
  if (cinfo->is_decompressor && dinfo->input_scan_number > (int)progress->maxScans) {
    progress->error->result = ROCKS_JPEG_SCAN_LIMIT;
    longjmp(progress->error->jump, 1);
  }
}

static void rocksJpegInitError(RocksJpegError* error)
{
  jpeg_std_error(&error->manager);
  error->manager.error_exit = rocksJpegErrorExit;
  error->manager.emit_message = rocksJpegEmitMessage;
  error->manager.output_message = rocksJpegOutputMessage;
  error->result = ROCKS_JPEG_OK;
}

RocksJpegResult RocksJpegReadInfo(const unsigned char* data, size_t size, RocksJpegInfo* info)
{
  struct jpeg_decompress_struct cinfo;
  RocksJpegError error;
  rocksJpegInitError(&error);
  cinfo.err = &error.manager;
  if (setjmp(error.jump)) {
    jpeg_destroy_decompress(&cinfo);
    return error.result;
  }

  jpeg_create_decompress(&cinfo);
  jpeg_mem_src(&cinfo, data, (unsigned long)size);
  jpeg_read_header(&cinfo, TRUE);
  info->width = cinfo.image_width;
  info->height = cinfo.image_height;
  info->components = cinfo.num_components;
  info->colorSpace = (int)cinfo.jpeg_color_space;
  info->precision = cinfo.data_precision;
  info->lossless = cinfo.master->lossless ? 1 : 0;
  info->progressive = cinfo.progressive_mode ? 1 : 0;
  jpeg_destroy_decompress(&cinfo);
  return ROCKS_JPEG_OK;
}

RocksJpegResult RocksJpegDecode(const unsigned char* data, size_t size, int grayscale, unsigned maxScans,
                                unsigned char* pixels, size_t rowBytes)
{
  struct jpeg_decompress_struct cinfo;
  RocksJpegError error;
  RocksJpegProgress progress;
  JSAMPARRAY line;
  /* Locals changed after setjmp are not read on the longjmp path (cinfo lives in memory - its address is taken), so
     none needs volatile. The error path destroys the decompressor inside C. */
  rocksJpegInitError(&error);
  cinfo.err = &error.manager;
  if (setjmp(error.jump)) {
    jpeg_destroy_decompress(&cinfo);
    return error.result;
  }

  jpeg_create_decompress(&cinfo);
  progress.manager.progress_monitor = rocksJpegProgressMonitor;
  progress.error = &error;
  progress.maxScans = maxScans;
  cinfo.progress = &progress.manager;
  jpeg_mem_src(&cinfo, data, (unsigned long)size);
  jpeg_read_header(&cinfo, TRUE);
  cinfo.out_color_space = grayscale ? JCS_GRAYSCALE : JCS_RGB;
  cinfo.dct_method = JDCT_ISLOW;
  jpeg_start_decompress(&cinfo);
  line = (*cinfo.mem->alloc_sarray)((j_common_ptr)&cinfo, JPOOL_IMAGE,
                                    cinfo.output_width * (JDIMENSION)cinfo.output_components, 1);
  while (cinfo.output_scanline < cinfo.output_height) {
    unsigned char* destination = pixels + (size_t)cinfo.output_scanline * rowBytes;
    JDIMENSION column;
    jpeg_read_scanlines(&cinfo, line, 1);
    if (grayscale) {
      for (column = 0; column < cinfo.output_width; ++column)
        destination[column] = line[0][column];
    } else {
      for (column = 0; column < cinfo.output_width; ++column) {
        destination[column * 4 + 0] = line[0][column * 3 + 0];
        destination[column * 4 + 1] = line[0][column * 3 + 1];
        destination[column * 4 + 2] = line[0][column * 3 + 2];
        destination[column * 4 + 3] = 255;
      }
    }
  }

  jpeg_finish_decompress(&cinfo);
  jpeg_destroy_decompress(&cinfo);
  return ROCKS_JPEG_OK;
}
