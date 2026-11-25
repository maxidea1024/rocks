#ifndef __CONFIG_TYPES_H__
#define __CONFIG_TYPES_H__

/* Rocks: the file configure generates from include/ogg/config_types.h.in on platforms with stdint.h
   (Linux and Android). os_types.h includes it outside its Windows and Apple branches. */
#include <stdint.h>

typedef int16_t ogg_int16_t;
typedef uint16_t ogg_uint16_t;
typedef int32_t ogg_int32_t;
typedef uint32_t ogg_uint32_t;
typedef int64_t ogg_int64_t;
typedef uint64_t ogg_uint64_t;

#endif
