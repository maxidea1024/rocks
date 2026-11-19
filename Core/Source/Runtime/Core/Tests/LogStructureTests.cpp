#include <TestSupport/Test.h>
#include <Core/Diagnostics/Log.h>
#include <Core/Diagnostics/LogCapture.h>
#include <Core/Diagnostics/LogLayout.h>
#include <Core/Diagnostics/LogStructure.h>
#include <Core/Diagnostics/LogSystem.h>
#include <Core/Diagnostics/RuntimeLogSink.h>
#include <Core/Memory/SystemAllocator.h>
#include <Core/Text/Json.h>
#include <Core/Text/StringBuilder.h>
#include <Platform/Atomics.h>
#include <Platform/Memory.h>
#include <Platform/Process.h>
#include <TestSupport/ProcessProbe.h>
#include <string.h>

// 로그 구조체 펼치기 "{@Name}"(LG16, Docs/Design/LoggingSystem.ko.md 14.1.6절) 시험이다: 템플릿
// 문법·검증, 캡처 시점 복사(원본을 바꾸거나 지워도 출력 불변), 깊이·필드·원소 상한의 정확한 경계,
// 공간 고갈, 블록 경로, 텍스트·JSON 결정적 출력과 노드 트리, 런타임 템플릿 오류, Sync·Manual·Async
// 모드의 같은 출력, Start 뒤 무할당. 컴파일 실패(특수화 없는 타입 등)는 Rocks.ToolTests의
// Native.LogCompilerContracts가, 리플렉션 일반 특수화는 ObjectRuntime 시험이 확인한다.
using namespace rocks;

namespace rocks::test {
uint64 OperatorNewCalls() noexcept;
bool RegisterProcessProbe(const char* mode, int (*function)(int, char**) noexcept) noexcept;
} // namespace rocks::test

namespace {

constexpr LogCategory LogStructureTests{"Tests.Structure"};

struct Point {
    int32 x = 0;
    int32 y = 0;
};

enum class Mode : uint8 {
    Fast = 2
};

// Formatter도 LogStructure도 없는 필드 타입이다(펼친 값 안에서는 "<unsupported>").
struct Opaque {
    int32 hidden = 0;
};

// Formatter로만 렌더되는 필드 타입이다.
struct Grid {
    int32 width = 0;
    int32 height = 0;
};

// Formatter가 실패하는 필드 타입이다.
struct Broken {};

struct Player {
    StringView name;
    int32 hp = 0;
    bool alive = false;
    float speed = 0.0f;
    Point position;
    int32 scores[3]{};
    Mode mode = Mode::Fast;
    const char* note = nullptr;
    Opaque opaque;
    Grid grid;
};

// 필드가 없는 struct다.
struct Empty {};

// level부터 limit까지 자기 자신을 중첩한다(깊이 상한 시험).
struct Deep {
    int32 level = 1;
    int32 limit = 1;
};

// count개의 필드(F0…)를 쓴다(필드 상한 시험).
struct Fields {
    usize count = 0;
};

// count개의 원소를 가진 배열 필드 하나다(원소 상한 시험).
struct Elements {
    int32 values[MaxLogStructureElements + 1]{};
    usize count = 0;
};

// Point 배열을 담는다(구조체 배열 시험).
struct Route {
    Point points[3];
};

// fields개의 필드마다 원소 MaxLogStructureElements개 배열을 둔다(노드 칸 넘침 시험).
struct Wide {
    usize fields = 0;
};

// 문자열 하나를 count번 반복한 필드를 둔다(공간 고갈 시험).
struct Long {
    StringView text;
    usize count = 0;
};

// API를 잘못 쓰는 특수화다(닫지 않은 객체·배열, 배열 원소의 이름).
struct Misused {};

// Formatter가 실패하는 필드와 그 뒤 필드다.
struct WithBroken {
    Broken broken;
    int32 after = 9;
};

// levels단 중첩 배열 하나를 쓴다(배열도 깊이 한 단계로 센다). 가장 안쪽에 정수 하나가 있다.
struct ArrayNest {
    usize levels = 1;
};

// 객체 안 배열 안의 긴 문자열이다(공간 고갈이 조상까지 잘림으로 보이는지).
struct DeepText {
    StringView text;
};

// Formatter와 LogStructure를 함께 가진 타입이다("{A}"는 Formatter, "{@A}"는 펼치기).
struct Both {
    int32 value = 0;
};

constexpr LogStaticName FieldNames[] = {
    "F0",  "F1",  "F2",  "F3",  "F4",  "F5",  "F6",  "F7",  "F8",  "F9",  "F10",
    "F11", "F12", "F13", "F14", "F15", "F16", "F17", "F18", "F19", "F20", "F21",
    "F22", "F23", "F24", "F25", "F26", "F27", "F28", "F29", "F30", "F31", "F32"};

static_assert(sizeof(FieldNames) / sizeof(FieldNames[0]) == MaxLogStructureFields + 1,
              "one name per field up to one over the limit");

} // namespace

namespace rocks {

template <>
struct LogStructure<Point> {
    static StringView TypeTag() noexcept {
        return "Point";
    }

    static void Write(LogStructureWriter& out, const Point& value) noexcept {
        out.Write("X", value.x);
        out.Write("Y", value.y);
    }
};

template <>
struct Formatter<Grid> {
    static Result<void> Format(StringBuilderBase& out, const Grid& value, StringView) noexcept {
        return out.TryAppendFormat("({}, {})", value.width, value.height);
    }
};

template <>
struct Formatter<Broken> {
    static Result<void> Format(StringBuilderBase& out, const Broken&, StringView) noexcept {
        (void)out.TryAppend("partial");
        return Result<void>::Failure(ErrorCode::DependencyFailed);
    }
};

template <>
struct LogStructure<Player> {
    static StringView TypeTag() noexcept {
        return "Player";
    }

    static void Write(LogStructureWriter& out, const Player& value) noexcept {
        out.Write("Name", value.name);
        out.Write("Hp", value.hp);
        out.Write("Alive", value.alive);
        out.Write("Speed", value.speed);
        out.Write("Pos", value.position);
        out.Write("Scores", value.scores);
        out.Write("Mode", value.mode);
        out.Write("Note", value.note);
        out.Write("Opaque", value.opaque);
        out.Write("Grid", value.grid);
    }
};

template <>
struct LogStructure<Empty> {
    static StringView TypeTag() noexcept {
        return "Empty";
    }

    static void Write(LogStructureWriter&, const Empty&) noexcept {
    }
};

template <>
struct LogStructure<Deep> {
    static StringView TypeTag() noexcept {
        return "Deep";
    }

    static void Write(LogStructureWriter& out, const Deep& value) noexcept {
        out.Write("Level", value.level);
        if (value.level < value.limit) {
            const Deep child{value.level + 1, value.limit};
            out.Write("Child", child);
        }
    }
};

// 태그 없는 특수화다(TypeTag 없음).
template <>
struct LogStructure<Fields> {
    static void Write(LogStructureWriter& out, const Fields& value) noexcept {
        for (usize index = 0; index < value.count; ++index) {
            out.Write(FieldNames[index], static_cast<int32>(index));
        }
    }
};

template <>
struct LogStructure<Elements> {
    static void Write(LogStructureWriter& out, const Elements& value) noexcept {
        out.WriteElements("Values", value.values, value.count);
    }
};

template <>
struct LogStructure<Route> {
    static void Write(LogStructureWriter& out, const Route& value) noexcept {
        out.Write("Points", value.points);
    }
};

template <>
struct LogStructure<Wide> {
    static void Write(LogStructureWriter& out, const Wide& value) noexcept {
        const int32 values[MaxLogStructureElements]{};
        for (usize index = 0; index < value.fields; ++index) {
            out.Write(FieldNames[index], values);
        }
    }
};

template <>
struct LogStructure<Long> {
    static void Write(LogStructureWriter& out, const Long& value) noexcept {
        for (usize index = 0; index < value.count; ++index) {
            out.Write(FieldNames[index], value.text);
        }
    }
};

template <>
struct LogStructure<Misused> {
    static void Write(LogStructureWriter& out, const Misused&) noexcept {
        out.Write("A", 1);
        // 닫지 않은 객체·배열은 캡처가 닫는다.
        (void)out.BeginObject("Open");
        out.Write("B", 2);
        (void)out.BeginArray("List", 1);
        out.Write("ignored", 3);
    }
};

template <>
struct LogStructure<ArrayNest> {
    static void Write(LogStructureWriter& out, const ArrayNest& value) noexcept {
        // 바깥 배열에는 이름이 있고 안쪽 배열은 원소다. 배열마다 원소가 하나다.
        if (out.BeginArray("L", 1)) {
            Nest(out, value.levels - 1);
        }

        out.EndArray();
    }

    static void Nest(LogStructureWriter& out, usize remaining) noexcept {
        if (remaining == 0) {
            out.Write({}, 1);
            return;
        }

        if (out.BeginArray({}, 1)) {
            Nest(out, remaining - 1);
        }

        out.EndArray();
    }
};

template <>
struct LogStructure<DeepText> {
    static void Write(LogStructureWriter& out, const DeepText& value) noexcept {
        out.Write("A", 1);
        if (out.BeginObject("Inner")) {
            if (out.BeginArray("List", 1)) {
                out.Write({}, value.text);
            }

            out.EndArray();
        }

        out.EndObject();
    }
};

template <>
struct LogStructure<WithBroken> {
    static void Write(LogStructureWriter& out, const WithBroken& value) noexcept {
        out.Write("Broken", value.broken);
        out.Write("After", value.after);
    }
};

template <>
struct LogStructure<Both> {
    static void Write(LogStructureWriter& out, const Both& value) noexcept {
        out.Write("Value", value.value);
    }
};

template <>
struct Formatter<Both> {
    static Result<void> Format(StringBuilderBase& out, const Both& value, StringView) noexcept {
        return out.TryAppendFormat("both:{}", value.value);
    }
};

} // namespace rocks

namespace {

using detail::AnalyzeLogTemplate;
using detail::FormatArgKind;
using detail::LogTemplateLayout;

constexpr FormatArgKind Customs[] = {FormatArgKind::Custom, FormatArgKind::Custom};
constexpr uint8 Structurable[] = {detail::LogArgStructurable, detail::LogArgStructurable};
constexpr uint8 FormattableOnly[] = {detail::LogArgFormattable, detail::LogArgFormattable};
constexpr uint8 StructureOnly[] = {detail::LogArgStructurable};
constexpr uint8 Mixed[] = {detail::LogArgStructurable, detail::LogArgFormattable};

constexpr LogTemplateError AnalyzeWith(StringView text, const uint8* traits,
                                       usize count = 1) noexcept {
    LogTemplateLayout layout{};
    return AnalyzeLogTemplate(text, Customs, count, layout, nullptr, traits);
}

constexpr bool StructuredAt(StringView text, usize index) noexcept {
    LogTemplateLayout layout{};
    (void)AnalyzeLogTemplate(text, Customs, 2, layout, nullptr, Structurable);
    return layout.structured[index];
}

// 문법과 검증은 constexpr이라 컴파일 타임에 확인한다.
static_assert(AnalyzeWith("{@P}", Structurable) == LogTemplateError::None);
static_assert(AnalyzeWith("{@P} and {Q}", Mixed, 2) == LogTemplateError::None);
static_assert(StructuredAt("{@P} {Q}", 0) && !StructuredAt("{@P} {Q}", 1));
static_assert(AnalyzeWith("{@P:x}", Structurable) == LogTemplateError::SpecOnStructure);
static_assert(AnalyzeWith("{@P:}", Structurable) == LogTemplateError::SpecOnStructure);
static_assert(AnalyzeWith("{@P}", FormattableOnly) ==
              LogTemplateError::StructureWithoutLogStructure);
static_assert(AnalyzeWith("{P}", StructureOnly) == LogTemplateError::StructureArgumentWithoutAt);
static_assert(AnalyzeWith("{}", StructureOnly) == LogTemplateError::StructureArgumentWithoutAt);
static_assert(AnalyzeWith("{@}", Structurable) == LogTemplateError::InvalidHoleName);
static_assert(AnalyzeWith("{@0}", Structurable) == LogTemplateError::InvalidHoleName);
static_assert(AnalyzeWith("{@@P}", Structurable) == LogTemplateError::InvalidHoleName);
static_assert(AnalyzeWith("{@", Structurable) == LogTemplateError::UnbalancedBrace);
static_assert(AnalyzeWith("{@P} {@P}", Structurable, 2) == LogTemplateError::DuplicateHoleName);
static_assert(AnalyzeWith("{@P} {P}", Structurable, 2) == LogTemplateError::DuplicateHoleName);
static_assert(AnalyzeWith("{@P} {}", Structurable, 2) == LogTemplateError::MixedNamedAndPositional);
// 펼치기 자리의 spec은 traits 없이도(렌더 경로) 오류다.
static_assert(AnalyzeWith("{@P:x}", nullptr) == LogTemplateError::SpecOnStructure);

// 리터럴 템플릿이 "{@Name}" 자리 인자를 비트로 남긴다.
static_assert(LogTemplate<Point>("{@P}").StructureMask() == 1u);
static_assert(LogTemplate<int, Point>("{A} {@P}").StructureMask() == 2u);
static_assert(LogTemplate<Both, Both>("{A} {@B}").StructureMask() == 2u);
static_assert(LogTemplate<int>("{A}").StructureMask() == 0u);

// 성질 비트: LogStructure만 가진 타입은 "{@Name}" 전용이다.
static_assert(detail::LogArgTraitsOf<Point>() == detail::LogArgStructurable);
static_assert(detail::LogArgTraitsOf<Both>() ==
              (detail::LogArgStructurable | detail::LogArgFormattable));
static_assert(detail::LogArgTraitsOf<int>() == detail::LogArgFormattable);
static_assert(HasLogStructure<Point> && !HasLogStructure<Opaque> && !HasLogStructure<int>);
static_assert(detail::LogStructureWriteOf<Point>() != nullptr &&
              detail::LogStructureWriteOf<int>() == nullptr);

// 시험용 블록 provider다. Count개의 Bytes byte 블록을 빌려 주고 빌림·반납을 센다.
class TestBlocks final : public LogBlockProvider {
public:
    static constexpr uint32 Count = 2;
    static constexpr usize Bytes = 1024;

    usize BlockBytes() const noexcept override {
        return Bytes;
    }

    bool TryBorrow(LogBlock& block) noexcept override {
        for (uint32 index = 0; index < Count; ++index) {
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
    uint32 borrowed = 0;
    uint32 released = 0;
};

// 렌더 결과 영역을 함께 둔다(펼친 값 노드 칸 포함).
struct Rendered {
    char message[4096]{};
    char propertyText[2048]{};
    LogProperty properties[MaxLogArguments];
    LogProperty nodes[MaxLogStructureNodes];
    LogEvent event;

    Result<void> From(Span<const uint8> capture, const LogBlockProvider* blocks = nullptr,
                      usize nodeCount = MaxLogStructureNodes) noexcept {
        LogRenderTarget target{Span<char>(message),
                               Span<char>(propertyText),
                               Span<LogProperty>(properties),
                               {},
                               0,
                               Span<LogProperty>(nodes, nodeCount)};
        return RenderLogCapture(capture, blocks, target, event);
    }
};

bool HasFlag(const LogEvent& event, LogEventFlags flag) noexcept {
    return HasAnyLogEventFlags(event.flags, flag);
}

// 기록 하나를 JSON lines로 렌더해 문서로 읽는다.
struct JsonRecord {
    char line[JsonLinesLogSink::BufferBytes]{};
    usize bytes = 0;

    static Result<void> Write(void* raw, StringView view) noexcept {
        auto& self = *static_cast<JsonRecord*>(raw);
        if (view.ByteCount() > sizeof(self.line) - self.bytes) {
            return Result<void>::Failure(ErrorCode::DependencyFailed);
        }

        platform::MemoryCopy(self.line + self.bytes, view.Data(), view.ByteCount());
        self.bytes += view.ByteCount();
        return Result<void>::Success();
    }

    static Result<void> Flush(void*) noexcept {
        return Result<void>::Success();
    }

    StringView View() const noexcept {
        return StringView(line, bytes);
    }
};

Player MakePlayer() noexcept {
    Player player;
    player.name = "A\"nn\n";
    player.hp = 30;
    player.alive = true;
    player.speed = 1.5f;
    player.position = Point{1, -2};
    player.scores[0] = 1;
    player.scores[1] = 2;
    player.scores[2] = 3;
    player.grid = Grid{3, 4};
    return player;
}

constexpr StringView PlayerText = "Player { Name: \"A\\\"nn\\n\", Hp: 30, Alive: true, Speed: 1.5, "
                                  "Pos: Point { X: 1, Y: -2 }, Scores: [1, 2, 3], Mode: 2, "
                                  "Note: null, Opaque: <unsupported>, Grid: \"(3, 4)\" }";

// 직접 자식 하나를 이름으로 찾는다(없으면 nullptr).
const LogProperty* Child(const LogEvent& event, const LogProperty& node, StringView name) noexcept {
    for (uint32 index = node.subtreeBegin; index < node.subtreeBegin + node.subtreeCount;
         index += LogPropertySubtreeSpan(event.structureNodes[index])) {
        if (event.structureNodes[index].name == name) {
            return &event.structureNodes[index];
        }
    }

    return nullptr;
}

} // namespace

// 일반 서식 파서와 로그 레이아웃은 '@'를 받지 않는다(로그 템플릿 전용).
TEST(LogStructureHoleIsAcceptedOnlyInLogTemplates) {
    detail::FormatParser structured("{@Player}", true, true);
    detail::FormatToken token;
    REQUIRE(structured.Next(token));
    CHECK(token.kind == detail::FormatTokenKind::Hole && token.structured &&
          token.name == "Player" && token.text == "{@Player}");

    detail::FormatParser named("{@Player}", true);
    CHECK(!named.Next(token) && named.Error() == detail::FormatParseError::InvalidHoleName);
    detail::FormatParser plain("{@Player}", false, true);
    CHECK(!plain.Next(token) && plain.Error() == detail::FormatParseError::InvalidHoleName);

    char storage[64];
    FixedStringBuilder text{Span<char>(storage)};
    CHECK(text.TryAppendFormat("{@0}", 1).Error() == ErrorCode::InvalidArgument);
    CHECK(text.TryAppendFormat("{@}", 1).Error() == ErrorCode::InvalidArgument);
    CHECK(!LogLayout::Parse("{@Message}").IsOk());
}

// 캡처 → 렌더: 텍스트는 Serilog 모양, 속성 text는 메시지 안 구간, 노드 트리는 앞 순서다.
TEST(LogStructureRendersSerilogShapeAndNodeTree) {
    const Player player = MakePlayer();
    uint8 slot[2048];
    const auto used = CaptureLog({Span<uint8>(slot)}, LogLevel::Info, LogStructureTests,
                                 "Spawned {@Player} at {Frame}", player, 7);
    REQUIRE(used.IsOk());
    Rendered rendered;
    REQUIRE(rendered.From(Span<const uint8>(slot, used.Value())).IsOk());
    const LogEvent& event = rendered.event;
    char expected[512];
    FixedStringBuilder expectedText{Span<char>(expected)};
    REQUIRE(expectedText.TryAppendFormat("Spawned {} at 7", PlayerText).IsOk());
    CHECK(event.message == expectedText.View());
    CHECK(!HasFlag(event, LogEventFlags::ValueTruncated | LogEventFlags::MessageTruncated |
                              LogEventFlags::FormatError));

    REQUIRE(event.properties.Size() == 2);
    const LogProperty& top = event.properties[0];
    CHECK(top.name == "Player" && top.kind == LogPropertyKind::Structure &&
          top.typeTag == "Player" && !top.truncated && top.text == PlayerText);
    CHECK(event.properties[1].name == "Frame" && event.properties[1].signedValue == 7);
    // 맨 위 필드 10개 + Pos 아래 2개 + Scores 아래 3개다.
    CHECK(top.subtreeBegin == 0 && top.subtreeCount == 15);
    CHECK(event.structureNodes.Size() == 15);

    const LogProperty* name = Child(event, top, "Name");
    REQUIRE(name != nullptr);
    CHECK(name->kind == LogPropertyKind::String && name->text == "A\"nn\n" &&
          name->parent == NoLogPropertyParent);
    const LogProperty* speed = Child(event, top, "Speed");
    REQUIRE(speed != nullptr);
    CHECK(speed->kind == LogPropertyKind::Float && speed->floatValue == 1.5 &&
          speed->text == "1.5");
    const LogProperty* position = Child(event, top, "Pos");
    REQUIRE(position != nullptr);
    CHECK(position->kind == LogPropertyKind::Structure && position->typeTag == "Point" &&
          position->subtreeCount == 2 && position->text.IsEmpty());
    const uint32 positionIndex = static_cast<uint32>(position - event.structureNodes.Data());
    const LogProperty* y = Child(event, *position, "Y");
    REQUIRE(y != nullptr);
    CHECK(y->kind == LogPropertyKind::Signed && y->signedValue == -2 && y->text == "-2" &&
          y->parent == positionIndex);
    const LogProperty* scores = Child(event, top, "Scores");
    REQUIRE(scores != nullptr);
    CHECK(scores->kind == LogPropertyKind::Array && scores->unsignedValue == 3 &&
          !scores->truncated && scores->subtreeCount == 3);
    CHECK(event.structureNodes[scores->subtreeBegin + 2].signedValue == 3 &&
          event.structureNodes[scores->subtreeBegin + 2].name.IsEmpty());
    const LogProperty* mode = Child(event, top, "Mode");
    REQUIRE(mode != nullptr);
    CHECK(mode->kind == LogPropertyKind::Unsigned && mode->unsignedValue == 2);
    CHECK(Child(event, top, "Note")->kind == LogPropertyKind::Null);
    CHECK(Child(event, top, "Opaque")->text == "<unsupported>");
    CHECK(Child(event, top, "Grid")->text == "(3, 4)");
}

// 캡처는 값을 복사한다: 캡처 뒤 원본을 바꾸거나 지워도(덮어써도) 렌더 결과가 같다.
TEST(LogStructureCopiesValuesAtCapture) {
    // Player는 trivially copyable이라 byte 복사로 저장소에 둔다(나중에 저장소째 덮어쓴다).
    static_assert(IsTriviallyCopyableV<Player>, "the test overwrites the player storage");
    char nameBytes[16] = "Ann";
    Player source = MakePlayer();
    source.name = StringView(nameBytes, 3);
    alignas(Player) uint8 storage[sizeof(Player)];
    platform::MemoryCopy(storage, &source, sizeof(Player));
    const Player* player = reinterpret_cast<const Player*>(storage);
    uint8 slot[2048];
    const auto used =
        CaptureLog({Span<uint8>(slot)}, LogLevel::Info, LogStructureTests, "{@P}", *player);
    REQUIRE(used.IsOk());
    Rendered before;
    REQUIRE(before.From(Span<const uint8>(slot, used.Value())).IsOk());
    char first[1024];
    FixedStringBuilder copy{Span<char>(first)};
    REQUIRE(copy.TryAppend(before.event.message).IsOk());

    // 원본 파괴: 저장소와 이름 byte를 덮어쓴다.
    constexpr uint8 Garbage = 0xCD;
    platform::MemorySet(storage, Garbage, sizeof(storage));
    platform::MemorySet(&source, Garbage, sizeof(source));
    platform::MemorySet(nameBytes, 'X', sizeof(nameBytes));
    Rendered after;
    REQUIRE(after.From(Span<const uint8>(slot, used.Value())).IsOk());
    CHECK(after.event.message == copy.View());
    CHECK(after.event.message.StartsWith("Player { Name: \"Ann\", Hp: 30",
                                         StringComparison::CaseSensitive));
}

// 깊이 상한: 맨 위가 1이고 MaxLogStructureDepth까지는 그대로, 하나 더 깊으면 "{ ... }"와 잘림이다.
TEST(LogStructureDepthLimitIsExact) {
    uint8 slot[2048];
    const Deep atLimit{1, static_cast<int32>(MaxLogStructureDepth)};
    auto used = CaptureLog({Span<uint8>(slot)}, LogLevel::Info, LogStructureTests, "{@D}", atLimit);
    REQUIRE(used.IsOk());
    Rendered exact;
    REQUIRE(exact.From(Span<const uint8>(slot, used.Value())).IsOk());
    CHECK(exact.event.message ==
          "Deep { Level: 1, Child: Deep { Level: 2, Child: Deep { Level: 3, Child: Deep { Level: "
          "4 } } } }");
    CHECK(!HasFlag(exact.event, LogEventFlags::ValueTruncated));
    CHECK(!exact.event.properties[0].truncated);

    const Deep overLimit{1, static_cast<int32>(MaxLogStructureDepth) + 1};
    used = CaptureLog({Span<uint8>(slot)}, LogLevel::Info, LogStructureTests, "{@D}", overLimit);
    REQUIRE(used.IsOk());
    Rendered over;
    REQUIRE(over.From(Span<const uint8>(slot, used.Value())).IsOk());
    CHECK(over.event.message == "Deep { Level: 1, Child: Deep { Level: 2, Child: Deep { Level: 3, "
                                "Child: Deep { Level: 4, Child: Deep { ... } } } } }");
    CHECK(HasFlag(over.event, LogEventFlags::ValueTruncated));
    // 잘린 객체 노드는 자식 없이 잘림 표시만 가진다.
    const LogProperty& last = over.event.structureNodes[over.event.structureNodes.Size() - 1];
    CHECK(last.kind == LogPropertyKind::Structure && last.truncated && last.subtreeCount == 0 &&
          last.typeTag == "Deep" && last.name == "Child");
}

// 필드 상한: MaxLogStructureFields개는 그대로, 하나 넘으면 그 필드를 버리고 ", ..."다.
TEST(LogStructureFieldLimitIsExact) {
    uint8 slot[4096];
    const Fields atLimit{MaxLogStructureFields};
    auto used = CaptureLog({Span<uint8>(slot)}, LogLevel::Info, LogStructureTests, "{@F}", atLimit);
    REQUIRE(used.IsOk());
    Rendered exact;
    REQUIRE(exact.From(Span<const uint8>(slot, used.Value())).IsOk());
    CHECK(exact.event.message.StartsWith("{ F0: 0, F1: 1,", StringComparison::CaseSensitive));
    CHECK(exact.event.message.EndsWith("F31: 31 }", StringComparison::CaseSensitive));
    CHECK(!HasFlag(exact.event, LogEventFlags::ValueTruncated));
    CHECK(exact.event.properties[0].subtreeCount == MaxLogStructureFields &&
          exact.event.properties[0].typeTag.IsEmpty());

    const Fields overLimit{MaxLogStructureFields + 1};
    used = CaptureLog({Span<uint8>(slot)}, LogLevel::Info, LogStructureTests, "{@F}", overLimit);
    REQUIRE(used.IsOk());
    Rendered over;
    REQUIRE(over.From(Span<const uint8>(slot, used.Value())).IsOk());
    CHECK(over.event.message.EndsWith("F31: 31, ... }", StringComparison::CaseSensitive));
    CHECK(!over.event.message.Contains("F32", StringComparison::CaseSensitive));
    CHECK(HasFlag(over.event, LogEventFlags::ValueTruncated));
    CHECK(over.event.properties[0].truncated &&
          over.event.properties[0].subtreeCount == MaxLogStructureFields);
}

// 원소 상한: MaxLogStructureElements개는 그대로, 넘으면 앞 원소와 전체 개수다. 빈 배열은 "[]"다.
TEST(LogStructureElementLimitIsExactAndKeepsTotalCount) {
    uint8 slot[2048];
    Elements elements;
    for (usize index = 0; index < MaxLogStructureElements + 1; ++index) {
        elements.values[index] = static_cast<int32>(index);
    }

    elements.count = MaxLogStructureElements;
    auto used =
        CaptureLog({Span<uint8>(slot)}, LogLevel::Info, LogStructureTests, "{@E}", elements);
    REQUIRE(used.IsOk());
    Rendered exact;
    REQUIRE(exact.From(Span<const uint8>(slot, used.Value())).IsOk());
    CHECK(exact.event.message == "{ Values: [0, 1, 2, 3, 4, 5, 6, 7] }");
    CHECK(!HasFlag(exact.event, LogEventFlags::ValueTruncated));

    elements.count = MaxLogStructureElements + 1;
    used = CaptureLog({Span<uint8>(slot)}, LogLevel::Info, LogStructureTests, "{@E}", elements);
    REQUIRE(used.IsOk());
    Rendered over;
    REQUIRE(over.From(Span<const uint8>(slot, used.Value())).IsOk());
    CHECK(over.event.message == "{ Values: [0, 1, 2, 3, 4, 5, 6, 7, ... 9 items] }");
    CHECK(HasFlag(over.event, LogEventFlags::ValueTruncated));
    const LogProperty& array = over.event.structureNodes[0];
    CHECK(array.kind == LogPropertyKind::Array && array.truncated && array.unsignedValue == 9 &&
          array.subtreeCount == MaxLogStructureElements);

    elements.count = 0;
    used = CaptureLog({Span<uint8>(slot)}, LogLevel::Info, LogStructureTests, "{@E}", elements);
    REQUIRE(used.IsOk());
    Rendered empty;
    REQUIRE(empty.From(Span<const uint8>(slot, used.Value())).IsOk());
    CHECK(empty.event.message == "{ Values: [] }");
}

// 구조체 배열과 빈 struct, LogStructure가 있는 Formatter 타입의 두 자리.
TEST(LogStructureNestedArraysEmptyStructAndBothHoles) {
    uint8 slot[2048];
    Route route;
    route.points[1] = Point{1, 2};
    route.points[2] = Point{3, 4};
    auto used = CaptureLog({Span<uint8>(slot)}, LogLevel::Info, LogStructureTests, "{@P} {@E}",
                           route, Empty{});
    REQUIRE(used.IsOk());
    Rendered rendered;
    REQUIRE(rendered.From(Span<const uint8>(slot, used.Value())).IsOk());
    CHECK(rendered.event.message == "{ Points: [Point { X: 0, Y: 0 }, Point { X: 1, Y: 2 }, "
                                    "Point { X: 3, Y: 4 }] } Empty {}");
    // Route 노드: 배열 1 + Point 3 × (객체 1 + 필드 2) = 10, Empty는 자식 없음.
    CHECK(rendered.event.properties[0].subtreeCount == 10);
    CHECK(rendered.event.properties[1].typeTag == "Empty" &&
          rendered.event.properties[1].subtreeCount == 0 &&
          rendered.event.properties[1].subtreeBegin == 10);
    const LogProperty& second = rendered.event.structureNodes[4];
    CHECK(second.kind == LogPropertyKind::Structure && second.parent == 0 && second.name.IsEmpty());

    const Both both{5};
    used =
        CaptureLog({Span<uint8>(slot)}, LogLevel::Info, LogStructureTests, "{A} {@B}", both, both);
    REQUIRE(used.IsOk());
    Rendered pair;
    REQUIRE(pair.From(Span<const uint8>(slot, used.Value())).IsOk());
    CHECK(pair.event.message == "both:5 { Value: 5 }");
    CHECK(pair.event.properties[0].kind == LogPropertyKind::String &&
          pair.event.properties[1].kind == LogPropertyKind::Structure);
}

// Formatter 오류 필드는 그 값만 "<format error>"이고 기록에 FormatError가 선다. API를 잘못
// 써도(닫지 않은 객체·배열, 배열 원소의 이름) 인코딩은 올바르다.
TEST(LogStructureFormatterFailureAndMisuse) {
    uint8 slot[1024];
    auto used =
        CaptureLog({Span<uint8>(slot)}, LogLevel::Info, LogStructureTests, "{@H}", WithBroken{});
    REQUIRE(used.IsOk());
    Rendered broken;
    REQUIRE(broken.From(Span<const uint8>(slot, used.Value())).IsOk());
    CHECK(broken.event.message == "{ Broken: <format error>, After: 9 }");
    CHECK(HasFlag(broken.event, LogEventFlags::FormatError));
    CHECK(!HasFlag(broken.event, LogEventFlags::ValueTruncated));
    CHECK(broken.event.structureNodes[0].text == "<format error>");

    used = CaptureLog({Span<uint8>(slot)}, LogLevel::Info, LogStructureTests, "{@M}", Misused{});
    REQUIRE(used.IsOk());
    Rendered misused;
    REQUIRE(misused.From(Span<const uint8>(slot, used.Value())).IsOk());
    CHECK(misused.event.message == "{ A: 1, Open: { B: 2, List: [3] } }");

    // 맨 위 밖의 쓰기와 두 번째 맨 위 객체는 버린다.
    uint8 direct[128];
    LogStructureWriter writer{Span<uint8>(direct)};
    writer.Write("Outside", 1);
    REQUIRE(writer.BeginObject({}, "Root"));
    writer.Write("Inside", 2);
    writer.EndObject();
    writer.EndObject();
    CHECK(!writer.BeginObject({}, "Second"));
    writer.EndObject();
    writer.Finish();
    const usize bytes = writer.Bytes();
    writer.Write("After", 3);
    CHECK(writer.Bytes() == bytes && !writer.Truncated());
}

// 공간 고갈: 블록이 없으면 남은 영역에서 필드를 버리고 잘림 표시를 남기며 인코딩은 늘 올바르다.
// 블록이 있으면 빌려 담고 렌더 뒤 돌려준다.
TEST(LogStructureSpaceExhaustionAndBlocks) {
    const Long value{"0123456789abcdef0123456789abcdef", 20};
    uint8 small[LogCaptureMinimumBytes(1) + 160];
    auto used = CaptureLog({Span<uint8>(small)}, LogLevel::Info, LogStructureTests, "{@L}", value);
    REQUIRE(used.IsOk());
    Rendered truncated;
    REQUIRE(truncated.From(Span<const uint8>(small, used.Value())).IsOk());
    CHECK(HasFlag(truncated.event, LogEventFlags::ValueTruncated));
    CHECK(truncated.event.message.StartsWith("{ F0: \"0123", StringComparison::CaseSensitive));
    CHECK(truncated.event.message.EndsWith(", ... }", StringComparison::CaseSensitive) ||
          truncated.event.message.EndsWith("... }", StringComparison::CaseSensitive));
    CHECK(truncated.event.properties[0].truncated);

    // 머리조차 들어가지 않으면 빈 인코딩이고 "{ ... }"다.
    uint8 tiny[LogCaptureMinimumBytes(1) + 4];
    used = CaptureLog({Span<uint8>(tiny)}, LogLevel::Info, LogStructureTests, "{@L}", value);
    REQUIRE(used.IsOk());
    Rendered nothing;
    REQUIRE(nothing.From(Span<const uint8>(tiny, used.Value())).IsOk());
    CHECK(nothing.event.message == "{ ... }");
    CHECK(HasFlag(nothing.event, LogEventFlags::ValueTruncated));

    // 블록: 슬롯에 들어가지 않는 값은 블록에 담고 렌더 뒤 돌려준다.
    TestBlocks blocks;
    const LogCaptureTarget target{Span<uint8>(small), &blocks};
    const Long fits{"abc", 3};
    used = CaptureLog(target, LogLevel::Info, LogStructureTests, "{@L}", value);
    REQUIRE(used.IsOk());
    CHECK(blocks.borrowed == 1 && blocks.released == 0);
    Rendered fromBlock;
    REQUIRE(fromBlock.From(Span<const uint8>(small, used.Value()), &blocks).IsOk());
    CHECK(!HasFlag(fromBlock.event, LogEventFlags::ValueTruncated));
    CHECK(fromBlock.event.message.EndsWith("F19: \"0123456789abcdef0123456789abcdef\" }",
                                           StringComparison::CaseSensitive));
    ReleaseLogCaptureBlocks(Span<const uint8>(small, used.Value()), blocks);
    CHECK(blocks.released == 1);

    // 작은 값은 블록에 인코딩한 뒤 슬롯으로 옮기고 블록을 바로 돌려준다.
    used = CaptureLog(target, LogLevel::Info, LogStructureTests, "{@L}", fits);
    REQUIRE(used.IsOk());
    CHECK(blocks.borrowed == 2 && blocks.released == 2);
    Rendered inlined;
    REQUIRE(inlined.From(Span<const uint8>(small, used.Value())).IsOk());
    CHECK(inlined.event.message == "{ F0: \"abc\", F1: \"abc\", F2: \"abc\" }");
}

// 노드 칸이 모자라면 노드만 버리고(MessageTruncated) 메시지는 다 렌더된다.
TEST(LogStructureNodeOverflowKeepsMessage) {
    static uint8 slot[8192];
    const Wide wide{MaxLogStructureFields};
    const auto used =
        CaptureLog({Span<uint8>(slot)}, LogLevel::Info, LogStructureTests, "{@W}", wide);
    REQUIRE(used.IsOk());
    static Rendered full;
    REQUIRE(full.From(Span<const uint8>(slot, used.Value())).IsOk());
    CHECK(!HasFlag(full.event, LogEventFlags::ValueTruncated));
    CHECK(HasFlag(full.event, LogEventFlags::MessageTruncated));
    CHECK(full.event.structureNodes.Size() == MaxLogStructureNodes);
    CHECK(full.event.properties[0].truncated);
    CHECK(full.event.message.EndsWith("F31: [0, 0, 0, 0, 0, 0, 0, 0] }",
                                      StringComparison::CaseSensitive));

    // JSON: 칸이 모자라 원소를 잃은 안쪽 배열과 그 조상은 잘림이다. 필드마다 노드는 배열 1 +
    // 원소 MaxLogStructureElements개이므로, 다 담긴 필드 뒤의 필드 하나가 일부만 담긴다.
    constexpr usize NodesPerField = 1 + MaxLogStructureElements;
    constexpr usize FullFields = MaxLogStructureNodes / NodesPerField;
    constexpr usize PartialItems = MaxLogStructureNodes - FullFields * NodesPerField - 1;
    static_assert(FullFields < MaxLogStructureFields && PartialItems < MaxLogStructureElements,
                  "the overflow must cut one array in the middle");
    static JsonRecord output;
    JsonLinesLogSink sink(LogTextOutput{&output, JsonRecord::Write, JsonRecord::Flush});
    REQUIRE(sink.Write(full.event).IsOk());
    SystemAllocator allocator;
    JsonDocument document(allocator);
    REQUIRE(document.Parse(output.View()).Succeeded());
    const JsonValue w = document.Root().Find("props").Find("W");
    CHECK(w.Find("$truncated").GetBool());
    CHECK(w.Find(FieldNames[FullFields - 1].View()).IsArray() &&
          w.Find(FieldNames[FullFields - 1].View()).Size() == MaxLogStructureElements);
    const JsonValue partial = w.Find(FieldNames[FullFields].View());
    CHECK(partial.Find("$count").TryGetUInt64().Value() == MaxLogStructureElements);
    CHECK(partial.Find("$items").Size() == PartialItems);
    CHECK(!w.Find(FieldNames[FullFields + 1].View()).IsValid());
    const LogProperty& cut = full.event.structureNodes[FullFields * NodesPerField];
    CHECK(cut.kind == LogPropertyKind::Array && cut.truncated && cut.subtreeCount == PartialItems);

    // 노드 칸이 없으면(빈 span) 노드를 채우지 않는다. 요청한 동작이라 잘림 표시도 없다.
    static Rendered none;
    REQUIRE(none.From(Span<const uint8>(slot, used.Value()), nullptr, 0).IsOk());
    CHECK(none.event.message == full.event.message);
    CHECK(none.event.structureNodes.Size() == 0);
    CHECK(!HasFlag(none.event, LogEventFlags::MessageTruncated));
    CHECK(!none.event.properties[0].truncated && none.event.properties[0].subtreeCount == 0);
}

// 공간이 모자라 중첩 객체·배열 안의 문자열이 잘리면 그것을 담은 모든 객체·배열이 잘림이다(텍스트의
// "..."와 JSON "$truncated"·"$count"가 같다).
TEST(LogStructureSpaceCutMarksAncestors) {
    // 머리 뒤 영역: 맨 위 객체 11 + A 19 + Inner 21 + List 19 = 70 byte와 End 3 byte를 쓰고,
    // 문자열은 머리·길이 5 byte 뒤 남은 22 byte만 담는다.
    constexpr usize StreamBytes = 100;
    constexpr usize KeptBytes = 22;
    uint8 slot[LogCaptureMinimumBytes(1) + StreamBytes];
    const DeepText value{"0123456789abcdef0123456789abcdef0123456789abcdef0123456789abcdef"};
    const auto used =
        CaptureLog({Span<uint8>(slot)}, LogLevel::Info, LogStructureTests, "{@T}", value);
    REQUIRE(used.IsOk());
    Rendered rendered;
    REQUIRE(rendered.From(Span<const uint8>(slot, used.Value())).IsOk());
    char expected[256];
    FixedStringBuilder text{Span<char>(expected)};
    REQUIRE(text.TryAppend("{ A: 1, Inner: { List: [\"").IsOk());
    REQUIRE(text.TryAppend(value.text.LeftBytesAtCodePoint(KeptBytes)).IsOk());
    REQUIRE(text.TryAppend("\"..., ... 1 items], ... }, ... }").IsOk());
    CHECK(rendered.event.message == text.View());
    CHECK(HasFlag(rendered.event, LogEventFlags::ValueTruncated));
    const LogEvent& event = rendered.event;
    CHECK(event.properties[0].truncated);
    // A(0)·Inner(1)·List(2)·문자열(3). 잘린 값과 상관없는 형제 A는 잘림이 아니다.
    REQUIRE(event.structureNodes.Size() == 4);
    CHECK(!event.structureNodes[0].truncated);
    CHECK(event.structureNodes[1].truncated && event.structureNodes[2].truncated &&
          event.structureNodes[3].truncated);
    CHECK(event.structureNodes[3].text.ByteCount() == KeptBytes);

    JsonRecord output;
    JsonLinesLogSink sink(LogTextOutput{&output, JsonRecord::Write, JsonRecord::Flush});
    REQUIRE(sink.Write(event).IsOk());
    SystemAllocator allocator;
    JsonDocument document(allocator);
    REQUIRE(document.Parse(output.View()).Succeeded());
    const JsonValue t = document.Root().Find("props").Find("T");
    CHECK(t.Find("$truncated").GetBool());
    CHECK(t.Find("Inner").Find("$truncated").GetBool());
    const JsonValue list = t.Find("Inner").Find("List");
    CHECK(list.Find("$count").TryGetUInt64().Value() == 1);
    CHECK(list.Find("$items").At(0).GetString() == value.text.LeftBytesAtCodePoint(KeptBytes));
}

// 깊이는 배열도 한 단계로 센다: 맨 위 객체 1 + 배열 MaxLogStructureDepth - 1단은 그대로, 한 단 더
// 깊으면 가장 안쪽 배열이 내용 없이 잘림("... 1 items")이고 JSON은 {"$count": 1, "$items": []}다.
TEST(LogStructureArrayDepthLimitIsExact) {
    uint8 slot[1024];
    auto used = CaptureLog({Span<uint8>(slot)}, LogLevel::Info, LogStructureTests, "{@N}",
                           ArrayNest{MaxLogStructureDepth - 1});
    REQUIRE(used.IsOk());
    Rendered exact;
    REQUIRE(exact.From(Span<const uint8>(slot, used.Value())).IsOk());
    CHECK(exact.event.message == "{ L: [[[1]]] }");
    CHECK(!HasFlag(exact.event, LogEventFlags::ValueTruncated));

    used = CaptureLog({Span<uint8>(slot)}, LogLevel::Info, LogStructureTests, "{@N}",
                      ArrayNest{MaxLogStructureDepth});
    REQUIRE(used.IsOk());
    Rendered over;
    REQUIRE(over.From(Span<const uint8>(slot, used.Value())).IsOk());
    CHECK(over.event.message == "{ L: [[[[... 1 items]]]] }");
    CHECK(HasFlag(over.event, LogEventFlags::ValueTruncated));

    JsonRecord output;
    JsonLinesLogSink sink(LogTextOutput{&output, JsonRecord::Write, JsonRecord::Flush});
    REQUIRE(sink.Write(over.event).IsOk());
    SystemAllocator allocator;
    JsonDocument document(allocator);
    REQUIRE(document.Parse(output.View()).Succeeded());
    const JsonValue innermost = document.Root().Find("props").Find("N").Find("L").At(0).At(0).At(0);
    CHECK(innermost.Find("$count").TryGetUInt64().Value() == 1);
    CHECK(innermost.Find("$items").IsArray() && innermost.Find("$items").Size() == 0);
}

namespace {

// 손으로 조립한 펼친 값 인코딩이다(손상 시험용). 기록 종류 번호와 비트는 인코딩 형식
// (Private/Diagnostics/LogStructureEncoding.h)을 시험 쪽에 따로 적은 값이다.
constexpr uint8 RecordNull = 1;
constexpr uint8 RecordBool = 2;
constexpr uint8 RecordSigned = 3;
constexpr uint8 RecordString = 6;
constexpr uint8 RecordObject = 10;
constexpr uint8 RecordArray = 11;
constexpr uint8 RecordEnd = 12;
constexpr uint8 RecordFirstUnknown = 13;
constexpr uint8 NameBit = 1u << 4;
constexpr uint8 TruncatedBit = 1u << 5;
constexpr uint8 ReservedBit6 = 1u << 6;
constexpr uint8 ReservedBit7 = 1u << 7;

constexpr char StaticA[] = "A";

struct Stream {
    uint8 bytes[512]{};
    usize size = 0;

    Stream& Put(const void* data, usize count) noexcept {
        platform::MemoryCopy(bytes + size, data, count);
        size += count;
        return *this;
    }

    Stream& Head(uint8 head) noexcept {
        return Put(&head, sizeof(head));
    }

    Stream& Static(const char* pointer, uint16 length) noexcept {
        Put(&pointer, sizeof(pointer));
        return Put(&length, sizeof(length));
    }

    // 맨 위 객체 머리(태그 없음)다.
    Stream& Root() noexcept {
        return Head(RecordObject).Static(nullptr, 0);
    }

    Stream& NamedSigned(int64 value) noexcept {
        Head(RecordSigned | NameBit).Static(StaticA, 1);
        return Put(&value, sizeof(value));
    }

    Stream& NamedObject(uint8 extra = 0) noexcept {
        return Head(static_cast<uint8>(RecordObject | NameBit | extra))
            .Static(StaticA, 1)
            .Static(nullptr, 0);
    }

    Stream& End() noexcept {
        return Head(RecordEnd);
    }
};

// 올바른 "{@F}" 캡처의 펼친 값 칸을 stream으로 바꿔 렌더한다. 캡처 안 값 영역(필드 32개)이 충분히
// 커서 손으로 만든 인코딩을 그 자리에 쓴다.
Result<void> RenderCorrupt(const Stream& stream) noexcept {
    static uint8 capture[4096];
    const auto used = CaptureLog({Span<uint8>(capture)}, LogLevel::Info, LogStructureTests, "{@F}",
                                 Fields{MaxLogStructureFields});
    if (!used) {
        return Result<void>::Failure(ErrorCode::DependencyFailed);
    }

    detail::LogCaptureValue value;
    platform::MemoryCopy(&value, capture + sizeof(detail::LogCaptureHeader), sizeof(value));
    if (stream.size > value.bytes) {
        return Result<void>::Failure(ErrorCode::DependencyFailed);
    }

    platform::MemoryCopy(capture + value.payload, stream.bytes, stream.size);
    value.bytes = static_cast<uint32>(stream.size);
    platform::MemoryCopy(capture + sizeof(detail::LogCaptureHeader), &value, sizeof(value));
    static Rendered rendered;
    return rendered.From(Span<const uint8>(capture, used.Value()));
}

} // namespace

// 손상된 펼친 값 인코딩은 렌더가 거부한다(범위 밖을 읽지 않고, 멈추지 않는다). 사례마다 하나씩
// 확인하고, 같은 도구로 만든 올바른 인코딩은 렌더된다(대조).
TEST(LogStructureRenderRejectsCorruptEncodings) {
    Stream valid;
    valid.Root().NamedSigned(1).End();
    CHECK(RenderCorrupt(valid).IsOk());

    Stream nested;
    nested.Root().NamedObject().NamedObject().NamedObject();
    nested.NamedObject(TruncatedBit).End().End().End().End().End();
    CHECK(RenderCorrupt(nested).IsOk());

    const auto rejected = [](const Stream& stream) noexcept {
        return RenderCorrupt(stream).Error() == ErrorCode::InvalidArgument;
    };

    Stream kindZero;
    kindZero.Head(0);
    CHECK(rejected(kindZero));

    Stream kindUnknown;
    kindUnknown.Root().Head(RecordFirstUnknown | NameBit).Static(StaticA, 1).End();
    CHECK(rejected(kindUnknown));

    Stream reserved6;
    reserved6.Head(RecordObject | ReservedBit6).Static(nullptr, 0).End();
    CHECK(rejected(reserved6));

    Stream reserved7;
    reserved7.Head(RecordObject | ReservedBit7).Static(nullptr, 0).End();
    CHECK(rejected(reserved7));

    const uint64 one = 1;
    Stream namedElement;
    namedElement.Root().Head(RecordArray | NameBit).Static(StaticA, 1).Put(&one, sizeof(one));
    namedElement.NamedSigned(1).End().End();
    CHECK(rejected(namedElement));

    const int64 number = 1;
    Stream unnamedField;
    unnamedField.Root().Head(RecordSigned).Put(&number, sizeof(number)).End();
    CHECK(rejected(unnamedField));

    Stream tooFewEnds;
    tooFewEnds.Root().NamedObject().NamedSigned(1).End();
    CHECK(rejected(tooFewEnds));

    Stream tooManyEnds;
    tooManyEnds.Root().End().End();
    CHECK(rejected(tooManyEnds));

    Stream trailing;
    trailing.Root().End().Head(RecordNull);
    CHECK(rejected(trailing));

    Stream secondRoot;
    secondRoot.Root().End().Root().End();
    CHECK(rejected(secondRoot));

    // 깊이 상한 자리(맨 위 + 3단 아래)의 객체는 잘림 표시가 있어야 하고 내용이 없어야 한다.
    Stream deepWithoutMark;
    deepWithoutMark.Root().NamedObject().NamedObject().NamedObject();
    deepWithoutMark.NamedObject().End().End().End().End().End();
    CHECK(rejected(deepWithoutMark));

    Stream deepWithContent;
    deepWithContent.Root().NamedObject().NamedObject().NamedObject();
    deepWithContent.NamedObject(TruncatedBit).NamedSigned(1).End().End().End().End().End();
    CHECK(rejected(deepWithContent));

    const uint32 pastEnd = 1000;
    Stream longString;
    longString.Root()
        .Head(RecordString | NameBit)
        .Static(StaticA, 1)
        .Put(&pastEnd, sizeof(pastEnd));
    longString.End();
    CHECK(rejected(longString));

    const uint8 two = 2;
    Stream boolTwo;
    boolTwo.Root().Head(RecordBool | NameBit).Static(StaticA, 1).Put(&two, sizeof(two)).End();
    CHECK(rejected(boolTwo));

    Stream nullName;
    nullName.Root().Head(RecordSigned | NameBit).Static(nullptr, 3).Put(&number, sizeof(number));
    nullName.End();
    CHECK(rejected(nullName));
}

// 런타임 템플릿: 펼치기 자리는 렌더 때 검증한다.
TEST(LogStructureRuntimeTemplates) {
    uint8 slot[1024];
    auto used = CaptureLog({Span<uint8>(slot)}, LogLevel::Info, LogStructureTests,
                           RuntimeTemplate{StringView("at {@P}")}, Point{5, 6});
    REQUIRE(used.IsOk());
    Rendered ok;
    REQUIRE(ok.From(Span<const uint8>(slot, used.Value())).IsOk());
    CHECK(ok.event.message == "at Point { X: 5, Y: 6 }");

    used = CaptureLog({Span<uint8>(slot)}, LogLevel::Info, LogStructureTests,
                      RuntimeTemplate{StringView("at {@P}")}, 5);
    REQUIRE(used.IsOk());
    Rendered missing;
    REQUIRE(missing.From(Span<const uint8>(slot, used.Value())).IsOk());
    CHECK(missing.event.message == "at {@P} [template error: StructureWithoutLogStructure]");
    CHECK(HasFlag(missing.event, LogEventFlags::TemplateError));

    used = CaptureLog({Span<uint8>(slot)}, LogLevel::Info, LogStructureTests,
                      RuntimeTemplate{StringView("at {@P:x}")}, Point{});
    REQUIRE(used.IsOk());
    Rendered spec;
    REQUIRE(spec.From(Span<const uint8>(slot, used.Value())).IsOk());
    CHECK(spec.event.message == "at {@P:x} [template error: SpecOnStructure]");

    // LogStructure만 있는 타입을 일반 자리에 쓰면 그 자리만 "<format error>"다.
    used = CaptureLog({Span<uint8>(slot)}, LogLevel::Info, LogStructureTests,
                      RuntimeTemplate{StringView("at {P}")}, Point{});
    REQUIRE(used.IsOk());
    Rendered plain;
    REQUIRE(plain.From(Span<const uint8>(slot, used.Value())).IsOk());
    CHECK(plain.event.message == "at <format error>");
    CHECK(HasFlag(plain.event, LogEventFlags::FormatError));
}

// JSON lines: props 안 중첩 객체, "$type"·"$truncated"·"$count"·"$items", 문자열 escape.
TEST(LogStructureJsonLinesNestsObjects) {
    Player player = MakePlayer();
    player.name = StringView("q\"b\\n\n\x01\xED\x95\x9C", 10);
    uint8 slot[2048];
    auto used = CaptureLog({Span<uint8>(slot)}, LogLevel::Info, LogStructureTests, "{@P}", player);
    REQUIRE(used.IsOk());
    Rendered rendered;
    REQUIRE(rendered.From(Span<const uint8>(slot, used.Value())).IsOk());
    CHECK(rendered.event.message.StartsWith(
        "Player { Name: \"q\\\"b\\\\n\\n\\u0001\xED\x95\x9C\", Hp: 30",
        StringComparison::CaseSensitive));
    JsonRecord output;
    JsonLinesLogSink sink(LogTextOutput{&output, JsonRecord::Write, JsonRecord::Flush});
    REQUIRE(sink.Write(rendered.event).IsOk());
    SystemAllocator allocator;
    JsonDocument document(allocator);
    REQUIRE(document.Parse(output.View()).Succeeded());
    const JsonValue p = document.Root().Find("props").Find("P");
    CHECK(p.Find("$type").GetString() == "Player");
    CHECK(!p.Find("$truncated").IsValid());
    CHECK(p.Find("Name").GetString() == StringView("q\"b\\n\n\x01\xED\x95\x9C", 10));
    CHECK(p.Find("Hp").TryGetInt64().Value() == 30);
    CHECK(p.Find("Alive").GetBool());
    CHECK(p.Find("Speed").TryGetDouble().Value() == 1.5);
    CHECK(p.Find("Pos").Find("$type").GetString() == "Point");
    CHECK(p.Find("Pos").Find("Y").TryGetInt64().Value() == -2);
    REQUIRE(p.Find("Scores").Size() == 3);
    CHECK(p.Find("Scores").At(2).TryGetInt64().Value() == 3);
    CHECK(p.Find("Mode").TryGetUInt64().Value() == 2);
    CHECK(p.Find("Note").IsNull());
    CHECK(p.Find("Opaque").GetString() == "<unsupported>");
    CHECK(p.Find("Grid").GetString() == "(3, 4)");

    // 잘린 배열과 객체.
    Elements elements;
    elements.count = MaxLogStructureElements + 1;
    used = CaptureLog({Span<uint8>(slot)}, LogLevel::Info, LogStructureTests, "{@E} {@F}", elements,
                      Fields{MaxLogStructureFields + 1});
    REQUIRE(used.IsOk());
    Rendered truncated;
    REQUIRE(truncated.From(Span<const uint8>(slot, used.Value())).IsOk());
    output.bytes = 0;
    REQUIRE(sink.Write(truncated.event).IsOk());
    REQUIRE(document.Parse(output.View()).Succeeded());
    const JsonValue props = document.Root().Find("props");
    const JsonValue values = props.Find("E").Find("Values");
    CHECK(values.Find("$count").TryGetUInt64().Value() == MaxLogStructureElements + 1);
    CHECK(values.Find("$items").Size() == MaxLogStructureElements);
    CHECK(!props.Find("E").Find("$type").IsValid());
    CHECK(props.Find("F").Find("$truncated").GetBool());
    CHECK(props.Find("F").Find("F31").TryGetInt64().Value() == 31);
}

// 손으로 만든 LogEvent의 잘못된 노드 범위는 쓰지 않고 대체 줄로 간다(범위 밖을 읽지 않는다).
TEST(LogStructureJsonRejectsBrokenNodeRanges) {
    LogProperty top;
    top.name = "P";
    top.kind = LogPropertyKind::Structure;
    top.subtreeBegin = 0;
    top.subtreeCount = 3;
    LogProperty nodes[1];
    nodes[0].name = "A";
    LogEvent event;
    event.category = "Tests.Structure";
    event.message = "m";
    event.properties = Span<const LogProperty>(&top, 1);
    event.structureNodes = Span<const LogProperty>(nodes, 1);
    JsonRecord output;
    JsonLinesLogSink sink(LogTextOutput{&output, JsonRecord::Write, JsonRecord::Flush});
    REQUIRE(sink.Write(event).IsOk());
    SystemAllocator allocator;
    JsonDocument document(allocator);
    REQUIRE(document.Parse(output.View()).Succeeded());
    CHECK(!document.Root().Find("props").IsValid());
    CHECK(document.Root().Find("flags").At(0).GetString() == "MessageTruncated");
}

namespace {

// 받은 기록의 메시지를 줄로 모은다.
class MessageSink final : public RuntimeLogSink {
public:
    Result<void> Write(const LogEvent& event) noexcept override {
        if (sizeof(text) - bytes >= MinimumLogRenderBytes) {
            FixedStringBuilder line{Span<char>(text + bytes, sizeof(text) - bytes)};
            (void)line.TryAppend(event.message);
            (void)line.TryAppendAscii('\n');
            bytes += line.ByteCount();
        }

        return Result<void>::Success();
    }

    StringView View() const noexcept {
        return StringView(text, bytes);
    }

    char text[16384]{};
    usize bytes = 0;
};

class CountingAllocator final : public Allocator {
public:
    Result<void*> Allocate(usize bytes, usize alignment, uint32 tag = 0) noexcept override {
        (void)platform::AtomicFetchAdd<platform::MemoryOrder::Relaxed>(&allocations, uint64{1});
        return inner.Allocate(bytes, alignment, tag);
    }

    Result<void> Deallocate(void* address) noexcept override {
        return inner.Deallocate(address);
    }

    uint64 Allocations() const noexcept {
        return platform::AtomicLoad<platform::MemoryOrder::Relaxed>(&allocations);
    }

    SystemAllocator inner;
    uint64 allocations = 0;
};

LogSystemConfig Config(LogMode mode) noexcept {
    LogSystemConfig config;
    config.mode = mode;
    config.globalMinimum = LogLevel::Trace;
    return config;
}

// 설치 없이 남은 낮은 레벨 버림 수를 비운다(설치 때 알림 한 줄이 sink에 들어가지 않게).
void ResetUninstalledDrops() noexcept {
    SystemAllocator allocator;
    LogSystem reset(allocator, Config(LogMode::Manual));
    (void)reset.Start();
    LogInstallScope installed(reset);
}

// 기록을 남긴 뒤 원본을 바꾼다(출력은 기록 때 값이어야 한다).
void EmitStructures() noexcept {
    Player player = MakePlayer();
    Log::Info(LogStructureTests, "spawn {@Player}", player);
    player.hp = -1;
    player.position.x = 99;
    Elements elements;
    elements.count = MaxLogStructureElements + 1;
    Log::Warning(LogStructureTests, "list {@E} then {@D}", elements,
                 Deep{1, static_cast<int32>(MaxLogStructureDepth) + 1});
    Log::Write(LogLevel::Info, LogStructureTests, RuntimeTemplate{StringView("runtime {@P}")},
               Point{7, 8});
    player.hp = 31;
    Log::Info(LogStructureTests, "again {@Player}", player);
}

} // namespace

// Sync·Manual·Async가 같은 출력을 낸다(캡처는 호출 스레드에서 끝나므로 출력 시점과 무관하다).
TEST(LogStructureModesProduceIdenticalOutput) {
    ResetUninstalledDrops();
    const LogMode modes[] = {LogMode::Sync, LogMode::Manual, LogMode::Async};
    static MessageSink sinks[3];
    for (usize index = 0; index < 3; ++index) {
        SystemAllocator allocator;
        LogSystem logs(allocator, Config(modes[index]));
        REQUIRE(logs.AddSink(sinks[index]).IsOk());
        REQUIRE(logs.Start().IsOk());
        {
            LogInstallScope installed(logs);
            EmitStructures();
            CHECK(logs.Flush());
        }

        logs.Stop();
    }

    CHECK(sinks[0].View() == sinks[1].View());
    CHECK(sinks[0].View() == sinks[2].View());
    CHECK(sinks[0].View().Contains("spawn Player { Name: \"A\\\"nn\\n\", Hp: 30,",
                                   StringComparison::CaseSensitive));
    CHECK(sinks[0].View().Contains("Pos: Point { X: 1, Y: -2 }", StringComparison::CaseSensitive));
    CHECK(sinks[0].View().Contains("again Player { Name: \"A\\\"nn\\n\", Hp: 31,",
                                   StringComparison::CaseSensitive));
    CHECK(
        sinks[0].View().Contains("runtime Point { X: 7, Y: 8 }", StringComparison::CaseSensitive));
    CHECK(sinks[0].View().Contains("list { Values: [0, 0, 0, 0, 0, 0, 0, 0, ... 9 items] } then",
                                   StringComparison::CaseSensitive));
}

// Start 뒤 펼치기 기록(텍스트·JSON sink 포함)은 할당하지 않는다.
TEST(LogStructureDoesNotAllocateAfterStart) {
    ResetUninstalledDrops();
    const LogMode modes[] = {LogMode::Async, LogMode::Sync, LogMode::Manual};
    for (LogMode mode : modes) {
        CountingAllocator allocator;
        LogSystemConfig config = Config(mode);
        config.queueSlots = 64;
        config.blockCount = 4;
        LogSystem logs(allocator, config);
        MessageSink sink;
        JsonRecord json;
        JsonLinesLogSink jsonSink(LogTextOutput{&json, JsonRecord::Write, JsonRecord::Flush});
        REQUIRE(logs.AddSink(sink).IsOk());
        REQUIRE(logs.AddSink(jsonSink).IsOk());
        REQUIRE(logs.Start().IsOk());
        const uint64 afterStart = allocator.Allocations();
        const uint64 news = test::OperatorNewCalls();
        {
            LogInstallScope installed(logs);
            for (uint32 round = 0; round < 8; ++round) {
                json.bytes = 0;
                EmitStructures();
                CHECK(logs.Flush());
            }
        }

        CHECK(allocator.Allocations() == afterStart);
        CHECK(test::OperatorNewCalls() == news);
        CHECK(sink.View().Contains("spawn Player {", StringComparison::CaseSensitive));
        logs.Stop();
    }
}

// LogLine::Write(Lazy)도 펼치기를 받는다.
TEST(LogStructureLazyLineWrite) {
    ResetUninstalledDrops();
    SystemAllocator allocator;
    LogSystem logs(allocator, Config(LogMode::Manual));
    static MessageSink sink;
    sink.bytes = 0;
    REQUIRE(logs.AddSink(sink).IsOk());
    REQUIRE(logs.Start().IsOk());
    {
        LogInstallScope installed(logs);
        Log::DebugLazy(LogStructureTests, [](LogLine& line) noexcept {
            line.Write("lazy {@P}", Point{1, 2});
            line.Write(" then {@E}", Empty{});
        });
        CHECK(logs.Pump() == 1);
    }

    CHECK(sink.View() == "lazy Point { X: 1, Y: 2 } then Empty {}\n");
}

namespace {

// 설치 전(LogSystem 없음) Warning의 비상 출력 경로에서 펼치기를 렌더한다. 자식 프로세스에서만
// 부른다.
int LogStructureEmergencyProbe(int, char**) noexcept {
    Log::Warning(LogStructureTests, "pre-install {@P} {@E}", Point{3, -4}, Empty{});
    return 0;
}

const bool LogStructureEmergencyProbeRegistered =
    rocks::test::RegisterProcessProbe("log-structure-emergency", &LogStructureEmergencyProbe);

} // namespace

// 설치 전 비상 경로(stderr 바로 쓰기)도 펼친 값을 같은 모양으로 낸다. 자식 프로세스의 stderr로
// 확인한다(기다림에 시간 한도를 두지 않는다).
TEST(LogStructureEmergencyPathBeforeInstall) {
    CHECK(LogStructureEmergencyProbeRegistered);
    char executable[4096];
    usize executableBytes = 0;
    REQUIRE(!!platform::GetExecutablePath(executable, sizeof(executable), executableBytes));
    const char* const mode = "log-structure-emergency";
    const platform::ProcessArgument arguments[] = {{"--process-probe", strlen("--process-probe")},
                                                   {mode, strlen(mode)}};
    platform::ProcessStartInfo info;
    info.executable = executable;
    info.executableBytes = executableBytes;
    info.arguments = arguments;
    info.argumentCount = 2;
    info.output = platform::ProcessStreamMode::Null;
    info.error = platform::ProcessStreamMode::Pipe;
    static char error[4096];
    platform::ProcessRunOptions options;
    options.error = error;
    options.errorCapacity = sizeof(error) - 1;
    const platform::ProcessRunResult run = platform::RunProcess(info, options);
    REQUIRE(run.status && !run.timedOut);
    CHECK(run.exit.kind == platform::ProcessExitKind::Exited && run.exit.code == 0);
    const StringView output(error, run.errorBytes < sizeof(error) ? run.errorBytes : 0);
    CHECK(output.Contains("[Warning][Tests.Structure] pre-install Point { X: 3, Y: -4 } Empty {}",
                          StringComparison::CaseSensitive));
}
