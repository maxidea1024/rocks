/* Rocks glue for libsamplerate (Docs/Design/AudioSystem section 3.31). Upstream samplerate.c
   builds its version string from the PACKAGE and VERSION macros that configure writes into
   config.h. Rocks builds without config.h (the other settings are manifest defines), so this unit
   defines the two string literals and compiles the upstream file. */
#define PACKAGE "libsamplerate"
#define VERSION "0.2.2"
#include "libsamplerate-0.2.2/src/samplerate.c"
