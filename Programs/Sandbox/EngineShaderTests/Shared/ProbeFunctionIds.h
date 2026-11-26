// 엔진 셰이더 probe 함수 번호다(Docs/Design/EngineShaders 7.6절). probe 셰이더(HLSL)와 Sandbox
// EngineShaderSelfTest(C++)가 함께 include한다. 루트 상수 control.x 하위 16 bit가 함수, 상위 16
// bit가 출력 선택자다. 번호를 바꾸면 두 쪽이 함께 바뀐다(golden은 번호와 무관하게 CPU 기준 구현이
// 만든다).
#pragma once

#if defined(ROCKS_SHADER_MODEL_MAJOR)
#define ENGINE_SHADER_PROBE(Name, Value) static const uint Probe##Name = Value;
#else
#include <Base/Types.h>
namespace sandbox::engine_shader_probe {
#define ENGINE_SHADER_PROBE(Name, Value) inline constexpr rocks::uint32 Probe##Name = Value;
#endif

// --- Math float(MathFloatProbePixel) ---
ENGINE_SHADER_PROBE(ScalarPowers, 1u)
ENGINE_SHADER_PROBE(ScalarMinMax, 2u)
ENGINE_SHADER_PROBE(ScalarSafe, 3u)
ENGINE_SHADER_PROBE(SafeNormalize, 4u)
ENGINE_SHADER_PROBE(MathConstants, 5u)
ENGINE_SHADER_PROBE(PowPositive, 6u)
ENGINE_SHADER_PROBE(OrthonormalBasis, 10u)
ENGINE_SHADER_PROBE(GramSchmidt, 11u)
ENGINE_SHADER_PROBE(ReflectRefract, 12u)
ENGINE_SHADER_PROBE(AngleBetween, 13u)
ENGINE_SHADER_PROBE(MatrixTransform, 20u)
ENGINE_SHADER_PROBE(Inverse3x3, 21u)
ENGINE_SHADER_PROBE(NormalMatrix, 22u)
ENGINE_SHADER_PROBE(ExtractTransform, 23u)
ENGINE_SHADER_PROBE(Projection, 24u)
ENGINE_SHADER_PROBE(Quaternion, 30u)
ENGINE_SHADER_PROBE(TangentFrame, 31u)
ENGINE_SHADER_PROBE(FastTrig, 40u)
ENGINE_SHADER_PROBE(FastExpLog, 41u)
ENGINE_SHADER_PROBE(ExtendedSums, 50u)
ENGINE_SHADER_PROBE(TranslatedWorld, 51u)
ENGINE_SHADER_PROBE(NormalizedDecode, 60u)
ENGINE_SHADER_PROBE(SmallFloatDecode, 61u)
ENGINE_SHADER_PROBE(Rgb9e5Decode, 62u)
ENGINE_SHADER_PROBE(HalfDecode, 63u)
ENGINE_SHADER_PROBE(Octahedral, 70u)
ENGINE_SHADER_PROBE(Dither, 71u)
ENGINE_SHADER_PROBE(UnitFloat, 80u)
ENGINE_SHADER_PROBE(RandomStream, 81u)
ENGINE_SHADER_PROBE(ScreenNoise, 82u)
ENGINE_SHADER_PROBE(LowDiscrepancy, 83u)
ENGINE_SHADER_PROBE(SampleWarping, 90u)
ENGINE_SHADER_PROBE(SphericalHarmonicsBasis, 92u)
ENGINE_SHADER_PROBE(SphericalHarmonicsGrid, 93u)
ENGINE_SHADER_PROBE(SphericalHarmonicsIrradiance, 94u)
ENGINE_SHADER_PROBE(RayIntersection, 95u)

// --- Math uint(MathUintProbePixel) ---
ENGINE_SHADER_PROBE(FiniteBits, 100u)
ENGINE_SHADER_PROBE(NormalizedRoundTrip, 101u)
ENGINE_SHADER_PROBE(NormalizedEncode, 102u)
ENGINE_SHADER_PROBE(PackColor, 103u)
ENGINE_SHADER_PROBE(SmallFloatEncode, 105u)
ENGINE_SHADER_PROBE(SmallFloatRoundTrip, 106u)
ENGINE_SHADER_PROBE(Rgb9e5Encode, 107u)
ENGINE_SHADER_PROBE(HalfEncode, 108u)
ENGINE_SHADER_PROBE(HalfRoundTrip, 109u)
ENGINE_SHADER_PROBE(BitFields, 110u)
ENGINE_SHADER_PROBE(QuantizeOctahedral, 111u)
ENGINE_SHADER_PROBE(Morton, 120u)
ENGINE_SHADER_PROBE(Hash, 121u)
ENGINE_SHADER_PROBE(RandomStreamBits, 122u)
ENGINE_SHADER_PROBE(LowDiscrepancyBits, 123u)
ENGINE_SHADER_PROBE(BlueNoise, 124u)
ENGINE_SHADER_PROBE(Watertight, 125u)

// --- Color(ColorFloatProbePixel) ---
ENGINE_SHADER_PROBE(ColorMatrix, 200u)
ENGINE_SHADER_PROBE(ColorMatrixRows, 201u)
ENGINE_SHADER_PROBE(ColorFunctions, 202u)
ENGINE_SHADER_PROBE(Adaptation, 203u)
ENGINE_SHADER_PROBE(WhiteBalance, 204u)
ENGINE_SHADER_PROBE(ColorTemperature, 205u)
ENGINE_SHADER_PROBE(Transfer, 206u)
ENGINE_SHADER_PROBE(SrgbRoundTrip, 207u)
ENGINE_SHADER_PROBE(PqStandard, 208u)
ENGINE_SHADER_PROBE(Filmic, 209u)

// --- Shading(ShadingFloatProbePixel) ---
ENGINE_SHADER_PROBE(Distributions, 300u)
ENGINE_SHADER_PROBE(DistributionGrid, 301u)
ENGINE_SHADER_PROBE(Visibility, 302u)
ENGINE_SHADER_PROBE(Fresnel, 303u)
ENGINE_SHADER_PROBE(Reciprocity, 304u)
ENGINE_SHADER_PROBE(Furnace, 305u)
ENGINE_SHADER_PROBE(Diffuse, 306u)
ENGINE_SHADER_PROBE(EnvironmentBrdf, 307u)
ENGINE_SHADER_PROBE(SurfaceDefaults, 308u)
ENGINE_SHADER_PROBE(ShadingParameters, 309u)
ENGINE_SHADER_PROBE(VndfSample, 310u)
ENGINE_SHADER_PROBE(VndfPdfGrid, 311u)

// 행렬 번호(ColorMatrix·ColorMatrixRows 선택자)다.
ENGINE_SHADER_PROBE(MatrixRec709ToXyz, 0u)
ENGINE_SHADER_PROBE(MatrixXyzToRec709, 1u)
ENGINE_SHADER_PROBE(MatrixRec2020ToXyz, 2u)
ENGINE_SHADER_PROBE(MatrixXyzToRec2020, 3u)
ENGINE_SHADER_PROBE(MatrixDisplayP3ToXyz, 4u)
ENGINE_SHADER_PROBE(MatrixXyzToDisplayP3, 5u)
ENGINE_SHADER_PROBE(MatrixAP0ToXyz, 6u)
ENGINE_SHADER_PROBE(MatrixXyzToAP0, 7u)
ENGINE_SHADER_PROBE(MatrixAP1ToXyz, 8u)
ENGINE_SHADER_PROBE(MatrixXyzToAP1, 9u)
ENGINE_SHADER_PROBE(MatrixAP0ToAP1, 10u)
ENGINE_SHADER_PROBE(MatrixAP1ToAP0, 11u)
ENGINE_SHADER_PROBE(MatrixRec709ToRec2020, 12u)
ENGINE_SHADER_PROBE(MatrixRec2020ToRec709, 13u)
ENGINE_SHADER_PROBE(MatrixBradfordD65ToD60, 14u)
ENGINE_SHADER_PROBE(MatrixBradfordD60ToD65, 15u)
ENGINE_SHADER_PROBE(MatrixCount, 16u)

#undef ENGINE_SHADER_PROBE

#if !defined(ROCKS_SHADER_MODEL_MAJOR)
} // namespace sandbox::engine_shader_probe
#endif
