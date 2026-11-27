#pragma once

#include <Base/Types.h>
#include <Core/Optional.h>

// 창 IME 기록의 종류·용량·결과다(Docs/Design/UiToolkit 19.2절 — TextInputSession이 쓴다).
namespace rocks::platform {

enum class TextInputRecordKind : uint8 {
    Composition,
    Commit,
    End,
    // TSF 거울 문서의 편집을 그대로 옮기는 기록이다(Docs/Design/UiToolkit 22.13절). 위치는 기록을
    // 앞에서부터 적용한 뒤의 문맥 글자 기준이다.
    // 조합을 시작한다(이미 조합 중이면 아무 일도 없다).
    CompositionBegin,
    // [begin, begin + length)를 글자로 바꾼다. 조합 중이면 미리보기에, 아니면 확정 편집 하나다.
    TextReplace,
    // 조합 범위다(조합 중이 아니면 시작한다).
    CompositionRange,
    // 조합을 확정하고 끝낸다(조합 중이 아니면 아무 일도 없다).
    CompositionCommit,
    // 선택을 [begin, begin + length)로 옮긴다.
    Selection
};

// 남은 기록 수와 문자 용량이다(창 객체 안 고정 배열 — OS 콜백 안에서 할당하지 않는다).
inline constexpr usize MaxTextInputRecords = 64;
inline constexpr usize MaxTextInputCodeUnits = 4096;

struct TextInputRecordResult {
    // 표지에 실을 번호다. 활성 문맥이 IME를 받지 않으면 없다(창은 OS 기본 처리로 넘긴다).
    Optional<uint32> serial;
    // 넘쳐서 남은 기록을 모두 버리고 '끝' 하나를 넣었다. 창은 OS 조합을 취소하고 로그를 남긴다.
    bool overflowed = false;
};

} // namespace rocks::platform
