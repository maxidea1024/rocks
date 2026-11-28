#pragma once

#include <AssetNugget/NuggetFileSystemBackend.h>
#include <Core/Containers/Array.h>
#include <Core/Containers/Span.h>
#include <Core/Result.h>
#include <Core/Text/NamePool.h>
#include <MaterialCompiler/MaterialConstants.h>
#include <MaterialCompiler/MaterialGeneratedSource.h>
#include <MaterialCompiler/MaterialTranslation.h>
#include <MaterialCook/MaterialTranslationValue.h>

// 번역 너겟 값의 부호화와 셰이더 집합 id다(Docs/Design/MaterialSystem 12.8절 MS3c "값"·"셰이더 집합
// id", 8.6절). 값은 part 0(머리 — 형식 버전·part 수·진단 수)과 part 1–12(MaterialTranslationValue
// 순서)이고 각 part는 little-endian 고정 배치다. 해석은 형식 버전·part 수·크기·열거 범위가 맞지
// 않으면 손상(CorruptData — 처리기는 적중을 미적중으로 다룬다).
//
// 사용 예:
//   MaterialTranslationValue value(allocator);
//   MakeMaterialTranslationValue(names, translation, source, constants, traits, value);
//   Array<NuggetValue> parts(allocator);
//   WriteMaterialTranslationNugget(value, parts);   // Put
//   const Sha256Digest set = MaterialShaderSetId(value);
namespace rocks {

// 값 형식 버전이다(배치가 바뀌면 올린다 — MaterialTranslationNuggetId 버전도 바꾼다).
inline constexpr uint32 MaterialTranslationValueVersion = 1;
// 값 part 수다(머리 포함).
inline constexpr uint32 MaterialTranslationValueParts = 13;
// 셰이더 집합 형식 버전이다(8.6절 — 바꾸면 모든 셰이더 집합 id가 바뀐다).
inline constexpr uint32 MaterialShaderSetFormatVersion = 1;

// 성공한 번역·생성 결과에서 값을 만든다(out을 비우고 채운다). traits는 옮긴다. 루트 그래프 진단의
// asset(rootAsset)은 null로 담는다. 파라미터 기본값과 파라미터 텍스처의 defaultAsset은 담지 않는다.
// 오류: 번역·생성이 성공하지 않았으면 InvalidArgument, Name 조회 실패, 할당 실패.
ROCKS_MATERIALCOOK_API Result<void>
MakeMaterialTranslationValue(NamePool& names, const material::MaterialTranslation& translation,
                             const material::MaterialGeneratedSource& source,
                             const material::MaterialConstants& constants,
                             material::MaterialShaderTraits&& traits, Guid rootAsset,
                             MaterialTranslationValue& out) noexcept;

// 값을 너겟 part(ValueId 0–12 순)로 쓴다(out을 비우고 채운다). 오류: 크기가 u32를 넘음
// CapacityExceeded, 할당 실패.
ROCKS_MATERIALCOOK_API Result<void>
WriteMaterialTranslationNugget(const MaterialTranslationValue& value,
                               Array<NuggetValue>& out) noexcept;

// 너겟 part를 해석한다(out을 비우고 채운다). 오류: 손상 CorruptData, 할당 실패.
ROCKS_MATERIALCOOK_API Result<void>
ReadMaterialTranslationNugget(Span<const NuggetValue> parts,
                              MaterialTranslationValue& out) noexcept;

// 셰이더 집합 id다(8.6절): SHA-256(형식 버전, 생성 원문, layout 서명 = part 2 바이트의 SHA-256,
// traits 정규 바이트). 오류: 할당 실패.
ROCKS_MATERIALCOOK_API Result<Sha256Digest>
MaterialShaderSetId(const MaterialTranslationValue& value) noexcept;

// 라이브러리 묶음 id다(셰이더 집합 id 앞 8 byte little-endian, 0이면 1 — 0은 엔진 묶음, 12.8절).
ROCKS_MATERIALCOOK_API uint64 MaterialShaderBundleId(const Sha256Digest& shaderSet) noexcept;

} // namespace rocks
