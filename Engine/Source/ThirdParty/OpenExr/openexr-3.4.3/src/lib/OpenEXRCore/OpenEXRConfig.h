/* Rocks configuration header for OpenEXRCore 3.4.3 (Docs/Design/AssetSystem section 12.10). The OpenEXR CMake build generates
   this file from cmake/OpenEXRConfig.h.in. Rocks compiles only the C core as a static library, so it writes by hand the subset
   OpenEXRCore uses: the version macros, empty export macros (no shared library, no visibility attributes) and the deprecation
   macro. The C++ library macros (namespaces, Imath and OpenJPH versions) are not needed by the C core. */
#ifndef INCLUDED_OPENEXR_CONFIG_H
#define INCLUDED_OPENEXR_CONFIG_H 1

#pragma once

#ifndef INCLUDED_OPENEXR_VERSION_H
#define OPENEXR_VERSION_MAJOR 3
#define OPENEXR_VERSION_MINOR 4
#define OPENEXR_VERSION_PATCH 3
#endif

#define OPENEXR_VERSION_STRING "3.4.3"
#define OPENEXR_PACKAGE_STRING "OpenEXR 3.4.3"
#define OPENEXR_VERSION_RELEASE_TYPE ""
#define OPENEXR_VERSION_EXTRA ""
#define OPENEXR_VERSION_HEX                                                                       \
    (((OPENEXR_VERSION_MAJOR) << 24) | ((OPENEXR_VERSION_MINOR) << 16) |                          \
     ((OPENEXR_VERSION_PATCH) << 8))

#define OPENEXR_EXPORT
#define OPENEXR_HIDDEN
#define OPENEXR_EXPORT_TYPE
#define OPENEXR_EXPORT_EXTERN_TEMPLATE
#define OPENEXR_EXPORT_ENUM
#define OPENEXR_EXPORT_TEMPLATE_TYPE
#define OPENEXR_EXPORT_TEMPLATE_INSTANCE

#if defined(__cplusplus) && (__cplusplus >= 201402L)
#define OPENEXR_DEPRECATED(msg) [[deprecated(msg)]]
#else
#define OPENEXR_DEPRECATED(msg) __attribute__((deprecated(msg)))
#endif

#endif /* INCLUDED_OPENEXR_CONFIG_H */
