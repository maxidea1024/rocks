#pragma once

#include <Base/Types.h>

// 상수 식 프로그램의 형식 계약이다(Docs/Design/MaterialSystem 9.4절, 12.8절 MS3d2). 컴파일러 상수
// 빌더와 실행 시 평가기가 같은 값을 쓴다(실행 시 평가는 컴파일러 형을 보지 않는다).
//
// 즉시값 규칙(MaterialConstantInstruction::immediates):
// - Constant: 성분마다 비트 패턴(Float 32 bit, Double 64 bit).
// - Swizzle: [0]에 성분 번호를 MaterialConstantSwizzleBits씩(성분 0이 하위).
// - Parameter·Texture: [MaterialConstantTableImmediate]이 artifact 파라미터 번호(번역 파라미터 표
//   순 — MaterialArtifact::parameters가 Material::parameters 번호로 잇는다)·artifact 텍스처 slot
//   번호다. [0]·[1]·[2]는 번역의 정체·종류 값이라 평가가 쓰지 않는다.
// - TextureSize: [0]이 MaterialTextureSizeLogical(크기) 또는 MaterialTextureSizeTexel(1/크기)다.
// - 그 밖의 연산은 0이다. operandCount 밖 피연산자 칸은 0이다.
namespace rocks::material {

// 프로그램 명령 형식 버전이다(바꾸면 올린다 — 번역 너겟 키).
inline constexpr uint32 MaterialConstantProgramVersion = 1;
// Parameter·Texture 명령이 표 번호를 담는 즉시값 칸이다.
inline constexpr usize MaterialConstantTableImmediate = 3;
// Swizzle 성분 번호 하나의 bit 수다.
inline constexpr uint32 MaterialConstantSwizzleBits = 2;
// TextureSize 모드다.
inline constexpr uint64 MaterialTextureSizeLogical = 0;
inline constexpr uint64 MaterialTextureSizeTexel = 1;
// Double 값(LWC) tile 한 칸의 크기(cm)다(DoubleTile·DoubleOffset 칸 — MaterialDouble.rsh
// MaterialDoubleTileSize, UE UE_LWC_RENDER_TILE_SIZE,
// Core/Public/Misc/LargeWorldRenderPosition.h:14).
inline constexpr double MaterialDoubleTileSize = 2097152.0;

} // namespace rocks::material
