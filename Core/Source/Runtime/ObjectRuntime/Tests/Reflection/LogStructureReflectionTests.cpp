#include <ReflectionTestFixtures/ArrayFixture.generated.h>
#include <ReflectionTestFixtures/BitfieldFixture.generated.h>
#include <ReflectionTestFixtures/OptionalFixture.generated.h>
#include <ReflectionTestFixtures/StaticArrayFixture.generated.h>
#include <ReflectionTestFixtures/StructFixture.generated.h>
#include <Core/Diagnostics/Log.h>
#include <Core/Diagnostics/LogCapture.h>
#include <Core/Diagnostics/LogSystem.h>
#include <Core/Diagnostics/RuntimeLogSink.h>
#include <Core/Memory/SystemAllocator.h>
#include <Core/Text/Json.h>
#include <Object/LogStructure.h>
#include <Platform/Atomics.h>
#include <Platform/Memory.h>
#include <TestSupport/Test.h>

// 리플렉션 타입의 로그 구조체 펼치기 시험이다(LG16, Docs/Design/LoggingSystem.ko.md 14.1.6절):
// MetaCompiler가 낸 필드 방문 함수(GeneratedType<T>::VisitFields)로 ObjectRuntime 일반 특수화가
// registry 없이 값을 읽는다. 중첩 struct·enum 이름(선언되지 않은 값은 정수)·String·정적 배열·배열
// 상한·Optional·bitfield(private 포함)·객체 참조(역참조 없이 id)·Formatter 종류(Guid·AssetId)·
// 지원하지 않는 종류(Name·ClassRef·SoftObjectRef·Digest), 캡처 뒤 원본 변경에도 출력 불변,
// JSON 중첩 객체, LogSystem 경로의 무할당을 확인한다.
using namespace rocks;
using reflection_fixture::ArrayTag;
using reflection_fixture::BitfieldFlags;
using reflection_fixture::BitfieldHolder;
using reflection_fixture::FixedMode;
using reflection_fixture::FixedOwner;
using reflection_fixture::OptionalPoint;
using reflection_fixture::OptionalSlot;
using reflection_fixture::StructHolder;
using reflection_fixture::StructLeaf;
using reflection_fixture::TextureGroup;

namespace rocks::test {
uint64 OperatorNewCalls() noexcept;
} // namespace rocks::test

// 생성 헤더와 Object/LogStructure.h를 함께 본 리플렉션 타입은 모두 LogStructure를 가진다(클래스·
// struct·bitfield struct). 생성 헤더는 그 헤더를 include하지 않으므로 여기서
// include한다(설계 14.2절 LG16의 include 규칙).
static_assert(HasLogStructure<StructHolder> && HasLogStructure<StructLeaf>);
static_assert(HasLogStructure<FixedOwner> && HasLogStructure<BitfieldHolder>);
static_assert(HasLogStructure<BitfieldFlags> && HasLogStructure<OptionalSlot>);
// 리플렉션 enum·일반 값은 펼치지 않는다.
static_assert(!HasLogStructure<TextureGroup> && !HasLogStructure<int32>);

namespace {

constexpr LogCategory LogReflectionTests{"Tests.ReflectionLog"};

struct Rendered {
    char message[8192]{};
    char propertyText[2048]{};
    LogProperty properties[MaxLogArguments];
    LogProperty nodes[MaxLogStructureNodes];
    LogEvent event;

    Result<void> From(Span<const uint8> capture) noexcept {
        LogRenderTarget target{
            Span<char>(message),     Span<char>(propertyText), Span<LogProperty>(properties), {}, 0,
            Span<LogProperty>(nodes)};
        return RenderLogCapture(capture, nullptr, target, event);
    }
};

void FillHolder(StructHolder& holder) noexcept {
    holder.middle.leaf.count = 3;
    CHECK(holder.middle.leaf.label.TryAssign("abc"));
    holder.middle.leaf.group = TextureGroup::EightBitData;
    holder.middle.offset = -5;
    holder.middle.note = 7;
    holder.middle.cache = 99; // NATIVE 멤버는 펼치지 않는다
    holder.flag = 1;
}

constexpr StringView HolderText =
    "StructHolder { target: null, middle: StructMiddle { leaf: StructLeaf { count: 3, label: "
    "\"abc\", group: EightBitData }, offset: -5, note: 7 }, flag: 1 }";

struct JsonLine {
    char line[JsonLinesLogSink::BufferBytes]{};
    usize bytes = 0;

    static Result<void> Write(void* raw, StringView view) noexcept {
        auto& self = *static_cast<JsonLine*>(raw);
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
};

} // namespace

// 생성 struct 왕복: 스키마 순서의 필드, 중첩 struct, enum 이름, NATIVE 멤버 제외, 원본 변경 뒤
// 불변.
TEST(LogStructureRoundTripsGeneratedStruct) {
    SystemAllocator allocator;
    StructHolder holder(allocator);
    FillHolder(holder);
    static uint8 slot[4096];
    const auto used =
        CaptureLog({Span<uint8>(slot)}, LogLevel::Info, LogReflectionTests, "{@Holder}", holder);
    REQUIRE(used.IsOk());

    // 캡처 뒤 원본을 바꾼다(문자열 버퍼 재할당 포함).
    holder.middle.leaf.count = 42;
    CHECK(holder.middle.leaf.label.TryAssign("a much longer label that reallocates the buffer"));
    holder.middle.leaf.group = TextureGroup::World;
    static Rendered rendered;
    REQUIRE(rendered.From(Span<const uint8>(slot, used.Value())).IsOk());
    CHECK(rendered.event.message == HolderText);
    CHECK(!HasAnyLogEventFlags(rendered.event.flags,
                               LogEventFlags::ValueTruncated | LogEventFlags::FormatError));
    const LogProperty& top = rendered.event.properties[0];
    CHECK(top.kind == LogPropertyKind::Structure && top.typeTag == "StructHolder" &&
          top.name == "Holder");
    // target, middle(leaf(count, label, group), offset, note), flag
    CHECK(top.subtreeCount == 9);
    const LogProperty& group = rendered.event.structureNodes[5];
    CHECK(group.name == "group" && group.text == "EightBitData" &&
          group.kind == LogPropertyKind::String && group.parent == 2);

    // 선언되지 않은 enum 값은 바탕 정수다.
    holder.middle.leaf.group = static_cast<TextureGroup>(5);
    const auto undeclared = CaptureLog({Span<uint8>(slot)}, LogLevel::Info, LogReflectionTests,
                                       "{@Leaf}", holder.middle.leaf);
    REQUIRE(undeclared.IsOk());
    REQUIRE(rendered.From(Span<const uint8>(slot, undeclared.Value())).IsOk());
    CHECK(rendered.event.message ==
          "StructLeaf { count: 42, label: \"a much longer label that reallocates the buffer\", "
          "group: 5 }");
}

// 클래스 객체(registry 밖에서 만든 값): 정적 배열·enum 배열·struct 배열·객체 참조(id만)·Formatter
// 종류·지원하지 않는 종류·원소 상한.
TEST(LogStructureReflectsClassFieldsWithoutDereferencing) {
    SystemAllocator allocator;
    FixedOwner owner(allocator);
    owner.flags[0] = true;
    owner.ints[0] = 1;
    owner.ints[1] = 2;
    owner.ints[2] = 3;
    owner.ints[3] = 4;
    owner.modes[1] = FixedMode::Second;
    owner.points[1].x = 7;
    owner.points[1].pair[0] = 1;
    owner.points[1].pair[1] = -1;
    owner.guids[0] = Guid{0x0123456789abcdefull, 0xfedcba9876543210ull};
    owner.watchers[0] = WeakObjectRef<FixedOwner>(ObjectHandle{1, 5, 2});
    CHECK(owner.labels[1].TryAssign("tail"));
    static uint8 slot[8192];
    const auto used =
        CaptureLog({Span<uint8>(slot)}, LogLevel::Info, LogReflectionTests, "{@Owner}", owner);
    REQUIRE(used.IsOk());
    static Rendered rendered;
    REQUIRE(rendered.From(Span<const uint8>(slot, used.Value())).IsOk());
    const StringView message = rendered.event.message;
    const StringView expected[] = {
        "FixedOwner { flags: [true, false, false], bytes: [0, 0],",
        "ints: [1, 2, 3, 4],",
        "guids: [\"01234567-89ab-cdef-fedc-ba9876543210\", "
        "\"00000000-0000-0000-0000-000000000000\"]",
        "labels: [\"\", \"tail\"]",
        "names: [<unsupported>, <unsupported>]",
        "refs: [null, null, null]",
        "watchers: [\"1:5:2\", null]",
        "kinds: [<unsupported>, <unsupported>]",
        "modes: [First, Second]",
        "points: [FixedPoint { x: 0, pair: [0, 0] }, FixedPoint { x: 7, pair: [1, -1] }]",
        "links: [FixedLink { target: null, weight: 0 }, FixedLink { target: null, weight: 0 }]",
        "softs: [<unsupported>, <unsupported>]",
        "digests: [<unsupported>, <unsupported>]",
        "octets: [0, 0, 0, 0, 0, 0, 0, 0, ... 256 items]",
        "quads: [0, 0, 0, 0, 0, 0, 0, 0, ... 256 items] }"};
    for (const StringView& part : expected) {
        CHECK(message.Contains(part, StringComparison::CaseSensitive));
    }

    CHECK(rendered.event.properties[0].typeTag == "FixedOwner");
    // 원소 상한을 넘은 배열은 잘림이다(기록에 ValueTruncated).
    CHECK(HasAnyLogEventFlags(rendered.event.flags, LogEventFlags::ValueTruncated));
    CHECK(!HasAnyLogEventFlags(rendered.event.flags, LogEventFlags::MessageTruncated));
}

// bitfield(생성 접근자, private 포함)·Optional·원소 struct 안 배열.
TEST(LogStructureReflectsBitfieldsOptionalsAndArrays) {
    SystemAllocator allocator;
    BitfieldHolder holder(allocator);
    holder.on = 1;
    holder.SetHidden(true);
    holder.flags.second = 1;
    holder.flags.plain = 4;
    static uint8 slot[4096];
    auto used =
        CaptureLog({Span<uint8>(slot)}, LogLevel::Info, LogReflectionTests, "{@Holder}", holder);
    REQUIRE(used.IsOk());
    static Rendered rendered;
    REQUIRE(rendered.From(Span<const uint8>(slot, used.Value())).IsOk());
    CHECK(rendered.event.message ==
          "BitfieldHolder { on: true, off: false, big: false, locked: false, flags: "
          "BitfieldFlags { first: false, second: true, wide: false, wider: false, widest: false, "
          "plain: 4 }, list: [], hidden: true }");

    OptionalSlot optional;
    optional.count.Emplace(3u);
    OptionalPoint& point = optional.point.Emplace(allocator);
    point.x = 1;
    point.y = 2;
    CHECK(point.label.TryAssign("p"));
    used = CaptureLog({Span<uint8>(slot)}, LogLevel::Info, LogReflectionTests, "{@Slot}", optional);
    REQUIRE(used.IsOk());
    REQUIRE(rendered.From(Span<const uint8>(slot, used.Value())).IsOk());
    CHECK(rendered.event.message == "OptionalSlot { count: 3, note: null, point: OptionalPoint { "
                                    "x: 1, y: 2, label: \"p\", weight: null } }");

    ArrayTag tag(allocator);
    tag.id = 9;
    for (uint16 code = 0; code < MaxLogStructureElements + 2; ++code) {
        CHECK(tag.codes.TryPushBack(code));
    }

    used = CaptureLog({Span<uint8>(slot)}, LogLevel::Info, LogReflectionTests, "{@Tag}", tag);
    REQUIRE(used.IsOk());
    REQUIRE(rendered.From(Span<const uint8>(slot, used.Value())).IsOk());
    CHECK(rendered.event.message ==
          "ArrayTag { id: 9, label: \"\", codes: [0, 1, 2, 3, 4, 5, 6, 7, ... 10 items] }");
}

// JSON lines: props 안 중첩 객체, "$type"은 도구 이름, enum은 이름 문자열.
TEST(LogStructureGeneratedStructJson) {
    SystemAllocator allocator;
    StructHolder holder(allocator);
    FillHolder(holder);
    static uint8 slot[4096];
    const auto used =
        CaptureLog({Span<uint8>(slot)}, LogLevel::Info, LogReflectionTests, "{@Holder}", holder);
    REQUIRE(used.IsOk());
    static Rendered rendered;
    REQUIRE(rendered.From(Span<const uint8>(slot, used.Value())).IsOk());
    static JsonLine output;
    output.bytes = 0;
    JsonLinesLogSink sink(LogTextOutput{&output, JsonLine::Write, JsonLine::Flush});
    REQUIRE(sink.Write(rendered.event).IsOk());
    JsonDocument document(allocator);
    REQUIRE(document.Parse(StringView(output.line, output.bytes)).Succeeded());
    const JsonValue value = document.Root().Find("props").Find("Holder");
    CHECK(value.Find("$type").GetString() == "StructHolder");
    CHECK(value.Find("target").IsNull());
    const JsonValue leaf = value.Find("middle").Find("leaf");
    CHECK(leaf.Find("$type").GetString() == "StructLeaf");
    CHECK(leaf.Find("count").TryGetUInt64().Value() == 3);
    CHECK(leaf.Find("label").GetString() == "abc");
    CHECK(leaf.Find("group").GetString() == "EightBitData");
    CHECK(value.Find("middle").Find("offset").TryGetInt64().Value() == -5);
    CHECK(value.Find("flag").TryGetUInt64().Value() == 1);
}

namespace {

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

    char text[8192]{};
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

} // namespace

// LogSystem 경로(Sync·Manual·Async): 기록 뒤 원본을 바꿔도 출력은 기록 때 값이고, Start 뒤 할당이
// 없다.
TEST(LogStructureGeneratedStructThroughLogSystem) {
    {
        // 앞선 시험이 설치 없이 남긴 버림 수를 비운다(설치 때 알림 한 줄이 sink에 들어가지 않게).
        SystemAllocator allocator;
        LogSystemConfig config;
        config.mode = LogMode::Manual;
        LogSystem reset(allocator, config);
        (void)reset.Start();
        LogInstallScope installed(reset);
    }

    const LogMode modes[] = {LogMode::Sync, LogMode::Manual, LogMode::Async};
    for (LogMode mode : modes) {
        SystemAllocator values;
        StructHolder holder(values);
        FillHolder(holder);
        CountingAllocator allocator;
        LogSystemConfig config;
        config.mode = mode;
        config.globalMinimum = LogLevel::Trace;
        LogSystem logs(allocator, config);
        static MessageSink sink;
        sink.bytes = 0;
        REQUIRE(logs.AddSink(sink).IsOk());
        REQUIRE(logs.Start().IsOk());
        const uint64 afterStart = allocator.Allocations();
        const uint64 news = test::OperatorNewCalls();
        {
            LogInstallScope installed(logs);
            Log::Info(LogReflectionTests, "{@Holder}", holder);
            holder.middle.leaf.count = 1000;
            holder.flag = 0;
            CHECK(logs.Flush());
        }

        CHECK(allocator.Allocations() == afterStart);
        CHECK(test::OperatorNewCalls() == news);
        CHECK(StringView(sink.text, sink.bytes) ==
              "StructHolder { target: null, middle: StructMiddle { "
              "leaf: StructLeaf { count: 3, label: \"abc\", "
              "group: EightBitData }, offset: -5, note: 7 }, "
              "flag: 1 }\n");
        logs.Stop();
    }
}
