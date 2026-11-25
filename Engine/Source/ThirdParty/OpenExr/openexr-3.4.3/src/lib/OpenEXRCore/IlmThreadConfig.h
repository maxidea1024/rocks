/* Rocks configuration header for OpenEXRCore 3.4.3 (Docs/Design/AssetSystem section 12.10). openexr_config.h includes the
   IlmThread configuration, which CMake generates from cmake/IlmThreadConfig.h.in. OpenEXRCore tests the threading macro with
   #ifdef, so Rocks defines nothing here: no context mutex, no POSIX file locking. ImageCodec uses one context from one thread
   with memory read callbacks, which is safe without locks. */
#ifndef INCLUDED_ILMTHREAD_CONFIG_H
#define INCLUDED_ILMTHREAD_CONFIG_H 1

#pragma once

#endif /* INCLUDED_ILMTHREAD_CONFIG_H */
