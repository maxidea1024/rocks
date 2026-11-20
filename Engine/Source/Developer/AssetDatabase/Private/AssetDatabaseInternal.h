#pragma once

#include <Core/Containers/Span.h>
#include <Core/Result.h>
#include <Core/Text/StringBuilder.h>

// AssetDatabase 모듈 안에서 함께 쓰는 도우미다(Docs/Design/AssetSystem.ko.md 7.1절).
namespace rocks::asset_database_detail {

// pieces를 차례로 out 뒤에 붙인다. 처음 실패한 붙이기의 오류를 돌려준다(그때까지 붙인 것은 남는다 —
// 호출자는 실패하면 out을 버린다). 짧은 조각 여러 개를 한 실패 지점으로 묶는다.
inline Result<void> AppendAll(StringBuilderBase& out, Span<const StringView> pieces) noexcept {
    for (StringView piece : pieces) {
        if (auto appended = out.TryAppend(piece); !appended) {
            return appended;
        }
    }

    return Result<void>::Success();
}

} // namespace rocks::asset_database_detail
