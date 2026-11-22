#pragma once

#include <Base/TypeTraits.h>
#include <Base/Types.h>
#include <Core/Text/StringView.h>
#include <TraceCapture/TraceChannel.h>
#include <TraceCapture/TraceFormat.h>

namespace rocks {

// 표준 채널 "mark"(표지)·"span"(시간 띠)·"frame"(프레임 경계)이다. 세션이 Start 때 등록하며
// 처음에는 꺼져 있다.
ROCKS_TRACECAPTURE_API extern TraceChannel TraceMarkChannel;
ROCKS_TRACECAPTURE_API extern TraceChannel TraceSpanChannel;
ROCKS_TRACECAPTURE_API extern TraceChannel TraceFrameChannel;

namespace detail {
struct TraceMarkSiteAccess;
} // namespace detail

// 표지의 호출 지점 정의다(형식 문자열·파일·줄). 정의 기록은 세션마다 그 지점의 첫 표지 바로 전에
// 한 번 나가고, 표지 기록은 tick·호출 지점 id·인자만 담는다. 보통 ROCKS_TRACE_MARK가 함수 안
// static으로 만든다(constexpr 생성자, 상수 초기화). 문자열은 정적 수명이어야 한다.
class TraceMarkSite {
public:
    constexpr TraceMarkSite(const char* format, const char* file, uint32 line) noexcept
        : format_(format), file_(file), line_(line) {
    }

    TraceMarkSite(const TraceMarkSite&) = delete;
    TraceMarkSite& operator=(const TraceMarkSite&) = delete;

    const char* Format() const noexcept {
        return format_;
    }

    const char* File() const noexcept {
        return file_;
    }

    uint32 Line() const noexcept {
        return line_;
    }

private:
    friend struct detail::TraceMarkSiteAccess;

    const char* format_;
    const char* file_;
    uint32 line_;
    // (세션 serial 하위 32비트 << 32) | 호출 지점 id.
    uint64 cache_ = 0;
};

// 표지 인자를 TraceStandardFormat의 tag 형식으로 모은다. 정수는 64비트로 넓히고, 실수는 double,
// 문자열은 길이와 byte다. 합이 MaxMarkArgumentBytes를 넘으면 넘친 인자부터 버리고 Truncated를 켠다.
class ROCKS_TRACECAPTURE_API TraceMarkArguments {
public:
    template <typename T>
    void Add(const T& value) noexcept {
        if constexpr (IsSameV<T, bool>) {
            AddBool(value);
        } else if constexpr (IsIntegralV<T> && IsSignedV<T>) {
            AddWord(TraceStandardFormat::MarkArgumentInt64, static_cast<uint64>(value));
        } else if constexpr (IsIntegralV<T>) {
            AddWord(TraceStandardFormat::MarkArgumentUInt64, static_cast<uint64>(value));
        } else if constexpr (IsFloatingPointV<T>) {
            AddFloat(static_cast<double>(value));
        } else {
            static_assert(IsConvertibleV<const T&, StringView>,
                          "trace mark arguments must be integers, floats, bools or strings");
            AddString(StringView(value));
        }
    }

    const uint8* Data() const noexcept {
        return bytes_;
    }

    usize Size() const noexcept {
        return size_;
    }

    bool Truncated() const noexcept {
        return truncated_;
    }

private:
    void AddBool(bool value) noexcept;
    void AddWord(uint8 tag, uint64 value) noexcept;
    void AddFloat(double value) noexcept;
    void AddString(StringView value) noexcept;

    uint8 bytes_[TraceStandardFormat::MaxMarkArgumentBytes] = {};
    usize size_ = 0;
    bool truncated_ = false;
};

namespace detail {
ROCKS_TRACECAPTURE_API void EmitTraceMark(TraceMarkSite& site,
                                          const TraceMarkArguments& arguments) noexcept;
} // namespace detail

// 한 시점의 이름 있는 사건을 남긴다(UE TRACE_BOOKMARK 대응, Docs/Design/TraceSystem.ko.md 6.3절).
// 형식 문자열의 자리표시(`{}`)는 분석기가 인자로 채운다. 채널 "mark"가 꺼져 있으면 인자를 모으지
// 않는다.
//
//   ROCKS_TRACE_MARK("Level loaded: {} ({} ms)", levelName, milliseconds);
template <typename... Arguments>
void TraceMark(TraceMarkSite& site, const Arguments&... arguments) noexcept {
    if (!TraceMarkChannel.IsEnabled()) {
        return;
    }

    TraceMarkArguments encoded;
    (encoded.Add(arguments), ...);
    detail::EmitTraceMark(site, encoded);
}

// 스레드와 무관한 이름 있는 시간 구간(시간 띠)의 식별 값이다. 기록하지 않은 띠는 value 0이다.
struct TraceSpanId {
    uint64 value = 0;
    uint64 serial = 0;
};

namespace detail {
ROCKS_TRACECAPTURE_API TraceSpanId BeginTraceSpan(StringView name) noexcept;
ROCKS_TRACECAPTURE_API void EndTraceSpan(TraceSpanId span) noexcept;
} // namespace detail

// 시간 띠를 연다(UE region 대응, 6.3절). 다른 스레드에서 닫아도 된다. 시작과 끝은 돌려받은 id로만
// 짝지으므로 같은 이름의 띠가 겹쳐도 모호하지 않다. 채널 "span"이 꺼져 있으면 기록하지 않고 빈 id를
// 돌려준다. 이름은 세션 이름 표(CPU 구간과 같은 표)로 간다.
//
//   const TraceSpanId loading = TraceBeginSpan("Streaming.Load");
//   ...                                               // 다른 스레드에서
//   TraceEndSpan(loading);
inline TraceSpanId TraceBeginSpan(StringView name) noexcept {
    return TraceSpanChannel.IsEnabled() ? detail::BeginTraceSpan(name) : TraceSpanId{};
}

// 연 세션이 그대로일 때만 끝을 기록한다(빈 id·끝난 세션의 id는 아무것도 하지 않는다). 채널을 그사이
// 꺼도 끝은 낸다(쌍이 깨지지 않는다).
inline void TraceEndSpan(TraceSpanId span) noexcept {
    if (span.value != 0) {
        detail::EndTraceSpan(span);
    }
}

// 프레임 종류다. 렌더 프레임 번호는 RenderBase 프레임 slot 세대와 같은 값을 쓴다.
enum class TraceFrameKind : uint8 {
    Game = 0,
    Render = 1,
};

namespace detail {
ROCKS_TRACECAPTURE_API void WriteTraceFrame(bool begin, TraceFrameKind kind, uint64 frame) noexcept;
} // namespace detail

// 프레임 경계(종류별 시작·끝 tick과 프레임 번호)다(6.3절). 채널 "frame"이 꺼져 있으면 분기 하나다.
inline void TraceBeginFrame(TraceFrameKind kind, uint64 frame) noexcept {
    if (TraceFrameChannel.IsEnabled()) {
        detail::WriteTraceFrame(true, kind, frame);
    }
}

inline void TraceEndFrame(TraceFrameKind kind, uint64 frame) noexcept {
    if (TraceFrameChannel.IsEnabled()) {
        detail::WriteTraceFrame(false, kind, frame);
    }
}

} // namespace rocks

// 호출 지점마다 정적 표지 정의를 두고 표지를 남긴다. format은 문자열 리터럴이다.
#define ROCKS_TRACE_MARK(format, ...)                                                              \
    do {                                                                                           \
        static ::rocks::TraceMarkSite rocksTraceMarkSite{format, __FILE__, __LINE__};              \
        ::rocks::TraceMark(rocksTraceMarkSite __VA_OPT__(, ) __VA_ARGS__);                         \
    } while (false)
