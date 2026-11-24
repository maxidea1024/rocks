#pragma once

#include <Core/Text/String.h>
#include <Core/Text/StringBuilder.h>

namespace rocks {

// 진단 덤프(DumpTickGraph·DumpSubsystems·DumpGameplay — Docs/Design/EngineRuntime 22.1절)가 쓰는
// 도우미다. out 끝에 서식 문자열(StringBuilder 서식)을 붙인다. 한 번에 붙이는 조각이 256 byte
// 안이면 조각을 만드는 데 할당하지 않는다. 오류: 서식·할당 실패(out은 그대로).
//
// 사용 예:
//   auto written = AppendFormatted(out, "  {} priority={}\n", name, priority);
template <class... Args>
Result<void> AppendFormatted(String& out, StringView format, const Args&... args) noexcept {
    StringBuilder<256> piece(out.GetAllocator());
    if (auto formatted = piece.TryAppendFormat(format, args...); !formatted) {
        return formatted;
    }

    return out.TryAppend(piece.View());
}

} // namespace rocks
