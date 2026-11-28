#pragma once

#include <AssetNugget/NuggetKey.h>
#include <Core/Encoding/Sha256.h>
#include <Core/Result.h>
#include <MaterialCook/MaterialTranslationKeyDesc.h>

// 머티리얼 번역 너겟 키다(Docs/Design/MaterialSystem 12.2절 표·12.8절 MS3c "번역 너겟"). AssetId
// 없는 평평한 키다(static 변형 공유 — 8.5절, 16절 MAS6). 순서: 공통 머리(정체성·결정적 수학
// 버전·툴체인 지문), 키 배치 버전, 에셋 TypeId, 루트 그래프 semantic hash(파라미터 기본값·기본
// 텍스처 제외), 기본 속성(리플렉션 정규 인코딩), static 값(parameterId 순), 번역 대상 바이트 전체,
// custom 속성 표 서명(지금은 내장 표만 — "builtin"), 쓰인 노드 종류의 (이름, schemaVersion,
// loweringVersion) 이름 순 목록(루트와 모은 그래프 전부), 계약 버전(MaterialCommon·상수
// 프로그램·특성 정규 바이트), 의존 (역할 문자열, AssetId, 내용 키) 목록((역할, AssetId) 순 — 열지
// 못한 대상은 None, 텍스처 역할은 아래 위치 목록이 대신한다), 텍스처 참조 위치 (그래프 asset(루트는
// null), 노드 id, 필드, 사실 바이트 또는 None) 목록(AssetId 없이 — 기본 텍스처를 사실이 같은
// 텍스처로 바꾸면 적중).
//
// 사용 예:
//   auto key = ComputeMaterialTranslationKey(desc);
//   NuggetCacheGet get{MaterialTranslationBucket, key.Value()};
namespace rocks {

// 번역 너겟 정체성이다(IR·생성 규칙·값 배치가 바뀌면 버전 Guid를 바꾼다 — 모든 키가 바뀐다).
inline constexpr NuggetProcessorId MaterialTranslationNuggetId{
    Guid{0x524f434b4d415452ull, 0x0000000000000001ull},
    Guid{0x524f434b4d41544eull, 0x0000000000000001ull}};

// 번역 너겟 bucket이다(소문자 규칙 — UE "MaterialTranslation" 버킷 대응, 12.1절).
inline constexpr char MaterialTranslationBucket[] = "material_translation";

// 키 본문 배치 버전이다(배치가 바뀌면 올린다).
inline constexpr uint32 MaterialTranslationKeyLayoutVersion = 1;

// 생성 원문이 기대는 MaterialCommon.rsh 계약 버전이다(그 계약이 바뀌면 올린다 — 7.2절).
inline constexpr uint32 MaterialCommonContractVersion = 1;

// desc의 번역 너겟 키다. 오류: desc의 필수 포인터가 null이면 InvalidArgument, semantic hash 오류,
// 리플렉션 쓰기 오류, 할당 실패.
ROCKS_MATERIALCOOK_API Result<Sha256Digest>
ComputeMaterialTranslationKey(const MaterialTranslationKeyDesc& desc) noexcept;

} // namespace rocks
