/* Rocks glue for miniaudio (Docs/Design/AudioSystem section 3.34.1). It compiles the single-file
   implementation once. The device-IO-only configuration (no decoding, encoding, generation,
   resource manager, node graph or engine; WASAPI, Core Audio, AAudio and OpenSL ES backends only)
   comes from the defines in ThirdParty.json, which AudioMiniaudio also sees so that the structure
   layouts match. */
#define MINIAUDIO_IMPLEMENTATION
#include "miniaudio.h"
