/* Rocks configuration header for OpenEXRCore 3.4.3 (Docs/Design/AssetSystem section 12.10), written by hand in place of the
   file CMake generates from cmake/OpenEXRConfigInternal.h.in. Only the internal libdeflate copy is selected. The inline AVX
   assembly probe (OPENEXR_IMF_HAVE_GCC_INLINE_ASM_AVX) stays undefined so that the DWA path uses its portable code, and
   OPENEXR_MISSING_ARM_VLD1 stays undefined because every Rocks ARM compiler has the vld1q_*_x4 intrinsics. The other entries of
   the template configure the C++ library and the threading pool, which Rocks does not build. */
#ifndef INCLUDED_OPENEXR_INTERNAL_CONFIG_H
#define INCLUDED_OPENEXR_INTERNAL_CONFIG_H 1

#pragma once

#define OPENEXR_USE_INTERNAL_DEFLATE 1

#endif /* INCLUDED_OPENEXR_INTERNAL_CONFIG_H */
