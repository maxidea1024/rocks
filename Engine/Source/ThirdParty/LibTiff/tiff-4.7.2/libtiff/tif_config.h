/* Rocks configuration header for libtiff 4.7.2 (Docs/Design/AssetSystem section 12.10), written by hand in place of the file the
   libtiff CMake build generates from tif_config.h.cmake.in. Only the entries the imported library sources test are set: the
   codec switches come from tiffconf.h, fcntl.h supplies the open mode flags _TIFFgetMode uses, and the size_t width selects the
   printf formats of the library messages. Rocks reads through TIFFClientOpenExt only, so no file I/O configuration is needed. */
#ifndef ROCKS_TIF_CONFIG_H
#define ROCKS_TIF_CONFIG_H

#include "tiffconf.h"

#define HAVE_ASSERT_H 1
#define HAVE_FCNTL_H 1

#define PACKAGE "LibTIFF Software"
#define PACKAGE_BUGREPORT "tiff@lists.osgeo.org"
#define PACKAGE_NAME "LibTIFF Software"
#define PACKAGE_TARNAME "tiff"
#define PACKAGE_URL ""

#if SIZE_MAX == UINT64_MAX
#define SIZEOF_SIZE_T 8
#else
#define SIZEOF_SIZE_T 4
#endif

#define WORDS_BIGENDIAN 0

#define TIFF_SIZE_FORMAT "zu"
#if SIZEOF_SIZE_T == 8
#define TIFF_SSIZE_FORMAT PRId64
#elif SIZEOF_SIZE_T == 4
#define TIFF_SSIZE_FORMAT PRId32
#else
#error "Unsupported size_t size"
#endif

#endif /* ROCKS_TIF_CONFIG_H */
