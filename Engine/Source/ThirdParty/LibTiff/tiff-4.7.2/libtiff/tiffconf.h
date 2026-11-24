/* Rocks configuration header for libtiff 4.7.2 (Docs/Design/AssetSystem section 12.10), written by hand in place of the file the
   libtiff CMake build generates from tiffconf.h.cmake.in. Every Rocks target is little endian with IEEE floating point. The codecs
   are the lossless ones UE 5.8 also enables (CCITT, LZW, NeXT, PackBits, ThunderScan, ZIP through the Rocks zlib); JPEG and old
   JPEG arrive with libjpeg-turbo (A5f rejects them), and PixarLog, SGILog, JBIG, LERC, LZMA, WebP and ZSTD are not built.
   DEFAULT_EXTRASAMPLE_AS_ALPHA matches UE: a fourth sample without an ExtraSamples value is alpha. */
#ifndef _TIFFCONF_
#define _TIFFCONF_

#include <inttypes.h>
#include <stddef.h>
#include <stdint.h>

#define TIFF_INT16_T int16_t
#define TIFF_INT32_T int32_t
#define TIFF_INT64_T int64_t
#define TIFF_INT8_T int8_t
#define TIFF_UINT16_T uint16_t
#define TIFF_UINT32_T uint32_t
#define TIFF_UINT64_T uint64_t
#define TIFF_UINT8_T uint8_t

#if SIZE_MAX == UINT64_MAX
#define TIFF_SSIZE_T int64_t
#else
#define TIFF_SSIZE_T int32_t
#endif

#define HAVE_IEEEFP 1
#define HOST_FILLORDER FILLORDER_LSB2MSB
#define HOST_BIGENDIAN 0

#define CCITT_SUPPORT 1
#define LZW_SUPPORT 1
#define NEXT_SUPPORT 1
#define PACKBITS_SUPPORT 1
#define THUNDER_SUPPORT 1
#define ZIP_SUPPORT 1

#define STRIPCHOP_DEFAULT TIFF_STRIPCHOP
#define SUBIFD_SUPPORT 1
#define DEFAULT_EXTRASAMPLE_AS_ALPHA 1
#define CHECK_JPEG_YCBCR_SUBSAMPLING 1
#define MDI_SUPPORT 1

#define COLORIMETRY_SUPPORT
#define YCBCR_SUPPORT
#define CMYK_SUPPORT
#define ICC_SUPPORT
#define PHOTOSHOP_SUPPORT
#define IPTC_SUPPORT

#endif /* _TIFFCONF_ */
