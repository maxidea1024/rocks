#pragma once

#include <Base/TypeTraits.h>
#include <Base/Types.h>
#include <Core/Containers/Span.h>
#include <Core/Text/StringView.h>
#include <TraceCapture/TraceEventType.h>

namespace rocks {

// 기록 하나의 가변 필드 내용이다. 형의 가변 필드(String·배열)마다 필드 표 순서대로 하나씩 넘긴다.
// bytes는 그 필드 원소 크기의 배수여야 한다(배열 원소 수 = bytes / 원소 크기). 내용은 기록 호출
// 동안만 빌리고 복사한다.
struct TraceVariablePart {
    const void* data = nullptr;
    usize bytes = 0;

    static TraceVariablePart FromString(StringView text) noexcept {
        return {text.Data(), text.ByteCount()};
    }

    template <typename T>
    static TraceVariablePart FromSpan(Span<const T> values) noexcept {
        static_assert(IsTriviallyCopyableV<T>, "trace array elements must be trivially copyable");
        return {values.Data(), values.ByteCount()};
    }
};

// 기록 호출의 결과다. 기록 지점은 보통 결과를 무시한다(손실은 세션이 세고 스트림에 남긴다).
enum class TraceWriteResult : uint8 {
    // 스트림에 들어갔다(정의 기록은 정의 버퍼에 들어갔다).
    Recorded,
    // 채널이 꺼졌거나, 세션이 없거나, 형이 등록되지 않아 아무것도 하지 않았다.
    Disabled,
    // 블록 풀이 상한에 닿았거나(스레드 기록), 정의 버퍼가 찼거나(정의 기록), 기록이 크기 상한을
    // 넘어 버렸다. 기다리지 않는다(결정 T1). 손실은 세션 통계와 손실 기록에 남는다.
    Lost,
};

namespace detail {

// TraceWrite의 느린 경로다. 채널이 켜져 있을 때만 부른다.
ROCKS_TRACECAPTURE_API TraceWriteResult
TraceWriteRecord(const TraceEventType& type, const void* fixed, usize fixedBytes,
                 Span<const TraceVariablePart> variable) noexcept;

} // namespace detail

// 이벤트 하나를 현재 trace 세션에 기록한다(Docs/Design/TraceSystem.ko.md 4.1·4.2절).
//
//   FrameBegin event{};
//   event.tick = platform::MonotonicTicks();
//   event.frame = frameNumber;
//   const TraceVariablePart parts[] = {TraceVariablePart::FromString(label)};
//   TraceWrite(FrameBeginType, event, parts);
//
// - 채널이 꺼져 있으면 relaxed 읽기와 분기 하나로 끝난다(Disabled).
// - 스레드 기록은 이 스레드의 블록에 잠금 없이 이어 쓴다. 이 스레드의 첫 기록만 세션에 스레드를
//   등록하느라 잠금 하나를 잡는다.
// - 블록을 받지 못하면 기다리지 않고 버린다(Lost). 다음에 블록을 받으면 먼저 손실 기록(채널별 수,
//   손실 시작·끝 tick)을 남긴다.
// - Definition 형은 세션의 정의 버퍼에 쓴다. 버퍼가 차 있으면 버리고 센다(Lost).
//
// 인자: fixed는 형의 고정부 struct이고 sizeof(Fixed)가 형의 FixedBytes와 같아야 한다. variable은
// 형의 가변 필드 수만큼이어야 한다. 어긋나면 프로그래밍 오류로 Fatal이다. 스레드: 어느 스레드에서나
// 부를 수 있다. 같은 스레드에서 기록 도중 다시 기록하면(할당기 hook 등) 안쪽 기록은 Lost다.
template <typename Fixed>
TraceWriteResult TraceWrite(const TraceEventType& type, const Fixed& fixed,
                            Span<const TraceVariablePart> variable = {}) noexcept {
    static_assert(IsTriviallyCopyableV<Fixed>, "trace fixed part must be trivially copyable");
    if (!type.Channel().IsEnabled()) {
        return TraceWriteResult::Disabled;
    }

    return detail::TraceWriteRecord(type, &fixed, sizeof(Fixed), variable);
}

// 고정부가 없는 형(FixedBytes 0)의 기록이다.
inline TraceWriteResult TraceWriteVariable(const TraceEventType& type,
                                           Span<const TraceVariablePart> variable = {}) noexcept {
    if (!type.Channel().IsEnabled()) {
        return TraceWriteResult::Disabled;
    }

    return detail::TraceWriteRecord(type, nullptr, 0, variable);
}

} // namespace rocks
