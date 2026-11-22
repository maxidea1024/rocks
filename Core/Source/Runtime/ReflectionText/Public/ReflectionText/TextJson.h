#pragma once

#include <Core/Text/Json.h>
#include <ReflectionText/TextAssignments.h>

// 리플렉션 값의 JSON 표기다(리플렉션 B9i4 — Docs/Design/ReflectionExtensions.ko.md 10.4·10.5절).
// 대상(살아 있는 객체·staged 객체·struct 값)의 필드를 JSON 객체 하나로 쓰고(ExportJson), JSON 객체
// 하나를 대상에 읽는 대입 묶음을 준비한다(ImportJson — 원자성·override 경로는 명령행 대입 묶음과
// 같다).
//
// 표기(10.4절 표의 JSON 열): 객체 키는 필드 도구 이름이다. bool true·false / 정수 수(int64·uint64
// 그대로) / float는 단정도 최단(JsonWriter::Float), double은 배정도 최단, 비유한 값은 문자열
// "nan"·"inf"·"-inf" / enum 도구 이름 문자열 / Flags enum 이름 배열 ["A","B"](0은 []) / Guid·
// AssetId·Digest256·String·Name 문자열 / ObjectRef·WeakObjectRef·Soft·ClassRef 문자열(resolver·타입
// 도구 이름), null은 null / Array·StaticArray·Set 배열 / Map 객체(키는 명령행 키 텍스트, 삽입 순서)
// / Optional unset은 null / struct 객체.
namespace rocks::reflection_text {

// target의 필드를 JSON 객체 하나로 writer에 쓴다(키 순서는 스키마 필드 순서, Map 항목·Set 원소는
// 삽입 순서). options.use의 출력 제외 플래그(10.1절 표 — 경로의 어느 단계든)가 있는 필드는 쓰지
// 않는다. 참조·Soft는 options.resolver로 쓰고(없거나 쓰지 못하면 경로 없는 Soft만 36자 AssetId),
// Name은 options.names(없으면 대상 Registry의 pool)로 쓴다. 읽기는 경로 연산(PathRead·PathCount·
// PathReadKeys)이라 대상을 바꾸지 않는다 — 대상의 소유 스레드에서 부른다.
//
// 반환: 성공하면 writer에 객체 하나를 다 썼다(Finish는 호출자가 한다). 오류: 옵션 InvalidArgument,
// options.names가 대상 Registry의 pool과 다르면 InvalidArgument, 쓸 수 없는 값(resolver가 쓰지
// 못한 non-null Hard·Weak·경로 있는 Soft, 이름 없는 enum 값, 아직 표기가 없는 종류 —
// InstancedStruct·Text·Opaque)은 Unsupported, 경로 연산의 오류(StaleHandle·WrongThread·Busy 등),
// writer의 오류(InvalidArgument·CapacityExceeded·OutOfMemory). 실패하면 writer는 실패 상태다(결과를
// 버린다).
//
// 사용 예(asset-info --json):
//   StringBuilder<> text(allocator);
//   JsonWriter writer(text);
//   TextCodecOptions options;                 // Edit 용도 — --all이면 options.use =
//   TextUse::Report options.resolver = &assetResolver; if (auto written =
//   ExportJson(AssignmentTarget::Live(registry, access, texture), writer, options);
//       !written || !writer.Finish()) {
//       ...
//   }
ROCKS_REFLECTIONTEXT_API Result<void> ExportJson(const AssignmentTarget& target, JsonWriter& writer,
                                                 const TextCodecOptions& options) noexcept;

// JSON 객체 root를 target에 읽는 대입 묶음을 준비한다(아무것도 게시하지 않는다 — Publish는
// 호출자가 한다). import 의미(10.4절 표):
//   - struct(최상위 객체 포함)는 멤버 재귀 병합이다: JSON에 없는 키는 지금 값을 둔다.
//   - Array·StaticArray·Set·Optional은 통째 교체다(Optional null은 unset, Optional<struct>는 기본값
//     뒤 멤버 병합, struct 원소 배열은 비운 뒤 원소마다 기본 원소 뒤 멤버 병합).
//   - Map은 options.mapMerge가 Replace면 통째 교체, MergeByKey면 JSON에 있는 키만 넣거나 바꾸고
//     (struct 값은 있던 항목이면 멤버 병합, 없던 항목이면 기본값 뒤 멤버 병합) 없는 키는 둔다.
// 객체 키는 필드 도구 이름이고 대소문자를 무시해 맞춘다. 대입 하나하나는 명령행 대입과 같은
// 규칙으로 준비 단위에 적용된다(PrepareAssignments — ReadOnly·같은 값 무동작·참조 고정·override
// 경로).
//
// 진단(위치는 그 JSON 값의 시작 — TextSource::Json, path는 필드 경로 텍스트):
// UnknownProperty(모르는 키), DuplicateKey(대소문자만 다른 두 키, Set 원소·Map 키 중복),
// ExcludedProperty(용도의 입력 제외 필드), InvalidValue(JSON 종류가 필드와 맞지 않음, 값 형식),
// OutOfRange(정수 폭·원소 수 — options.maxElements·필드 MaxElements), HiddenEnumerator, ReadOnly,
// UnknownType·Unresolved· Unsupported(참조·ClassRef, 아직 표기가 없는 종류, 원소를 담은
// StructRefArray). 진단은 한도 (options.maxDiagnostics) 안에서 모두 모은다.
//
// 반환: 진단이 없으면 게시할 묶음(Touches·Append·Publish — 10.2절). 오류: 진단을 남겼으면
// InvalidArgument(내용은 diagnostics), Report 용도·옵션 InvalidArgument, 무효 root InvalidArgument,
// JSON 깊이가 options.maxJsonDepth를 넘거나 입력 byte(키·문자열·숫자 토큰)가
// options.maxInputBytes를 넘으면 CapacityExceeded, 그 밖은 PrepareAssignments의 호출 오류. 실패하면
// 대상은 바뀌지 않는다(Name intern은 남을 수 있다).
//
// 수명: root의 문서와 diagnostics·options.resolver·names, 대상은 반환한 묶음보다 오래 살아야 한다
// (문서는 이 호출 동안만 — 묶음은 값을 복사한다).
//
// 사용 예(TargetProfile 재정의):
//   JsonDocument document(allocator);
//   if (!document.Parse(text).Succeeded()) { ... }
//   TextCodecOptions options;
//   options.mapMerge = MapMergeMode::MergeByKey;
//   auto prepared = ImportJson(document.Root(), AssignmentTarget::Value(registry, access, lease,
//                                                                       &settings, allocator),
//                              options, diagnostics);
//   if (!prepared) { /* diagnostics의 line·column 보고 */ }
//
//   auto published = prepared.Value().Publish();
ROCKS_REFLECTIONTEXT_API Result<PreparedAssignments>
ImportJson(JsonValue root, const AssignmentTarget& target, const TextCodecOptions& options,
           TextDiagnostics& diagnostics) noexcept;

} // namespace rocks::reflection_text
