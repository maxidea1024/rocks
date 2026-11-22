/* Rocks glue for libFLAC (Docs/Design/AudioSystem.ko.md section 3.27). On Windows, libFLAC opens
   files through the UTF-8 wrappers of src/share/win_utf8_io (compat.h maps flac_fopen to
   fopen_utf8), which the file-based decoder init functions reference. The upstream file includes
   windows.h and is Windows only. The manifest defines ROCKS_FLAC_WIN_UTF8_IO for Windows builds
   (platformDefines), so the platform choice stays in the manifest and this unit compiles the file
   only then. Other platforms map the wrappers to the C library and need nothing from this unit. */
#if defined(ROCKS_FLAC_WIN_UTF8_IO)
/* The upstream file relies on windows.h to declare malloc, calloc and free; the Rocks build defines
   WIN32_LEAN_AND_MEAN, which leaves stdlib.h out, so it is included here first. */
#include <stdlib.h>

#include "flac-1.5.0/src/share/win_utf8_io/win_utf8_io.c"
#else
typedef int RocksFlacWindowsFileIoUnused;
#endif
