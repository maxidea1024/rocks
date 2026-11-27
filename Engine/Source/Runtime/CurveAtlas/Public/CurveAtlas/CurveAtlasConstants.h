#pragma once

#include <Base/Types.h>

// 색 커브 atlas의 크기 상수다(Docs/Design/CurveAssets 4.2·4.3절, UE UCurveLinearColorAtlas).
namespace rocks {

// 생성자 너비다(UE 생성자 TextureSize = 256 — 클래스 기본값).
inline constexpr uint32 DefaultColorCurveAtlasWidth = 256;

// asset-new가 너비를 대입하지 않았을 때의 너비다(UE UCurveLinearColorAtlasFactory Width = 64 —
// EditorFactories.cpp:2363·2375, 원본 재구성 hook의 New 까닭이 둔다).
inline constexpr uint32 NewColorCurveAtlasWidth = 64;

// 굽는 너비의 최솟값이다(UE UpdateTextures — 표본 시간 x / (W − 1)에 W > 1이 필요하다).
inline constexpr uint32 MinColorCurveAtlasWidth = 2;

// 너비·높이(커브 수) 상한이다(텍스처 원본 임포트 상한 ImageDecodeLimits::DefaultMaxDimension과
// 같다 — 넘으면 굽기가 InvalidSetting).
inline constexpr uint32 MaxColorCurveAtlasDimension = 16384;

} // namespace rocks
