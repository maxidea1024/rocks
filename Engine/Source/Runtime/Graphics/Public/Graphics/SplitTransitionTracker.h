#pragma once

#include <Core/Containers/Array.h>
#include <Graphics/ResourceAccess.h>

namespace rocks::graphics {

// 분리 전이 짝 표다(CommandContext::BeginTransitions·EndTransitions 계약, Device.h). backend가 기록
// 하나 동안 열린 짝을 들고, end가 begin과 같은 목록을 넘겼는지(개수·내용 지문) 대조하고, 기록 끝에
// 끝나지 않은 짝을 센다. 짝 번호는 장치 수명 동안 1부터 늘며 0(null)을 주지 않는다.
//
// 사용 예(backend의 end 지점 낮추기):
//   SplitTransition BeginTransitions(Span<const ResourceTransition> list) {
//       return recording_ ? splits_.Begin(list) : SplitTransition{};
//   }
//   void EndTransitions(SplitTransition split, Span<const ResourceTransition> list) {
//       const SplitEndResult ended = splits_.End(split, list);
//       if (ended == SplitEndResult::Unknown) { ++ignoredDraws_; }
//       if (ended == SplitEndResult::Mismatch && validation_) { ++validationErrors_; }
//       LowerTransitions(list);   // end 지점에서 한 번에 낮춘다
//   }
//   // EndFrame: validationErrors_ += splits_.Close();
//
// 스레드: 장치 잠금 안에서 쓴다(스레드 안전하지 않다).
enum class SplitEndResult : uint8 {
    // 열린 짝이었고 목록이 begin과 같다.
    Matched,
    // 열린 짝이었지만 목록이 begin과 다르다(계약 위반).
    Mismatch,
    // null 짝이다(begin이 null을 돌려줬다). end는 Transition과 같다.
    Null,
    // 모르는·이미 끝난 짝이다.
    Unknown
};

class ROCKS_GRAPHICS_API SplitTransitionTracker {
public:
    explicit SplitTransitionTracker(Allocator& allocator) noexcept;

    // 짝을 열고 목록의 지문을 남긴다. 빈 목록이나 짝 칸 할당 실패면 null이다(그때 end는
    // Transition과 같다).
    SplitTransition Begin(Span<const ResourceTransition> transitions) noexcept;

    // 짝을 닫는다.
    SplitEndResult End(SplitTransition split, Span<const ResourceTransition> transitions) noexcept;

    // 기록 끝이다. 끝나지 않은 짝 수를 돌려주고 모두 버린다.
    usize Close() noexcept;

    usize OpenCount() const noexcept {
        return open_.Size();
    }

    // 목록의 지문이다(개수와 내용 byte의 SipHash).
    static uint64 Fingerprint(Span<const ResourceTransition> transitions) noexcept;

private:
    struct Open {
        uint32 id = 0;
        uint32 count = 0;
        uint64 fingerprint = 0;
    };

    Array<Open> open_;
    uint32 serial_ = 0;
};

} // namespace rocks::graphics
