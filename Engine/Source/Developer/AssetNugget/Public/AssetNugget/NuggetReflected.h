#pragma once

#include <AssetNugget/NuggetKey.h>
#include <ReflectionText/TextAssignments.h>

// 리플렉션 값의 너겟 키 기록이다(Docs/Design/AssetSystem.ko.md 9.1절 결정 2). 처리기는 설정을
// 손으로 쓰지 않고 이것으로 쓴다 — 필드마다 Field(이름) + 태그 + 값이라 필드를 빠뜨리거나, 같은
// 타입 필드를 맞바꾸거나, 이름만 바꿔도 키가 바뀐다.
namespace rocks {

// target(설정 struct 값·객체)의 값을 writer에 쓴다(ReflectionText VisitValues 순회 — Report 용도,
// Transient만 뺀다). 인코딩:
//   struct        Struct, (Field 이름, 값)*, End
//   단일 값       Bool / I64(부호 정수·enum) / U64(부호 없는 정수) / F32 / F64 / Guid(Guid·AssetId)
//                 / Digest / String(String·Name 표시 문자열)
//   ClassRef      Guid(클래스 TypeId — null은 null Guid)
//   Soft 참조     Guid(AssetId) + String(객체 경로)
//   unset Optional·빈 InstancedStruct  None
//   InstancedStruct  Guid(담긴 struct TypeId) + struct
//   배열          List(n) + 원소 n개        byte 배열 Bytes
//   Set           Set(n) + 원소(정규 인코딩 byte 사전순)
//   Map           Map(n) + (키, 값)(키의 정규 인코딩 byte 사전순)
//
// 오류: 키에 쓸 수 없는 값(ObjectRef·WeakObjectRef — 실행 중 handle, Text — 지역화 원문이 바뀔 수
// 있다, 접근기 없는 Opaque)은 Unsupported, 순회(VisitValues)의 오류, Name 조회 실패, 할당 실패.
// 실패하면 writer는 그 오류 상태다(Finish가 같은 오류를 돌려준다).
//
// 사용 예:
//   const AssignmentTarget target =
//       AssignmentTarget::Value(registry, access, settingsLease, &settings, allocator);
//   if (auto written = WriteReflected(writer, target); !written) { ... }
Result<void> WriteReflected(NuggetKeyWriter& writer,
                            const reflection_text::AssignmentTarget& target) noexcept;

} // namespace rocks
