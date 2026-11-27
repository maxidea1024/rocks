#pragma once

#include <Base/Types.h>
#include <Core/Optional.h>

// 창 IME 기록의 종류·용량·결과다(Docs/Design/UiToolkit 19.2절 — TextInputSession이 쓴다).
namespace rocks::platform {

enum class TextInputRecordKind : uint8 {
    Composition,
    Commit,
    End
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
