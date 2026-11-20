/* Rocks glue: reference build of the unmodified upstream mikktspace.c behaviour.
 *
 * Comparison tests (Docs/Design/MeshAnimationAssets.ko.md section 12, MeshTools MikkTSpace
 * reference tests) need the original algorithm next to the patched one. This translation unit
 * defines the Rocks hooks of the patched source as the exact original upstream expressions (CRT
 * cos/acos and malloc/free) and renames the two public functions so that both builds link into one
 * executable. Production code never calls these functions. */
#include <math.h>
#include <stdlib.h>

#define ROCKS_MIKKTSPACE_THRESHOLD_COS(fAngularThreshold)                                          \
    ((float)cos((fAngularThreshold * (float)M_PI) / 180.0f))
#define ROCKS_MIKKTSPACE_ACOS(fCos) ((float)acos(fCos))
#define ROCKS_MIKKTSPACE_MALLOC(pContext, uBytes) malloc(uBytes)
#define ROCKS_MIKKTSPACE_FREE(pContext, pMemory) free(pMemory)
#define genTangSpaceDefault RocksMikkTSpaceReferenceGenTangSpaceDefault
#define genTangSpace RocksMikkTSpaceReferenceGenTangSpace

#include "mikktspace-3e895b49d05e/mikktspace.c"
