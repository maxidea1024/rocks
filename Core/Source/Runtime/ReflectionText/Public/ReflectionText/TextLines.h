#pragma once

#include <Core/Text/String.h>
#include <ReflectionText/TextAssignments.h>

// 리플렉션 값의 `<경로>=<값>` 줄 출력이다(Docs/Design/ReflectionExtensions.ko.md 10.4절 "asset-info
// 출력", Docs/Design/AssetSystem.ko.md 13절·7.1절 15 A2c3). 대상의 필드를 명령행 대입(asset-set
// 입력) 줄로 쓴다 — 출력을 그대로 대입 묶음(PrepareAssignments)에 넣으면 값이 보존된다(값 보존
// 왕복). JSON 출력(ExportJson)과 같은 순회다.
//
// 줄(10.4절 표의 명령행 열):
//   단일 값 필드 `f=v`, Optional unset `--unset=f`(set이면 내부 표기, Optional<struct>는 `f={}` 뒤
//   멤버 줄), struct 필드는 멤버 줄 `f.m=v`(멤버가 없으면 줄 없음), 단일 값 원소의
//   Array·StaticArray·Set은 쉼표 목록 한 줄(바이트 배열은 Base64), 단일 값의 Map은 `k:v` 목록 한
//   줄, struct 원소의 Array는 `f=` 뒤 원소마다 `f[i]={}`와 멤버 줄, struct 원소 StaticArray는
//   원소마다 멤버 줄, struct 값 Map은 `f=` 뒤 항목마다 `f[k]={}`와 멤버 줄, InstancedStruct는
//   `f=<타입 도구 이름>` 뒤 멤버 줄(비어 있으면 `f=none`), InstancedStruct 원소 Array·값 Map은
//   `f=` 뒤 `f[i]=<타입>`(또는 `f[k]=<타입>`)과 멤버 줄, 강한 참조(ObjectRef) 원소의 StaticArray는
//   원소마다 `f[i]=v`(통째 대입은 준비 단위가 없다). 각 줄은 '\n'으로 끝난다.
namespace rocks::reflection_text {

// target의 필드를 줄로 out 뒤에 붙인다(필드는 등록 스키마의 fields 순서 — 선언 순서와 다를 수
// 있으나 결정적이다, Map 항목·Set 원소는 삽입 순서).
// options.use의 출력 제외 플래그(10.1절 — 경로의 어느 단계든)가 있는 필드는 쓰지 않는다. 참조는
// options.resolver로 쓴다(ExportJson과 같다). 읽기만 한다 — 대상의 소유 스레드에서 부른다.
//
// 오류: 옵션·options.names가 대상 Registry의 pool과 다르면 InvalidArgument, 줄로 쓸 수 없는 값
// (줄바꿈(\n·\r)이 든 경로·값 텍스트 — JSON 출력을 쓴다, Localized Text, resolver가 쓰지 못한 값,
// 이름 없는 enum 값, Opaque 필드, 도구 이름 없는 struct를 담은 InstancedStruct, 경로 단계 상한을
// 넘는 자리)은 Unsupported, 경로 연산의 오류, 할당 실패. 실패하면 out에 앞부분이 남을 수 있다.
//
// 사용 예(asset-info):
//   String text(allocator);
//   TextCodecOptions options;
//   options.resolver = &assetResolver;
//   if (auto written = ExportLines(AssignmentTarget::Live(registry, access, asset), text, options);
//       !written) {
//       ...
//   }
Result<void> ExportLines(const AssignmentTarget& target, String& out,
                         const TextCodecOptions& options) noexcept;

} // namespace rocks::reflection_text
