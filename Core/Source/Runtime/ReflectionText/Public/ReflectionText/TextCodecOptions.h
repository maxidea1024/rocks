#pragma once

#include <Core/Result.h>
#include <Meta/TypeSchema.h>
#include <ReflectionText/TextDiagnostics.h>
#include <ReflectionText/TextUse.h>

namespace rocks {

class NamePool;

} // namespace rocks

// 텍스트·JSON codec 공용 옵션이다(리플렉션 B9i — Docs/Design/ReflectionExtensions.ko.md 10.2절).
// 대입 묶음(PrepareAssignments), 명령행 값 표기, JSON import·export가 같은 옵션을 받는다.
//
// 사용 예(asset-set 입력):
//   TextCodecOptions options;              // use = Edit, mapMerge = Replace, 기본 한도
//   options.resolver = &assetResolver;     // 논리 경로·AssetId 참조(A1·A2)
//   options.names = &registryNames;        // 대상 Registry의 RegistryOptions::names와 같은 pool
//   auto checked = ValidateTextCodecOptions(options);
namespace rocks::reflection_text {

// 참조 텍스트(논리 경로·AssetId)를 handle·Soft 값으로 풀고 반대로 쓰는 hook이다(10.3절 — 정의는
// <ReflectionText/TextReference.h>).
class TextReferenceResolver;

// JSON import가 기존 Map 값 위에 읽는 방식이다(10.4절 "JSON import 의미").
enum class MapMergeMode : uint8 {
    // Map을 통째로 교체한다(기본).
    Replace,
    // JSON에 있는 키만 병합하고(struct 값은 멤버 재귀 병합) 없는 키는 유지한다.
    MergeByKey
};

// 한 번의 codec 호출(Prepare와 그 묶음의 Append 누계, 또는 JSON import 한 번)이 복사·해석하는 입력
// 텍스트 byte 합의 기본 상한이다(Core JsonReadOptions::maxInputBytes와 같은 64 MiB).
inline constexpr uint64 DefaultMaxTextInputBytes = 64ull << 20;

// JSON import가 따라 들어가는 JSON 값 중첩 깊이의 기본 상한이다(Core JsonReadOptions::maxDepth와
// 같다).
inline constexpr uint32 DefaultMaxTextJsonDepth = 128;

// 값 텍스트 하나(쉼표 목록·Map 항목)나 JSON 배열·객체 하나가 만드는 원소 수의 기본 상한이다. 필드의
// MaxElements보다 먼저, 원소를 만들기 전에 거부하는 해석 상한이다(최댓값 MaxArrayElements).
inline constexpr uint32 DefaultMaxTextElements = MaxArrayElements;

struct TextCodecOptions {
    // 출력·입력 제외 플래그를 고르는 용도(10.1절). Report는 출력 전용이다.
    TextUse use = TextUse::Edit;
    // 참조 resolver(빌린다 — codec 호출 동안 살아 있어야 한다). 없으면 참조 입력은 `none`과 36자
    // AssetId(빈 객체 경로 Soft)만 받고 non-null Hard·Weak는 진단 Unsupported, 출력은 36자
    // AssetId다.
    TextReferenceResolver* resolver = nullptr;
    // Name 값의 intern·조회 pool(빌린다). 객체 대상이면 대상 Registry의 RegistryOptions::names와
    // 같아야 한다(다르면 대입 묶음이 InvalidArgument). 없으면 Name 입력은 진단 Unsupported다.
    NamePool* names = nullptr;
    // JSON import의 Map 병합 방식(10.4절).
    MapMergeMode mapMerge = MapMergeMode::Replace;
    // 게시 전 struct 검증 hook(Docs/Design/CurveData/README.ko.md 9.1절)을 부르는지다. false는 최종
    // 값이 아닌 중간 값(설정 층 배열 연산 목록의 정규 표기를 얻으려고 기본값에 잠시 넣는 import
    // 등)에만 쓴다 — 그 결과를 게시할 최종 값은 반드시 true로 다시 거친다. 기본은 true다.
    bool validateStructHooks = true;
    // 저장할 진단 수 상한(TextDiagnostics::SetLimit). 1 이상.
    uint32 maxDiagnostics = DefaultMaxTextDiagnostics;
    // 입력 텍스트 byte 합 상한. 넘으면 CapacityExceeded다. 1 이상.
    uint64 maxInputBytes = DefaultMaxTextInputBytes;
    // JSON 값 중첩 깊이 상한. 넘으면 CapacityExceeded다. 1 이상.
    uint32 maxJsonDepth = DefaultMaxTextJsonDepth;
    // 값 하나의 원소 수 상한. 넘으면 진단 OutOfRange다. 1 이상 MaxArrayElements 이하.
    uint32 maxElements = DefaultMaxTextElements;
    // 순회(VisitValues)만 본다: 참이면 텍스트 꼴이 없는 필드(접근기 없는 Opaque)를 Unsupported로
    // 멈추지 않고 건너뛴다. 값 일부만 보는 소비자(에셋 bundle 태그 수집 — 소프트 참조만 본다,
    // AssetSystem 10.2절 A4e2b)가 쓴다. 텍스트 출력·키 기록은 거짓(빠진 필드를 숨기지 않는다).
    bool skipFieldsWithoutText = false;
};

// 옵션 값의 범위를 검사한다(대상과 무관한 검사만 — names와 대상 Registry pool의 일치는 대입 묶음이
// 본다). codec 진입점이 맨 앞에서 부른다.
// 오류: use·mapMerge가 정의되지 않은 값이거나 한도가 0, maxElements > MaxArrayElements면
// InvalidArgument. 입력 가능 여부(Report)는 보지 않는다 — 입력 진입점은 TextInputExcludedFlags로
// 본다.
ROCKS_REFLECTIONTEXT_API Result<void>
ValidateTextCodecOptions(const TextCodecOptions& options) noexcept;

} // namespace rocks::reflection_text
