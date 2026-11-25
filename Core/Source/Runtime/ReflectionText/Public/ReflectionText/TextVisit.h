#pragma once

#include <ReflectionText/TextAssignments.h>

// 리플렉션 값 순회다(Docs/Design/ReflectionExtensions.ko.md 10.6절 — 에셋 너겟 키의 리플렉션 값
// 기록, AssetSystem 9.1절 결정 2). ExportJson과 같은 스키마·경로 연산 순회로 대상의 값을 선언
// 순서대로 visitor에 넘긴다. 순회를 쓰는 쪽(키 기록 등)이 순회를 따로 만들지 않게 하는 공용 경로다.
namespace rocks::reflection_text {

// 순회 이벤트를 받는 쪽이다. 이벤트 순서(문법):
//   대상     := BeginStruct 필드* EndStruct
//   필드     := Field(이름) 값
//   값       := Value | Absent | Bytes | struct | 배열 | 집합 | 사전 | 인스턴스
//   struct   := BeginStruct 필드* EndStruct
//   배열     := BeginArray(n) 값×n EndArray          (Array·StaticArray — byte 배열은 Bytes 하나)
//   집합     := BeginSet(n) Value×n EndSet            (Set 원소 — 저장 순서)
//   사전     := BeginMap(n) (MapKey 값)×n EndMap      (Map 항목 — 저장 순서)
//   인스턴스 := Absent | Instanced(type, 이름) struct (InstancedStruct)
// 실패를 돌려주면 순회가 그 오류로 멈춘다(VisitValues가 그대로 돌려준다).
class ValueVisitor {
public:
    virtual ~ValueVisitor() = default;

    // struct(최상위 대상·struct 필드·Optional<struct> set·struct 원소·InstancedStruct 내용)의
    // 시작·끝이다.
    virtual Result<void> BeginStruct() noexcept = 0;
    virtual Result<void> EndStruct() noexcept = 0;
    // 필드 하나의 시작이다(실효 도구 이름 — ROCKS_PROPERTY(Name="…") 또는 C++ 이름). 뒤에 그
    // 필드의 값 하나가 온다. field는 그 필드의 스키마(이 호출 동안 유효 — bundle 목록 등 필드
    // 메타), flags는 유효 필드 플래그다(바깥 필드에서 전파된 EditorOnly 등을 포함한 경로의
    // 플래그 — 에셋 A4e2b bundle 태그 수집이 EditorOnly 경로를 거른다).
    virtual Result<void> Field(StringView name, const FieldSchema& field,
                               uint32 flags) noexcept = 0;
    // 단일 값이다(PathRead의 FieldValue 그대로 — enum은 정수, Text·참조도 FieldValue로). 값은 이
    // 호출 동안만 유효하다.
    virtual Result<void> Value(const FieldValue& value) noexcept = 0;
    // 값 없음이다(unset Optional·빈 InstancedStruct).
    virtual Result<void> Absent() noexcept = 0;
    // byte 배열(`Array<uint8>` 필드 통째)이다. 이 호출 동안만 유효하다.
    virtual Result<void> Bytes(Span<const uint8> bytes) noexcept = 0;
    virtual Result<void> BeginArray(usize count) noexcept = 0;
    virtual Result<void> EndArray() noexcept = 0;
    virtual Result<void> BeginSet(usize count) noexcept = 0;
    virtual Result<void> EndSet() noexcept = 0;
    virtual Result<void> BeginMap(usize count) noexcept = 0;
    // Map 항목의 키다(뒤에 그 항목의 값 하나가 온다). 이 호출 동안만 유효하다.
    virtual Result<void> MapKey(const FieldValue& key) noexcept = 0;
    virtual Result<void> EndMap() noexcept = 0;
    // InstancedStruct에 담긴 struct의 타입이다(toolName은 비어 있을 수 있다 — 수동 등록). 뒤에 그
    // struct가 온다.
    virtual Result<void> Instanced(TypeId type, StringView toolName) noexcept = 0;
};

// target의 값을 visitor에 넘긴다(필드는 스키마 필드 순서, Map 항목·Set 원소는 저장 순서 — 정렬은
// 소비자가 한다). visitor는 순회 중 대상을 바꾸지 않는다(계약 — 바꾸면 경로 연산 오류가 날 수
// 있다). options.use의 출력 제외 플래그(10.1절 표 — 경로의 어느 단계든)가 있는 필드는
// 넘기지 않는다(키 기록은 Report — Transient만 뺀다). 읽기는 경로 연산이라 대상을 바꾸지 않는다 —
// 대상의 소유 스레드에서 부른다.
//
// 오류: options.names가 대상 Registry의 pool과 다르면 InvalidArgument, 순회할 수 없는
// 필드(접근기 없는 Opaque — options.skipFieldsWithoutText면 건너뛴다) Unsupported, 경로 연산의
// 오류, visitor가 돌려준 오류, 할당 실패. 실패하면 visitor는 그 지점까지 받은 이벤트를 받은 상태다.
//
// 사용 예(설정 struct 값의 키 기록):
//   KeyVisitor visitor(writer);
//   TextCodecOptions options;
//   options.use = TextUse::Report;
//   const AssignmentTarget target =
//       AssignmentTarget::Value(registry, access, lease, &settings, allocator);
//   if (auto visited = VisitValues(target, visitor, options); !visited) {
//       ...
//   }
ROCKS_REFLECTIONTEXT_API Result<void> VisitValues(const AssignmentTarget& target,
                                                  ValueVisitor& visitor,
                                                  const TextCodecOptions& options) noexcept;

} // namespace rocks::reflection_text
