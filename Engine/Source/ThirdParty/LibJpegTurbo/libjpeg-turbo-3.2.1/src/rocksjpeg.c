/* Rocks interface to libjpeg-turbo 3.2.1 (Docs/Design/AssetSystem section 12.11). See rocksjpeg.h. */
#define JPEG_INTERNALS
#include "jinclude.h"
#include "jpeglib.h"
#include "rocksjpeg.h"

#include <limits.h>
#include <setjmp.h>
#include <string.h>

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

/* Everything libjpeg touches between setjmp and longjmp. C17 7.13.2.1 leaves a non-volatile automatic object of the
   function that called setjmp indeterminate after longjmp when it changed in between, so the public functions own this
   state and the functions that call setjmp reach it only through a pointer (like the TurboJPEG handle). With a local
   cinfo, Release ThinLTO removed the error branch of RocksJpegReadInfo: longjmp re-entered the normal path and looped
   (create, error, longjmp) while leaking every memory pool. */
typedef struct RocksJpegState {
  struct jpeg_decompress_struct cinfo;
  RocksJpegError error;
  RocksJpegProgress progress;
} RocksJpegState;

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

/* Zeroes state (a null cinfo.mem keeps jpeg_destroy_decompress safe on every path) and installs the error manager. */
static void rocksJpegInitState(RocksJpegState* state)
{
  memset(state, 0, sizeof(*state));
  jpeg_std_error(&state->error.manager);
  state->error.manager.error_exit = rocksJpegErrorExit;
  state->error.manager.emit_message = rocksJpegEmitMessage;
  state->error.manager.output_message = rocksJpegOutputMessage;
  state->error.result = ROCKS_JPEG_OK;
  state->cinfo.err = &state->error.manager;
}

/* No local here changes after setjmp; all libjpeg state lives in *state. */
static RocksJpegResult rocksJpegReadInfoWith(RocksJpegState* state, const unsigned char* data, size_t size,
                                             RocksJpegInfo* info)
{
  j_decompress_ptr cinfo = &state->cinfo;
  if (setjmp(state->error.jump)) {
    jpeg_destroy_decompress(cinfo);
    return state->error.result;
  }

  jpeg_create_decompress(cinfo);
  jpeg_mem_src(cinfo, data, (unsigned long)size);
  jpeg_read_header(cinfo, TRUE);
  info->width = cinfo->image_width;
  info->height = cinfo->image_height;
  info->components = cinfo->num_components;
  info->colorSpace = (int)cinfo->jpeg_color_space;
  info->precision = cinfo->data_precision;
  info->lossless = cinfo->master->lossless ? 1 : 0;
  info->progressive = cinfo->progressive_mode ? 1 : 0;
  jpeg_destroy_decompress(cinfo);
  return ROCKS_JPEG_OK;
}

RocksJpegResult RocksJpegReadInfo(const unsigned char* data, size_t size, RocksJpegInfo* info)
{
  RocksJpegState state;
  /* jpeg_mem_src takes an unsigned long (32 bits on Windows). */
  if (size > ULONG_MAX)
    return ROCKS_JPEG_ERROR;

  rocksJpegInitState(&state);
  return rocksJpegReadInfoWith(&state, data, size, info);
}

/* line, destination and column change after setjmp but the longjmp path does not read them; cinfo does not change. The
   error path destroys the decompressor inside C. */
static RocksJpegResult rocksJpegDecodeWith(RocksJpegState* state, const unsigned char* data, size_t size,
                                           int grayscale, unsigned maxScans, unsigned char* pixels, size_t rowBytes)
{
  j_decompress_ptr cinfo = &state->cinfo;
  JSAMPARRAY line;
  if (setjmp(state->error.jump)) {
    jpeg_destroy_decompress(cinfo);
    return state->error.result;
  }

  jpeg_create_decompress(cinfo);
  state->progress.manager.progress_monitor = rocksJpegProgressMonitor;
  state->progress.error = &state->error;
  state->progress.maxScans = maxScans;
  cinfo->progress = &state->progress.manager;
  jpeg_mem_src(cinfo, data, (unsigned long)size);
  jpeg_read_header(cinfo, TRUE);
  cinfo->out_color_space = grayscale ? JCS_GRAYSCALE : JCS_RGB;
  cinfo->dct_method = JDCT_ISLOW;
  jpeg_start_decompress(cinfo);
  line = (*cinfo->mem->alloc_sarray)((j_common_ptr)cinfo, JPOOL_IMAGE,
                                     cinfo->output_width * (JDIMENSION)cinfo->output_components, 1);
  while (cinfo->output_scanline < cinfo->output_height) {
    unsigned char* destination = pixels + (size_t)cinfo->output_scanline * rowBytes;
    JDIMENSION column;
    jpeg_read_scanlines(cinfo, line, 1);
    if (grayscale) {
      for (column = 0; column < cinfo->output_width; ++column)
        destination[column] = line[0][column];
    } else {
      for (column = 0; column < cinfo->output_width; ++column) {
        destination[column * 4 + 0] = line[0][column * 3 + 0];
        destination[column * 4 + 1] = line[0][column * 3 + 1];
        destination[column * 4 + 2] = line[0][column * 3 + 2];
        destination[column * 4 + 3] = 255;
      }
    }
  }

  jpeg_finish_decompress(cinfo);
  jpeg_destroy_decompress(cinfo);
  return ROCKS_JPEG_OK;
}

RocksJpegResult RocksJpegDecode(const unsigned char* data, size_t size, int grayscale, unsigned maxScans,
                                unsigned char* pixels, size_t rowBytes)
{
  RocksJpegState state;
  if (size > ULONG_MAX)
    return ROCKS_JPEG_ERROR;

  rocksJpegInitState(&state);
  return rocksJpegDecodeWith(&state, data, size, grayscale, maxScans, pixels, rowBytes);
}
