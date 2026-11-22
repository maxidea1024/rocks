/* Rocks glue for libFLAC (Docs/Design/AudioSystem.ko.md section 3.27). Upstream format.c takes its
   version strings from the PACKAGE_VERSION macro that configure writes into config.h. Rocks builds
   without config.h (the other settings are manifest defines), so this unit defines the string
   literal and compiles the upstream file. */
#define PACKAGE_VERSION "1.5.0"
#include "flac-1.5.0/src/libFLAC/format.c"
