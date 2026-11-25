#pragma once

#include <Base/Guid.h>
#include <Core/Result.h>
#include <Core/Text/Name.h>
#include <Core/Text/StringView.h>

// 노드 그래프의 정규 순서다(Docs/Design/NodeGraph 6.1·6.3절).
//
// - Guid(GraphId·NodeId·주석 id)는 high, low의 부호 없는 사전 순이다.
// - Name(DomainId·NodeTypeId·PinKey)은 원문 순이다. None이 맨 앞이고, 그다음은 번호 접미를 뗀
//   base를 ASCII 소문자로 바꾼 바이트 사전 순, 같으면 번호 순(번호 없음이 먼저)이다. Name 같음
//   규칙(NameTextEquals — ASCII 대소문자 무시, 번호 접미)과 일치한다. 비교 결과가 0이면 같은
//   Name이다.
//
// 토큰 값(Name::id 등)은 실행마다 다르므로 순서에 쓰지 않는다. 같은 내용이면 같은 저장 바이트가
// 되게 하려는 것이다.
namespace rocks::nodegraph {

// Guid 두 개를 정규 순서로 비교한다. left가 앞이면 음수, 같으면 0, 뒤면 양수다.
constexpr int32 CompareGuid(const Guid& left, const Guid& right) noexcept {
    if (left.high != right.high) {
        return left.high < right.high ? -1 : 1;
    }

    if (left.low != right.low) {
        return left.low < right.low ? -1 : 1;
    }

    return 0;
}

// Name 원문 두 개를 정규 순서로 비교한다(위 규칙). 원문은 Name 표시 문자열이다(NameLease::View).
// 잘못된 UTF-8은 검사하지 않고 바이트 규칙대로 비교한다. 할당하지 않는다.
//
// 사용 예:
//   CompareNameText("Alpha", "ALPHA");      // 0(같은 Name)
//   CompareNameText("", "Alpha");           // 음수(None이 앞)
//   CompareNameText("Pin_2", "Pin_10");     // 음수(base가 같으면 번호 순)
ROCKS_NODEGRAPH_API int32 CompareNameText(StringView left, StringView right) noexcept;

// 토큰 두 개를 원문으로 비교한다. 같은 토큰(operator==)이면 pool을 보지 않고 0이다. 그 밖에는
// names에서 두 원문을 읽는다.
//
// 오류: names의 Acquire 오류(StaleHandle·WrongContext·ShuttingDown 등)와 토큰 구조
// 오류(InvalidArgument). 스레드: names의 병행 규칙을 따른다.
ROCKS_NODEGRAPH_API Result<int32> CompareNames(NamePool& names, Name left, Name right) noexcept;

} // namespace rocks::nodegraph
