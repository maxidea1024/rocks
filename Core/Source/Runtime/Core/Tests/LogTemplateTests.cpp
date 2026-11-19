#include <TestSupport/Test.h>
#include <Core/Diagnostics/LogCapture.h>
#include <Core/Memory/SystemAllocator.h>
#include <Core/Text/StringBuilder.h>
#include <Core/Time/DateTime.h>
#include <Platform/Clock.h>
#include <Platform/Memory.h>

using namespace rocks;

namespace {

struct Vector2i {
    int32 x = 0;
    int32 y = 0;
};

// spec "fail"이면 Formatter 자신의 오류를 낸다.
struct Failing {};

// count개의 'x'를 내는 큰 Formatter 값이다(슬롯을 넘겨 블록을 빌리게 한다).
struct Wide {
    usize count = 0;
};

enum class Color : uint8 {
    Red = 2
};

enum class Offset : int16 {
    Back = -3
};

// Formatter가 불린 횟수를 센다(캡처가 Formatter를 한 번만 부르는지 확인).
struct Counted {
    usize count = 0;
};

thread_local uint32 countedCalls = 0;

// Formatter 안에서 TryReserve를 부른다(잘림 모드 builder에서 실패하지 않아야 한다).
struct Reserving {};

// spec을 그대로 "[spec]"으로 낸다(미리 계산한 spec 위치 확인).
struct EchoSpec {};

// IsValidSpec이 "short"만 받는 사용자 Formatter 타입이다.
struct CheckedSpec {};

} // namespace

namespace rocks {

template <>
struct Formatter<Vector2i> {
    static Result<void> Format(StringBuilderBase& out, const Vector2i& value,
                               StringView spec) noexcept {
        if (!spec.IsEmpty()) {
            return Result<void>::Failure(ErrorCode::InvalidArgument);
        }

        return out.TryAppendFormat("({}, {})", value.x, value.y);
    }
};

template <>
struct Formatter<Failing> {
    static Result<void> Format(StringBuilderBase& out, const Failing&, StringView) noexcept {
        (void)out.TryAppend("partial");
        return Result<void>::Failure(ErrorCode::DependencyFailed);
    }
};

template <>
struct Formatter<Wide> {
    static Result<void> Format(StringBuilderBase& out, const Wide& value, StringView) noexcept {
        return out.TryAppendRepeated('x', value.count);
    }
};

template <>
struct Formatter<Counted> {
    static Result<void> Format(StringBuilderBase& out, const Counted& value, StringView) noexcept {
        ++countedCalls;
        return out.TryAppendRepeated('c', value.count);
    }
};

template <>
struct Formatter<Reserving> {
    static Result<void> Format(StringBuilderBase& out, const Reserving&, StringView) noexcept {
        auto reserved = out.TryReserve(4096);
        if (!reserved) {
            return reserved;
        }

        return out.TryAppend("reserved");
    }
};

template <>
struct Formatter<CheckedSpec> {
    static Result<void> Format(StringBuilderBase& out, const CheckedSpec&, StringView) noexcept {
        return out.TryAppend("checked");
    }

    static constexpr bool IsValidSpec(StringView spec) noexcept {
        return spec.IsEmpty() || spec == "short";
    }
};

template <>
struct Formatter<EchoSpec> {
    static Result<void> Format(StringBuilderBase& out, const EchoSpec&, StringView spec) noexcept {
        auto result = out.TryAppendAscii('[');
        if (result) {
            result = out.TryAppend(spec);
        }

        if (result) {
            result = out.TryAppendAscii(']');
        }

        return result;
    }
};

namespace test {
uint64 OperatorNewCalls() noexcept;
} // namespace test

namespace test {
uint32 CaptureLogFromProbeUnit(Span<uint8> buffer) noexcept;
} // namespace test

} // namespace rocks

namespace {

constexpr LogCategory LogTests{"Tests.Template"};

using detail::AnalyzeLogTemplate;
using detail::FormatArgKind;
using detail::LogTemplateLayout;

constexpr LogTemplateError Analyze(StringView text, const FormatArgKind* kinds,
                                   usize count) noexcept {
    LogTemplateLayout layout{};
    return AnalyzeLogTemplate(text, kinds, count, layout);
}

constexpr FormatArgKind Ints[] = {FormatArgKind::Signed, FormatArgKind::Signed,
                                  FormatArgKind::Signed};
constexpr FormatArgKind Texts[] = {FormatArgKind::Text};
constexpr FormatArgKind Doubles[] = {FormatArgKind::Double};
constexpr FormatArgKind Customs[] = {FormatArgKind::Custom};

// 템플릿 검증은 constexpr이라 컴파일 타임에 확인한다. 컴파일 실패 자체는 Rocks.ToolTests의
// Native.LogCompilerContracts가 확인한다.
static_assert(Analyze("{A} and {B}", Ints, 2) == LogTemplateError::None);
static_assert(Analyze("{} and {}", Ints, 2) == LogTemplateError::None);
static_assert(Analyze("{1} {0} {1}", Ints, 2) == LogTemplateError::None);
static_assert(Analyze("{{literal}} {A}", Ints, 1) == LogTemplateError::None);
static_assert(Analyze("no holes", Ints, 0) == LogTemplateError::None);
static_assert(Analyze("{A}", Ints, 2) == LogTemplateError::ArgumentCountMismatch);
static_assert(Analyze("{A} {B} {C}", Ints, 2) == LogTemplateError::ArgumentCountMismatch);
static_assert(Analyze("{0}", Ints, 2) == LogTemplateError::ArgumentCountMismatch);
static_assert(Analyze("{2}", Ints, 2) == LogTemplateError::ArgumentIndexOutOfRange);
static_assert(Analyze("{A} {}", Ints, 2) == LogTemplateError::MixedNamedAndPositional);
static_assert(Analyze("{0} {A}", Ints, 2) == LogTemplateError::MixedNamedAndPositional);
static_assert(Analyze("{} {0}", Ints, 2) == LogTemplateError::MixedAutomaticAndManualIndex);
static_assert(Analyze("{A} {A}", Ints, 2) == LogTemplateError::DuplicateHoleName);
static_assert(Analyze("{A", Ints, 1) == LogTemplateError::UnbalancedBrace);
static_assert(Analyze("A}", Ints, 0) == LogTemplateError::UnbalancedBrace);
static_assert(Analyze("{A-B}", Ints, 1) == LogTemplateError::InvalidHoleName);
static_assert(Analyze("{01}", Ints, 2) == LogTemplateError::InvalidArgumentIndex);
static_assert(Analyze("{A:.2f}", Ints, 1) == LogTemplateError::InvalidSpec);
static_assert(Analyze("{A:#x}", Ints, 1) == LogTemplateError::None);
static_assert(Analyze("{A:.2f}", Doubles, 1) == LogTemplateError::None);
static_assert(Analyze("{A:#}", Doubles, 1) == LogTemplateError::InvalidSpec);
static_assert(Analyze("{A:>8.3}", Texts, 1) == LogTemplateError::None);
static_assert(Analyze("{A:+}", Texts, 1) == LogTemplateError::InvalidSpec);
// 사용자 Formatter의 spec은 컴파일 타임에 보지 않는다(부분 검증).
static_assert(Analyze("{A:anything}", Customs, 1) == LogTemplateError::None);
// 사용자 Formatter 인자는 캡처 때 한 번만 렌더하므로 자리마다 spec이 같아야 한다.
static_assert(Analyze("{0:x} {0:x}", Customs, 1) == LogTemplateError::None);
static_assert(Analyze("{0} {0:x}", Customs, 1) == LogTemplateError::ConflictingFormatterSpec);
static_assert(Analyze("{0:x} {0:y}", Customs, 1) == LogTemplateError::ConflictingFormatterSpec);
static_assert(Analyze("{0:.1f} {0:.2f}", Doubles, 1) == LogTemplateError::None);

constexpr LogTemplateError AnalyzeChecked(StringView text) noexcept {
    constexpr detail::LogSpecCheck checks[] = {detail::LogSpecCheckOf<CheckedSpec>()};
    LogTemplateLayout layout{};
    return AnalyzeLogTemplate(text, Customs, 1, layout, checks);
}

// IsValidSpec이 있으면 사용자 Formatter spec도 검사한다(없으면 위처럼 통과).
static_assert(detail::LogSpecCheckOf<CheckedSpec>() != nullptr);
static_assert(detail::LogSpecCheckOf<EchoSpec>() == nullptr);
static_assert(detail::LogSpecCheckOf<int>() == nullptr);
static_assert(AnalyzeChecked("{A}") == LogTemplateError::None);
static_assert(AnalyzeChecked("{A:short}") == LogTemplateError::None);
static_assert(AnalyzeChecked("{A:long}") == LogTemplateError::InvalidFormatterSpec);
static_assert(detail::HasFormatterSpecCheck<DateTime> && Formatter<DateTime>::IsValidSpec("ms") &&
              !Formatter<DateTime>::IsValidSpec("s"));
static_assert(Analyze("{A}", Ints, MaxLogArguments + 1) == LogTemplateError::TooManyArguments);
static_assert(Analyze(StringView("\xff{A}", 4), Ints, 1) == LogTemplateError::InvalidUtf8);

// EventId는 원문 해시이며 컴파일 타임에 계산된다.
static_assert(LogTemplate<int>("count {Count}").EventId() == HashStringBytes("count {Count}"));

// 이 번역 단위의 두 번째 함수에서 캡처한다(위치 시험).
uint32 CaptureLogFromThisUnit(Span<uint8> buffer) noexcept {
    const uint32 line = __LINE__ + 1;
    const auto result = CaptureLog({buffer}, LogLevel::Info, LogTests, "this unit {Value}", 1);
    return result.IsOk() ? line : 0;
}

// 시험용 블록 provider다. Count개의 Bytes byte 블록을 빌려 주고 빌림·반납을 센다.
class TestBlocks final : public LogBlockProvider {
public:
    static constexpr uint32 Count = 2;
    static constexpr usize Bytes = 256;

    bool TryBorrow(LogBlock& block) noexcept override {
        for (uint32 index = 0; index < Count && index < limit; ++index) {
            if (!used[index]) {
                used[index] = true;
                ++borrowed;
                block.index = index;
                block.bytes = Span<uint8>(storage[index], Bytes);
                return true;
            }
        }

        return false;
    }

    usize BlockBytes() const noexcept override {
        return Bytes;
    }

    Span<const uint8> Resolve(uint32 index) const noexcept override {
        if (index >= Count || !used[index]) {
            return Span<const uint8>();
        }

        return Span<const uint8>(storage[index], Bytes);
    }

    void Release(uint32 index) noexcept override {
        if (index < Count && used[index]) {
            used[index] = false;
            ++released;
        }
    }

    uint8 storage[Count][Bytes]{};
    bool used[Count]{};
    uint32 limit = Count;
    uint32 borrowed = 0;
    uint32 released = 0;
};

// 계약을 어기고 BlockBytes보다 훨씬 작은(1 byte) 블록을 주는 provider다. 캡처는 그 블록을 돌려주고
// 블록 없이 진행해야 한다(Fatal로 가면 안 된다).
class TinyBlocks final : public LogBlockProvider {
public:
    usize BlockBytes() const noexcept override {
        return 256;
    }

    bool TryBorrow(LogBlock& block) noexcept override {
        ++borrowed;
        block.index = 0;
        block.bytes = Span<uint8>(storage, 1);
        return true;
    }

    Span<const uint8> Resolve(uint32) const noexcept override {
        return Span<const uint8>(storage, 1);
    }

    void Release(uint32) noexcept override {
        ++released;
    }

    uint8 storage[1]{};
    uint32 borrowed = 0;
    uint32 released = 0;
};

// 렌더 결과 영역을 함께 둔다.
struct Rendered {
    char message[512]{};
    char propertyText[128]{};
    LogProperty properties[MaxLogArguments];
    LogEvent event;

    Result<void> From(Span<const uint8> capture,
                      const LogBlockProvider* blocks = nullptr) noexcept {
        LogRenderTarget target{
            Span<char>(message), Span<char>(propertyText), Span<LogProperty>(properties), {}};
        return RenderLogCapture(capture, blocks, target, event);
    }
};

bool HasFlag(const LogEvent& event, LogEventFlags flag) noexcept {
    return HasAnyLogEventFlags(event.flags, flag);
}

} // namespace

TEST(FormatParserMatchesStringBuilderGrammar) {
    detail::FormatParser parser("a{{b}}c{0:>4}{1}", false);
    detail::FormatToken token;
    const char* const literals[] = {"a", "{", "b", "}", "c"};
    for (const char* literal : literals) {
        REQUIRE(parser.Next(token));
        CHECK(token.kind == detail::FormatTokenKind::Literal && token.text == literal);
    }

    REQUIRE(parser.Next(token));
    CHECK(token.kind == detail::FormatTokenKind::Hole && token.argument == 0 && token.hasSpec &&
          token.spec == ">4" && token.name.IsEmpty());
    REQUIRE(parser.Next(token));
    CHECK(token.argument == 1 && !token.hasSpec);
    CHECK(!parser.Next(token) && parser.Error() == detail::FormatParseError::None);

    // 이름 자리는 로그 템플릿 전용이다. TryAppendFormat은 지금처럼 거부한다.
    SystemAllocator allocator;
    StringBuilder<> builder(allocator);
    CHECK(builder.TryAppendFormat("{Name}", 1).Error() == ErrorCode::InvalidArgument);
    CHECK(builder.TryAppendFormat("{Name:x}", 1).Error() == ErrorCode::InvalidArgument);
    CHECK(builder.IsEmpty());
    REQUIRE(builder.TryAppendFormat("{{{}}}", 1).IsOk());
    CHECK(builder.View() == "{1}");
}

TEST(FixedStringBuilderTruncatesAtCodePointBoundary) {
    const uint64 attempts = detail::FixedStringBuilderAllocationAttempts();
    char storage[8];
    FixedStringBuilder text{Span<char>(storage)};
    CHECK(text.Capacity() == 7 && !text.Truncated());
    // "가나다"는 9 byte다. 7 byte에는 "가나"(6 byte)만 들어가고 "다"의 앞 byte는 버린다.
    REQUIRE(text.TryAppend("가나다").IsOk());
    CHECK(text.View() == "가나" && text.Truncated());
    CHECK(text.CString()[6] == '\0');
    // 잘린 뒤의 붙이기는 버린다(1 byte가 남아도 "x"를 뒤에 붙이지 않는다).
    REQUIRE(text.TryAppend("x").IsOk());
    REQUIRE(text.TryAppendFormat("{}", 5).IsOk());
    CHECK(text.View() == "가나");

    text.Clear();
    CHECK(!text.Truncated() && text.IsEmpty());
    REQUIRE(text.TryAppendFormat("{:>5}|{}", 1, 23456).IsOk());
    CHECK(text.View() == "    1|2" && text.Truncated());

    // 서식 오류는 잘림 모드에서도 실패하고 호출 전 내용·잘림 표시로 돌아간다.
    text.Clear();
    REQUIRE(text.TryAppend("ab").IsOk());
    CHECK(text.TryAppendFormat("{} {:q}", 12345678, 1).Error() == ErrorCode::InvalidArgument);
    CHECK(text.View() == "ab" && !text.Truncated());

    // 반복·이어 붙이기도 자른다. 자기 내용을 인자로 써도 된다(저장소가 움직이지 않는다).
    text.Clear();
    REQUIRE(text.TryAppendRepeated('-', 3).IsOk());
    REQUIRE(text.TryAppendFormat("{}", text.View()).IsOk());
    CHECK(text.View() == "------" && !text.Truncated());
    REQUIRE(text.TryAppendRepeated('-', 3).IsOk());
    CHECK(text.ByteCount() == 7 && text.Truncated());

    text.Clear();
    const StringView parts[] = {"ab", "cd", "ef"};
    REQUIRE(text.TryAppendJoin(Span<const StringView>(parts), ", ").IsOk());
    CHECK(text.View() == "ab, cd," && text.Truncated());

    // 사용자 Formatter도 그대로 쓴다. Formatter 자신의 오류는 그 서식 호출의 오류다.
    text.Clear();
    REQUIRE(text.TryAppendFormat("{}", Vector2i{1, 2}).IsOk());
    CHECK(text.View() == "(1, 2)");
    CHECK(text.TryAppendFormat("{}", Failing{}).Error() == ErrorCode::DependencyFailed);
    CHECK(text.View() == "(1, 2)");
    CHECK(detail::FixedStringBuilderAllocationAttempts() == attempts);
}

TEST(LogTemplateCarriesTextEventIdAndLocation) {
    const LogTemplate<int, double> text("{Count} at {Seconds:.1f}");
    CHECK(text.Text() == "{Count} at {Seconds:.1f}");
    CHECK(text.EventId() == HashStringBytes("{Count} at {Seconds:.1f}"));
    CHECK(StringView(text.Location().file).EndsWith("LogTemplateTests.cpp"));

    // 같은 원문의 런타임 템플릿은 EventId가 같다.
    const RuntimeTemplate runtime{StringView("{Count} at {Seconds:.1f}")};
    CHECK(runtime.EventId() == text.EventId() && runtime.Text() == text.Text());
}

TEST(LogCaptureRoundTripsNamedValues) {
    uint8 slot[512];
    const int32 count = 3;
    const auto captured =
        CaptureLog({Span<uint8>(slot), nullptr, 111, 222, 333}, LogLevel::Warning, LogTests,
                   "{Count} items {Ratio:.2f} {Name} {Flag} {Letter} {Shade} {Point} {Delta}",
                   count, 0.5, "abc", true, 'z', Color::Red, Vector2i{1, 2}, Offset::Back);
    REQUIRE(captured.IsOk());

    Rendered rendered;
    REQUIRE(rendered.From(Span<const uint8>(slot, captured.Value())).IsOk());
    const LogEvent& event = rendered.event;
    CHECK(event.message == "3 items 0.50 abc true z 2 (1, 2) -3");
    CHECK(event.flags == LogEventFlags::None);
    CHECK(event.level == LogLevel::Warning && event.category == "Tests.Template");
    CHECK(event.messageTemplate ==
          "{Count} items {Ratio:.2f} {Name} {Flag} {Letter} {Shade} {Point} {Delta}");
    CHECK(event.eventId == HashStringBytes(event.messageTemplate));
    CHECK(event.monotonicTicks == 111 && event.threadId == 222 && event.frame == 333);
    CHECK(event.monotonicNanoseconds ==
          detail::LogTicksToNanoseconds(111, platform::MonotonicTickFrequency()));
    REQUIRE(event.properties.Size() == 8);

    const LogProperty* p = event.properties.Data();
    CHECK(p[0].name == "Count" && p[0].kind == LogPropertyKind::Signed && p[0].signedValue == 3 &&
          p[0].text == "3");
    CHECK(p[1].name == "Ratio" && p[1].kind == LogPropertyKind::Float && p[1].floatValue == 0.5 &&
          p[1].text == "0.5");
    CHECK(p[2].name == "Name" && p[2].kind == LogPropertyKind::String && p[2].text == "abc");
    CHECK(p[3].name == "Flag" && p[3].kind == LogPropertyKind::Bool && p[3].boolValue &&
          p[3].text == "true");
    CHECK(p[4].name == "Letter" && p[4].kind == LogPropertyKind::String && p[4].text == "z");
    CHECK(p[5].name == "Shade" && p[5].kind == LogPropertyKind::Unsigned &&
          p[5].unsignedValue == 2);
    CHECK(p[6].name == "Point" && p[6].kind == LogPropertyKind::String && p[6].text == "(1, 2)");
    CHECK(p[7].name == "Delta" && p[7].kind == LogPropertyKind::Signed && p[7].signedValue == -3);

    // 캡처는 위치와 무관하다. 다른 버퍼로 복사해도 같은 결과다.
    uint8 moved[512];
    for (usize index = 0; index < captured.Value(); ++index) {
        moved[index] = slot[index];
    }

    for (usize index = 0; index < captured.Value(); ++index) {
        slot[index] = 0;
    }

    Rendered again;
    REQUIRE(again.From(Span<const uint8>(moved, captured.Value())).IsOk());
    CHECK(again.event.message == "3 items 0.50 abc true z 2 (1, 2) -3");
}

TEST(LogCapturePositionalNamesAndSpecialValues) {
    uint8 slot[512];
    const double infinity = platform::BitCast<double>(0x7FF0000000000000ull);
    const auto captured =
        CaptureLog({Span<uint8>(slot)}, LogLevel::Info, LogTests, "{1}/{0:x} {2} {3} {4}", 255u,
                   -1.5f, infinity, -infinity, static_cast<const void*>(nullptr));
    REQUIRE(captured.IsOk());
    Rendered rendered;
    REQUIRE(rendered.From(Span<const uint8>(slot, captured.Value())).IsOk());
    CHECK(rendered.event.message == "-1.5/ff inf -inf 0x0");
    const LogProperty* p = rendered.event.properties.Data();
    CHECK(p[0].name == "Arg0" && p[0].kind == LogPropertyKind::Unsigned &&
          p[0].unsignedValue == 255 && p[0].text == "255");
    CHECK(p[1].name == "Arg1" && p[1].kind == LogPropertyKind::Float && p[1].floatValue == -1.5);
    // 무한대·NaN은 JSON 숫자가 아니므로 문자열 속성이다.
    CHECK(p[2].kind == LogPropertyKind::String && p[2].text == "inf");
    CHECK(p[3].kind == LogPropertyKind::String && p[3].text == "-inf");
    CHECK(p[4].name == "Arg4" && p[4].kind == LogPropertyKind::String && p[4].text == "0x0");
}

TEST(LogCaptureRuntimeTemplateIsCopiedAndValidatedAtRender) {
    uint8 slot[512];
    char source[] = "copied {Value}";
    const auto captured = CaptureLog({Span<uint8>(slot)}, LogLevel::Info, LogTests,
                                     RuntimeTemplate{StringView(source)}, 42);
    REQUIRE(captured.IsOk());
    // 캡처 뒤 원문을 바꿔도 렌더는 캡처한 사본을 쓴다.
    source[0] = 'X';
    Rendered rendered;
    REQUIRE(rendered.From(Span<const uint8>(slot, captured.Value())).IsOk());
    CHECK(rendered.event.message == "copied 42");
    CHECK(rendered.event.messageTemplate == "copied {Value}");
    CHECK(rendered.event.eventId == HashStringBytes("copied {Value}"));
    CHECK(rendered.event.properties.Size() == 1 &&
          rendered.event.properties.Data()[0].name == "Value");

    // 잘못된 런타임 템플릿은 원문 + 오류 코드로 기록하고 실패하지 않는다.
    const auto broken = CaptureLog({Span<uint8>(slot)}, LogLevel::Info, LogTests,
                                   RuntimeTemplate{StringView("{A} {B}")}, 1);
    REQUIRE(broken.IsOk());
    Rendered error;
    REQUIRE(error.From(Span<const uint8>(slot, broken.Value())).IsOk());
    CHECK(error.event.message == "{A} {B} [template error: ArgumentCountMismatch]");
    CHECK(HasFlag(error.event, LogEventFlags::TemplateError));
    CHECK(error.event.properties.Size() == 1 && error.event.properties.Data()[0].name == "Arg0");

    const auto badSpec = CaptureLog({Span<uint8>(slot)}, LogLevel::Info, LogTests,
                                    RuntimeTemplate{StringView("{A:.3f}")}, 1);
    REQUIRE(badSpec.IsOk());
    Rendered spec;
    REQUIRE(spec.From(Span<const uint8>(slot, badSpec.Value())).IsOk());
    CHECK(spec.event.message == "{A:.3f} [template error: InvalidSpec]");
}

TEST(LogCaptureFormatterFailureAffectsOnlyThatHole) {
    uint8 slot[512];
    const char invalid[] = {'o', 'k', static_cast<char>(0xff), '\0'};
    const auto captured =
        CaptureLog({Span<uint8>(slot)}, LogLevel::Error, LogTests, "a={A} b={B} c={C} d={D}",
                   Failing{}, 5, static_cast<const char*>(nullptr), invalid);
    REQUIRE(captured.IsOk());
    Rendered rendered;
    REQUIRE(rendered.From(Span<const uint8>(slot, captured.Value())).IsOk());
    CHECK(rendered.event.message == "a=<format error> b=5 c=<format error> d=<format error>");
    CHECK(HasFlag(rendered.event, LogEventFlags::FormatError));
    CHECK(rendered.event.properties.Data()[0].text == "<format error>");
    CHECK(rendered.event.properties.Data()[1].signedValue == 5);
}

TEST(LogCaptureLargeValuesBorrowBlocksOrTruncate) {
    // 슬롯은 머리와 값 칸에 짧은 여유만 있다. 긴 문자열은 블록으로 간다.
    uint8 slot[LogCaptureMinimumBytes(2) + 8];
    char longText[200];
    for (usize index = 0; index < sizeof(longText) - 1; ++index) {
        longText[index] = static_cast<char>('a' + index % 26);
    }

    longText[sizeof(longText) - 1] = '\0';
    TestBlocks blocks;
    const auto captured = CaptureLog({Span<uint8>(slot), &blocks}, LogLevel::Info, LogTests,
                                     "{Text}|{Wide}", longText, Wide{100});
    REQUIRE(captured.IsOk());
    CHECK(blocks.borrowed == 2);
    Rendered rendered;
    REQUIRE(rendered.From(Span<const uint8>(slot, captured.Value()), &blocks).IsOk());
    CHECK(rendered.event.message.ByteCount() == 199 + 1 + 100);
    CHECK(rendered.event.message.StartsWith("abcdefghij", StringComparison::CaseSensitive));
    CHECK(!HasFlag(rendered.event, LogEventFlags::ValueTruncated));
    ReleaseLogCaptureBlocks(Span<const uint8>(slot, captured.Value()), blocks);
    CHECK(blocks.released == 2 && !blocks.used[0] && !blocks.used[1]);

    // 블록이 없으면 남은 영역(8 byte)에 UTF-8 경계로 잘라 담고 ValueTruncated를 세운다.
    blocks.limit = 0;
    uint8 small[LogCaptureMinimumBytes(1) + 8];
    const auto korean = CaptureLog({Span<uint8>(small), &blocks}, LogLevel::Info, LogTests,
                                   "{Text}", "가나다라마바");
    REQUIRE(korean.IsOk());
    Rendered truncated;
    REQUIRE(truncated.From(Span<const uint8>(small, korean.Value()), &blocks).IsOk());
    CHECK(HasFlag(truncated.event, LogEventFlags::ValueTruncated));
    CHECK(truncated.event.message == "가나");
    ReleaseLogCaptureBlocks(Span<const uint8>(small, korean.Value()), blocks);
    CHECK(blocks.released == 2);

    // 블록이 모자라면 Formatter 결과도 잘린다.
    const auto wide =
        CaptureLog({Span<uint8>(slot), &blocks}, LogLevel::Info, LogTests, "{Wide}", Wide{50});
    REQUIRE(wide.IsOk());
    Rendered wideRendered;
    REQUIRE(wideRendered.From(Span<const uint8>(slot, wide.Value()), &blocks).IsOk());
    CHECK(HasFlag(wideRendered.event, LogEventFlags::ValueTruncated));
    CHECK(wideRendered.event.message.ByteCount() < 50);

    // 머리와 값 칸이 들어가지 않으면 아무것도 담지 않고 실패한다.
    CHECK(CaptureLog({Span<uint8>(slot, LogCaptureMinimumBytes(1) - 1)}, LogLevel::Info, LogTests,
                     "{A}", 1)
              .Error() == ErrorCode::CapacityExceeded);
}

TEST(LogRenderTruncatesMessageAndRejectsCorruptCaptures) {
    uint8 slot[512];
    const auto captured = CaptureLog({Span<uint8>(slot)}, LogLevel::Info, LogTests, "{A} and {B}",
                                     "first value", "second value");
    REQUIRE(captured.IsOk());
    char message[10];
    char texts[4];
    LogProperty properties[1];
    LogEvent event;
    REQUIRE(RenderLogCapture(
                Span<const uint8>(slot, captured.Value()), nullptr,
                {Span<char>(message), Span<char>(texts), Span<LogProperty>(properties), {}}, event)
                .IsOk());
    CHECK(event.message == "first val");
    CHECK(HasFlag(event, LogEventFlags::MessageTruncated));
    CHECK(event.properties.Size() == 1);

    // 손상된 캡처(머리 표지)는 렌더하지 않는다.
    slot[0] = static_cast<uint8>(slot[0] ^ 0xffu);
    CHECK(RenderLogCapture(
              Span<const uint8>(slot, captured.Value()), nullptr,
              {Span<char>(message), Span<char>(texts), Span<LogProperty>(properties), {}}, event)
              .Error() == ErrorCode::InvalidArgument);
    CHECK(RenderLogCapture(
              Span<const uint8>(slot, 4), nullptr,
              {Span<char>(message), Span<char>(texts), Span<LogProperty>(properties), {}}, event)
              .Error() == ErrorCode::InvalidArgument);
}

namespace {

// 캡처 머리·값 칸을 고쳐 손상을 만든다(화이트 박스). 값 칸의 flags 1은 "블록에 있음"이다.
constexpr uint8 ValueInBlockFlag = 1;

struct CorruptCapture {
    uint8 bytes[512]{};
    usize size = 0;

    detail::LogCaptureHeader Header() const noexcept {
        detail::LogCaptureHeader header;
        platform::MemoryCopy(&header, bytes, sizeof(header));
        return header;
    }

    void SetHeader(const detail::LogCaptureHeader& header) noexcept {
        platform::MemoryCopy(bytes, &header, sizeof(header));
    }

    detail::LogCaptureValue Value(usize index) const noexcept {
        detail::LogCaptureValue value;
        platform::MemoryCopy(&value,
                             bytes + sizeof(detail::LogCaptureHeader) +
                                 index * sizeof(detail::LogCaptureValue),
                             sizeof(value));
        return value;
    }

    void SetValue(usize index, const detail::LogCaptureValue& value) noexcept {
        platform::MemoryCopy(bytes + sizeof(detail::LogCaptureHeader) +
                                 index * sizeof(detail::LogCaptureValue),
                             &value, sizeof(value));
    }

    ErrorCode Render(const LogBlockProvider* blocks = nullptr) const noexcept {
        Rendered rendered;
        return rendered.From(Span<const uint8>(bytes, size), blocks).Error();
    }
};

CorruptCapture MakeValidCapture() noexcept {
    CorruptCapture capture;
    const auto captured = CaptureLog({Span<uint8>(capture.bytes)}, LogLevel::Info, LogTests,
                                     "{Text} {Number}", "text", 7);
    capture.size = captured.IsOk() ? captured.Value() : 0;
    return capture;
}

} // namespace

TEST(LogRenderRejectsCorruptHeaderAndValueCells) {
    REQUIRE(MakeValidCapture().Render() == ErrorCode::None);

    CorruptCapture capture = MakeValidCapture();
    detail::LogCaptureValue text = capture.Value(0);
    text.payload = capture.size; // 텍스트 시작이 캡처 끝
    text.bytes = 1;
    capture.SetValue(0, text);
    CHECK(capture.Render() == ErrorCode::InvalidArgument);

    capture = MakeValidCapture();
    text = capture.Value(0);
    text.bytes = 1000; // 길이가 캡처를 넘는다
    capture.SetValue(0, text);
    CHECK(capture.Render() == ErrorCode::InvalidArgument);

    capture = MakeValidCapture();
    detail::LogCaptureHeader header = capture.Header();
    header.valueCount = static_cast<uint16>(MaxLogArguments + 1);
    capture.SetHeader(header);
    CHECK(capture.Render() == ErrorCode::InvalidArgument);

    capture = MakeValidCapture();
    detail::LogCaptureValue number = capture.Value(1);
    number.kind = static_cast<uint8>(detail::FormatArgKind::Custom) + 1;
    capture.SetValue(1, number);
    CHECK(capture.Render() == ErrorCode::InvalidArgument);

    capture = MakeValidCapture();
    header = capture.Header();
    header.level = static_cast<uint8>(LogLevelCount);
    capture.SetHeader(header);
    CHECK(capture.Render() == ErrorCode::InvalidArgument);

    capture = MakeValidCapture();
    header = capture.Header();
    header.categoryBytes = static_cast<uint8>(MaxLogCategoryNameBytes + 1);
    capture.SetHeader(header);
    CHECK(capture.Render() == ErrorCode::InvalidArgument);

    // 블록에 있다고 표시한 값의 블록을 provider가 모른다(빌려 준 적 없는 index).
    capture = MakeValidCapture();
    text = capture.Value(0);
    text.flags = ValueInBlockFlag;
    text.payload = 1;
    capture.SetValue(0, text);
    TestBlocks blocks;
    CHECK(capture.Render(&blocks) == ErrorCode::InvalidArgument);
    CHECK(capture.Render() == ErrorCode::InvalidArgument);
}

// 런타임 템플릿이 캡처 때 잘렸으면 해석하지 않는다(잘린 "{A"를 짝 없는 중괄호로 오판하지 않는다).
TEST(LogRenderTruncatedRuntimeTemplateIsNotATemplateError) {
    uint8 slot[LogCaptureMinimumBytes(1) + 8];
    const auto captured = CaptureLog({Span<uint8>(slot)}, LogLevel::Info, LogTests,
                                     RuntimeTemplate{StringView("value {A} and more")}, 1);
    REQUIRE(captured.IsOk());
    Rendered rendered;
    REQUIRE(rendered.From(Span<const uint8>(slot, captured.Value())).IsOk());
    CHECK(rendered.event.message == "value {A");
    CHECK(HasFlag(rendered.event, LogEventFlags::MessageTruncated));
    CHECK(HasFlag(rendered.event, LogEventFlags::ValueTruncated));
    CHECK(!HasFlag(rendered.event, LogEventFlags::TemplateError));
    CHECK(rendered.event.properties.Size() == 1 &&
          rendered.event.properties.Data()[0].name == "Arg0");
}

// 메시지 영역이 모자라도 오류 표기는 남고 원본이 잘린다.
TEST(LogRenderTemplateErrorKeepsSuffixWhenMessageIsFull) {
    uint8 slot[512];
    const auto captured = CaptureLog({Span<uint8>(slot)}, LogLevel::Info, LogTests,
                                     RuntimeTemplate{StringView("a long template {A} {B}")}, 1);
    REQUIRE(captured.IsOk());
    char message[48];
    char texts[16];
    LogProperty properties[1];
    LogEvent event;
    REQUIRE(RenderLogCapture(
                Span<const uint8>(slot, captured.Value()), nullptr,
                {Span<char>(message), Span<char>(texts), Span<LogProperty>(properties), {}}, event)
                .IsOk());
    CHECK(event.message.EndsWith(" [template error: ArgumentCountMismatch]",
                                 StringComparison::CaseSensitive));
    CHECK(event.message.StartsWith("a long", StringComparison::CaseSensitive));
    CHECK(HasFlag(event, LogEventFlags::TemplateError));
    CHECK(HasFlag(event, LogEventFlags::MessageTruncated));
}

// 캡처는 카테고리 객체가 아니라 이름(정적 문자열)을 담는다. 함수 지역 constexpr 카테고리도 렌더 때
// 유효하다(설계 8.3절).
usize CaptureWithLocalCategory(Span<uint8> buffer) noexcept {
    constexpr LogCategory LogLocal{"Tests.Local"};
    const auto captured = CaptureLog({buffer}, LogLevel::Info, LogLocal, "local {Value}", 3);
    return captured.IsOk() ? captured.Value() : 0;
}

TEST(LogCaptureKeepsCategoryNameNotObject) {
    uint8 slot[256];
    const usize used = CaptureWithLocalCategory(Span<uint8>(slot));
    REQUIRE(used != 0);
    Rendered rendered;
    REQUIRE(rendered.From(Span<const uint8>(slot, used)).IsOk());
    CHECK(rendered.event.category == "Tests.Local" && rendered.event.message == "local 3");
}

// Formatter는 한 번만 부른다. 남은 영역이 블록보다 작으면 블록에 먼저 렌더하고, 결과가 남은 영역에
// 들어가면 옮긴 뒤 블록을 돌려준다.
TEST(LogCaptureCallsFormatterOnce) {
    TestBlocks blocks;
    uint8 slot[LogCaptureMinimumBytes(1) + 64];
    countedCalls = 0;
    const auto fits =
        CaptureLog({Span<uint8>(slot), &blocks}, LogLevel::Info, LogTests, "{C}", Counted{40});
    REQUIRE(fits.IsOk());
    CHECK(countedCalls == 1);
    CHECK(blocks.borrowed == 1 && blocks.released == 1);
    Rendered small;
    REQUIRE(small.From(Span<const uint8>(slot, fits.Value()), &blocks).IsOk());
    CHECK(small.event.message.ByteCount() == 40);

    countedCalls = 0;
    const auto large =
        CaptureLog({Span<uint8>(slot), &blocks}, LogLevel::Info, LogTests, "{C}", Counted{200});
    REQUIRE(large.IsOk());
    CHECK(countedCalls == 1);
    Rendered big;
    REQUIRE(big.From(Span<const uint8>(slot, large.Value()), &blocks).IsOk());
    CHECK(big.event.message.ByteCount() == 200 &&
          !HasFlag(big.event, LogEventFlags::ValueTruncated));
    ReleaseLogCaptureBlocks(Span<const uint8>(slot, large.Value()), blocks);
    CHECK(blocks.borrowed == blocks.released);

    // 블록이 계약보다 작으면 돌려주고 남은 영역에 렌더한다(Fatal로 가지 않는다).
    TinyBlocks tiny;
    countedCalls = 0;
    const auto tinyResult =
        CaptureLog({Span<uint8>(slot), &tiny}, LogLevel::Info, LogTests, "{C}", Counted{100});
    REQUIRE(tinyResult.IsOk());
    CHECK(countedCalls == 1 && tiny.borrowed == 1 && tiny.released == 1);
    Rendered fallback;
    REQUIRE(fallback.From(Span<const uint8>(slot, tinyResult.Value())).IsOk());
    CHECK(fallback.event.message.ByteCount() == 63);
    CHECK(HasFlag(fallback.event, LogEventFlags::ValueTruncated));
}

// 리터럴 템플릿은 consteval로 계산한 spec 위치를, 런타임 템플릿은 캡처 때 찾은 spec을 Formatter에
// 준다.
TEST(LogCaptureGivesFormatterItsSpec) {
    uint8 slot[256];
    const auto literal = CaptureLog({Span<uint8>(slot)}, LogLevel::Info, LogTests,
                                    "{0:abc} and {0:abc}", EchoSpec{});
    REQUIRE(literal.IsOk());
    Rendered first;
    REQUIRE(first.From(Span<const uint8>(slot, literal.Value())).IsOk());
    CHECK(first.event.message == "[abc] and [abc]");

    const auto runtime = CaptureLog({Span<uint8>(slot)}, LogLevel::Info, LogTests,
                                    RuntimeTemplate{StringView("{Value:xy}")}, EchoSpec{});
    REQUIRE(runtime.IsOk());
    Rendered second;
    REQUIRE(second.From(Span<const uint8>(slot, runtime.Value())).IsOk());
    CHECK(second.event.message == "[xy]");

    // 런타임 템플릿에서 같은 Formatter 인자에 다른 spec을 쓰면 템플릿 오류다.
    const auto conflict = CaptureLog({Span<uint8>(slot)}, LogLevel::Info, LogTests,
                                     RuntimeTemplate{StringView("{0:a} {0:b}")}, EchoSpec{});
    REQUIRE(conflict.IsOk());
    Rendered third;
    REQUIRE(third.From(Span<const uint8>(slot, conflict.Value())).IsOk());
    CHECK(third.event.message == "{0:a} {0:b} [template error: ConflictingFormatterSpec]");

    // 런타임 템플릿에서도 IsValidSpec이 거부하면 템플릿 오류다(검사는 캡처 때, 표시는 렌더 때).
    const auto rejected = CaptureLog({Span<uint8>(slot)}, LogLevel::Info, LogTests,
                                     RuntimeTemplate{StringView("{Value:long}")}, CheckedSpec{});
    REQUIRE(rejected.IsOk());
    Rendered fourth;
    REQUIRE(fourth.From(Span<const uint8>(slot, rejected.Value())).IsOk());
    CHECK(fourth.event.message == "{Value:long} [template error: InvalidFormatterSpec]");
    CHECK(HasAnyLogEventFlags(fourth.event.flags, LogEventFlags::TemplateError));
    const auto accepted = CaptureLog({Span<uint8>(slot)}, LogLevel::Info, LogTests,
                                     RuntimeTemplate{StringView("{Value:short}")}, CheckedSpec{});
    REQUIRE(accepted.IsOk());
    Rendered fifth;
    REQUIRE(fifth.From(Span<const uint8>(slot, accepted.Value())).IsOk());
    CHECK(fifth.event.message == "checked");
}

// 잘림 모드 builder의 TryReserve는 할당하지 않고 성공한다. Formatter가 불러도 실패하지 않는다.
TEST(FixedStringBuilderTryReserveSucceedsWithoutAllocating) {
    const uint64 attempts = detail::FixedStringBuilderAllocationAttempts();
    char storage[6];
    FixedStringBuilder text{Span<char>(storage)};
    CHECK(text.TryReserve(100000).IsOk());
    REQUIRE(text.TryAppendFormat("{}", Reserving{}).IsOk());
    CHECK(text.View() == "reser" && text.Truncated());

    uint8 slot[256];
    const auto captured =
        CaptureLog({Span<uint8>(slot)}, LogLevel::Info, LogTests, "{R}", Reserving{});
    REQUIRE(captured.IsOk());
    Rendered rendered;
    REQUIRE(rendered.From(Span<const uint8>(slot, captured.Value())).IsOk());
    CHECK(rendered.event.message == "reserved");
    CHECK(detail::FixedStringBuilderAllocationAttempts() == attempts);
}

// 캡처·렌더는 힙을 쓰지 않는다. 두 가지 갈고리로 이 스레드의 할당 시도를 센다.
// - FixedStringBuilder의 거절 allocator(스레드별 카운터): builder가 저장소를 늘리려 하면 센다.
// - 이 시험 바이너리의 전역 operator new 대체(스레드별 카운터, OperatorNewCounter.cpp).
// 큰 값·잘림·Formatter(TryReserve 포함)·런타임 템플릿 경로를 모두 지나도 둘 다 0회여야 한다.
// 캡처·렌더 API는 Allocator를 받지 않으므로 Rocks allocator 경로는 구조상 없다.
TEST(LogCaptureAndRenderDoNotAllocate) {
    const uint64 attempts = detail::FixedStringBuilderAllocationAttempts();
    const uint64 news = test::OperatorNewCalls();
    TestBlocks blocks;
    uint8 slot[LogCaptureMinimumBytes(5) + 16];
    char longText[300];
    for (usize index = 0; index < sizeof(longText) - 1; ++index) {
        longText[index] = 'q';
    }

    longText[sizeof(longText) - 1] = '\0';
    for (uint32 round = 0; round < 3; ++round) {
        blocks.limit = round;
        const auto captured = CaptureLog({Span<uint8>(slot), &blocks}, LogLevel::Debug, LogTests,
                                         "{A} {B} {C} {D:.3e} {E}", longText, Vector2i{3, 4},
                                         Wide{400}, 2.5, Reserving{});
        REQUIRE(captured.IsOk());
        Rendered rendered;
        REQUIRE(rendered.From(Span<const uint8>(slot, captured.Value()), &blocks).IsOk());
        ReleaseLogCaptureBlocks(Span<const uint8>(slot, captured.Value()), blocks);

        const auto runtime = CaptureLog({Span<uint8>(slot), &blocks}, LogLevel::Debug, LogTests,
                                        RuntimeTemplate{StringView(longText)});
        REQUIRE(runtime.IsOk());
        REQUIRE(rendered.From(Span<const uint8>(slot, runtime.Value()), &blocks).IsOk());
        ReleaseLogCaptureBlocks(Span<const uint8>(slot, runtime.Value()), blocks);
    }

    CHECK(blocks.borrowed == blocks.released);
    CHECK(detail::FixedStringBuilderAllocationAttempts() == attempts);
    CHECK(test::OperatorNewCalls() == news);
}

// 리터럴 템플릿의 기본 인자 SourceLocation::Current()는 CaptureLog를 부른 줄을 담는다. 두 번역
// 단위·두 함수에서 확인한다.
TEST(LogCaptureLocationComesFromCallerInEachUnit) {
    uint8 here[256];
    uint8 there[256];
    const uint32 hereLine = CaptureLogFromThisUnit(Span<uint8>(here));
    const uint32 thereLine = test::CaptureLogFromProbeUnit(Span<uint8>(there));
    REQUIRE(hereLine != 0 && thereLine != 0);

    Rendered first;
    Rendered second;
    REQUIRE(first.From(Span<const uint8>(here)).IsOk());
    REQUIRE(second.From(Span<const uint8>(there)).IsOk());
    const SourceLocation a = first.event.location;
    const SourceLocation b = second.event.location;
    CHECK(StringView(a.file).EndsWith("LogTemplateTests.cpp"));
    CHECK(StringView(b.file).EndsWith("LogTemplateLocationProbe.cpp"));
    CHECK(a.line == hereLine && b.line == thereLine);
    CHECK(StringView(a.function) == "CaptureLogFromThisUnit");
    CHECK(StringView(b.function) == "CaptureLogFromProbeUnit");
    CHECK(first.event.message == "this unit 1" && second.event.message == "probe unit 7");
    CHECK(second.event.category == "Tests.LocationProbe");
}
