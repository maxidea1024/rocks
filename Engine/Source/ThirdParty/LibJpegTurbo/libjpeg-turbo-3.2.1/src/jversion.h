/* Rocks configuration header for libjpeg-turbo 3.2.1 (Docs/Design/AssetSystem section 12.11): the template
   src/jversion.h.in copied as is with the CMake values filled in by hand (no SIMD, arithmetic coding on,
   libjpeg API version 62 - the libjpeg-turbo default). */
/*
 * jversion.h
 *
 * This file was part of the Independent JPEG Group's software:
 * Copyright (C) 1991-2020, Thomas G. Lane, Guido Vollbeding.
 * libjpeg-turbo Modifications:
 * Copyright (C) 2010, 2012-2026, D. R. Commander.
 * For conditions of distribution and use, see the accompanying README.ijg
 * file.
 *
 * This file contains software version identification.
 */


#if JPEG_LIB_VERSION >= 80

#define JVERSION        "8d  15-Jan-2012"

#elif JPEG_LIB_VERSION >= 70

#define JVERSION        "7  27-Jun-2009"

#else

#define JVERSION        "6b  27-Mar-1998"

#endif

#define JCOPYRIGHT \
  "Copyright (C) 1991-2026 The libjpeg-turbo Project and many others"
