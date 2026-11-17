#include <ReflectionTestFixtures/TextFieldFixture.generated.h>
#include <Core/Memory/SystemAllocator.h>
#include <Object/PropertyPath.h>
#include <ObjectSerialization/ObjectGraph.h>
#include <Platform/Fatal.h>
#include <Serialization/Archive.h>
#include <Serialization/Record.h>
#include <Serialization/TextWire.h>
#include <TestSupport/NFailAllocator.h>
#include <TestSupport/Test.h>
#include "GraphTestSupport.h"

// 리플렉션 B9l Text PROPERTY 시험이다(Docs/Design/ReflectionExtensions.ko.md 13절, 17절 B9l 항목,
// fixture ReflectionTestFixtures/TextFieldFixture.h): 세 형태(비어 있음·Invariant·Localized)의
// 필드·Array 원소·정적 배열 원소·Map 값·struct 멤버 왕복과 다시 저장한 byte 같음, Text::Localized
// 거부 집합과 ReadTextWire·WriteTextWire 거부 집합의 대조, Format·Integer encode Unsupported, 원문
// 한도, 2단계 할당 실패 rollback, FieldValue 읽기·Registry 사이 복사의 DeepClone(원래 allocator
// 해제 뒤에도 유효), 경로 쓰기.
using namespace rocks;
using reflection_fixture::TextFieldCaption;
using reflection_fixture::TextFieldHolder;

namespace {

constexpr TextId TitleId{0x7458, 1};
constexpr TextId LineId{0x7458, 2};

const FieldSchema& SchemaField(const TypeSchema& schema, StringView name) noexcept {
    for (const FieldSchema& field : schema.fields) {
        if (StringView(field.name.Data(), field.name.Size()) == name) {
            return field;
        }
    }

    platform::Fatal("fixture field missing");
}

Guid FieldOf(StringView name) noexcept {
    return SchemaField(GeneratedType<TextFieldHolder>::Schema(), name).id;
}

bool RegisterFixture(TypeRegistry& types) noexcept {
    return GeneratedType<TextFieldCaption>::Register(types).IsOk() &&
           GeneratedType<TextFieldHolder>::Register(types).IsOk();
}

RootRef<TextFieldHolder> New(ObjectRegistry& objects) noexcept {
    auto created =
        objects.NewObject<TextFieldHolder>(objects.OwnerAccess(), TypeIdOf<TextFieldHolder>());
    if (!created) {
        platform::Fatal("test object creation failed");
    }

    return Move(created.Value());
}

template <class Edit>
void With(ObjectRegistry& objects, ObjectHandle handle, Edit&& edit) noexcept {
    auto borrowed = objects.Resolve<TextFieldHolder>(objects.OwnerAccess(), handle);
    REQUIRE(borrowed);
    auto value = borrowed.Value().Get();
    REQUIRE(value);
    edit(*value.Value());
}

Text MakeInvariant(Allocator& allocator, StringView source) noexcept {
    auto made = Text::Invariant(allocator, source);
    if (!made) {
        platform::Fatal("test text creation failed");
    }

    return Move(made.Value());
}

Text MakeLocalized(Allocator& allocator, TextId id, uint64 revision, StringView source) noexcept {
    auto made = Text::Localized(allocator, id, revision, source);
    if (!made) {
        platform::Fatal("test text creation failed");
    }

    return Move(made.Value());
}

// text의 영속 형태가 기대와 같은지다.
bool HasForm(const Text& text, TextPersistentForm form, StringView source, TextId id = {},
             uint64 revision = 0) noexcept {
    TextPersistentView view;
    return text.TryGetPersistentForm(view) && view.form == form && view.source == source &&
           view.id == id && view.sourceRevision == revision;
}

// 모든 Text 자리를 채운다: title = Localized(TitleId, 3, "Title"), note 비어 있음, lines =
// [Invariant "a", 비어 있음, Localized(LineId, 9, "b")], fixed = [Invariant "f", 비어 있음], byKey
// = {1: Invariant "x", 2: Localized(LineId, 9, "y")}, caption = {Invariant "cap", 4}.
void Fill(ObjectRegistry& objects, ObjectHandle holder) noexcept {
    Allocator& allocator = objects.ObjectAllocator();
    With(objects, holder, [&](TextFieldHolder& value) noexcept {
        value.title = MakeLocalized(allocator, TitleId, 3, "Title");
        REQUIRE(value.lines.TryPushBack(MakeInvariant(allocator, "a")));
        REQUIRE(value.lines.TryPushBack(Text()));
        REQUIRE(value.lines.TryPushBack(MakeLocalized(allocator, LineId, 9, "b")));
        value.fixed[0] = MakeInvariant(allocator, "f");
        REQUIRE(value.byKey.Insert(1u, MakeInvariant(allocator, "x")));
        REQUIRE(value.byKey.Insert(2u, MakeLocalized(allocator, LineId, 9, "y")));
        value.caption.label = MakeInvariant(allocator, "cap");
        value.caption.order = 4;
    });
}

void CheckFilled(ObjectRegistry& objects, ObjectHandle holder) noexcept {
    With(objects, holder, [](TextFieldHolder& value) noexcept {
        CHECK(HasForm(value.title, TextPersistentForm::Localized, "Title", TitleId, 3));
        CHECK(!value.note.IsValid());
        REQUIRE(value.lines.Size() == 3);
        CHECK(HasForm(value.lines[0], TextPersistentForm::Invariant, "a"));
        CHECK(!value.lines[1].IsValid());
        CHECK(HasForm(value.lines[2], TextPersistentForm::Localized, "b", LineId, 9));
        CHECK(HasForm(value.fixed[0], TextPersistentForm::Invariant, "f"));
        CHECK(!value.fixed[1].IsValid());
        REQUIRE(value.byKey.Count() == 2);
        CHECK(value.byKey.Find(1u) &&
              HasForm(*value.byKey.Find(1u), TextPersistentForm::Invariant, "x"));
        CHECK(value.byKey.Find(2u) &&
              HasForm(*value.byKey.Find(2u), TextPersistentForm::Localized, "y", LineId, 9));
        CHECK(HasForm(value.caption.label, TextPersistentForm::Invariant, "cap"));
        CHECK(value.caption.order == 4u);
    });
}

// title 필드 payload가 content인 TextFieldHolder 그래프다.
Array<uint8> TitleGraph(Allocator& allocator, Span<const uint8> content) noexcept {
    RecordBuilder record(allocator, TypeIdOf<TextFieldHolder>(), TypeOf<TextFieldHolder>().version);
    if (!record.AddField(FieldOf("title"), static_cast<uint32>(WireType::Text), content)) {
        platform::Fatal("test record field add failed");
    }

    auto built = record.Build();
    if (!built) {
        platform::Fatal("test record build failed");
    }

    auto graph = test::graph::Single(allocator, TypeIdOf<TextFieldHolder>(), built.Value().View());
    if (!graph) {
        platform::Fatal("test graph build failed");
    }

    return Move(graph.Value());
}

// 원문 한도(TextCreateLimits 기본 maxSourceBytes)보다 1 byte 긴 ASCII 원문이다.
Array<char> OversizedSource(Allocator& allocator) noexcept {
    Array<char> big(allocator);
    if (!big.TryResize(TextCreateLimits{}.maxSourceBytes + 1)) {
        platform::Fatal("test buffer allocation failed");
    }

    for (usize index = 0; index < big.Size(); ++index) {
        big[index] = 'a';
    }

    return big;
}

FieldValue TextValue(Allocator& allocator, Text text) noexcept {
    FieldValue value(allocator);
    value.kind = ValueKind::Text;
    value.text = Move(text);
    return value;
}

// 지정한 할당 직전에 콜백을 한 번 부르는 allocator다(경로 쓰기 재진입 시험 — B9j–B9m 묶음 리뷰 I2).
class ReentryAllocator final : public Allocator {
public:
    explicit ReentryAllocator(Allocator& backing) noexcept : backing_(backing) {
    }

    Result<void*> Allocate(usize bytes, usize alignment, uint32 tag) noexcept override {
        if (hook) {
            auto run = hook;
            hook = nullptr;
            run(context);
        }

        return backing_.Allocate(bytes, alignment, tag);
    }

    Result<void> Deallocate(void* address) noexcept override {
        return backing_.Deallocate(address);
    }

    void (*hook)(void*) noexcept = nullptr;
    void* context = nullptr;

private:
    Allocator& backing_;
};

// 재진입 콜백이 같은 자리를 지우거나 쓰려 한 결과다.
struct Reentry {
    ObjectRegistry* objects = nullptr;
    ObjectHandle owner{};
    PropertyPath target{};
    PropertyPath container{};
    Allocator* scratch = nullptr;
    ErrorCode written = ErrorCode::None;
    ErrorCode removed = ErrorCode::None;
};

void Reenter(void* context) noexcept {
    Reentry& probe = *static_cast<Reentry*>(context);
    FieldValue other = TextValue(*probe.scratch, MakeInvariant(*probe.scratch, "intruder"));
    auto written =
        PathWrite(*probe.objects, probe.objects->OwnerAccess(), probe.owner, probe.target, other);
    probe.written = written ? ErrorCode::None : written.Error();
    if (probe.container.steps.Size() != 0) {
        auto removed = PathRemoveElement(*probe.objects, probe.objects->OwnerAccess(), probe.owner,
                                         probe.container);
        probe.removed = removed ? ErrorCode::None : removed.Error();
    }
}

} // namespace

// Text 경로 쓰기(Array 원소·struct 멤버·정적 배열 원소)는 고정 아래 새 노드를 만든다: 할당 콜백의
// 재진입 쓰기·원소 삭제는 Busy이고 바깥 쓰기가 그대로 게시된다(B9j–B9m 묶음 리뷰 I2).
TEST(TextGraph_PathWritesPinWhileAllocating) {
    SystemAllocator backing;
    ReentryAllocator hooked(backing);
    TypeRegistry types(backing);
    REQUIRE(RegisterFixture(types));
    {
        ObjectRegistry objects(hooked, types, {0x5447, 9});
        RootRef<TextFieldHolder> holder = New(objects);
        const ObjectHandle handle = holder.Handle();
        PathStep line[] = {{FieldOf("lines"), 0, PathSelector::Index}};
        const FieldValue first = TextValue(backing, MakeInvariant(backing, "first"));
        REQUIRE(
            PathInsertElement(objects, objects.OwnerAccess(), handle, PropertyPath{line}, &first));
        const Guid captionLabel =
            SchemaField(GeneratedType<TextFieldCaption>::Schema(), "label").id;
        const PathStep label[] = {{FieldOf("caption")}, {captionLabel}};
        const PathStep fixed[] = {{FieldOf("fixed"), 1, PathSelector::Index}};
        const PropertyPath targets[] = {PropertyPath{line}, PropertyPath{label},
                                        PropertyPath{fixed}};
        for (const PropertyPath& target : targets) {
            Reentry probe;
            probe.objects = &objects;
            probe.owner = handle;
            probe.target = target;
            probe.container = target.steps.Data() == line ? PropertyPath{line} : PropertyPath{};
            probe.scratch = &backing;
            hooked.context = &probe;
            hooked.hook = &Reenter;
            const FieldValue replacement =
                TextValue(backing, MakeLocalized(backing, TitleId, 4, "outer"));
            REQUIRE(PathWrite(objects, objects.OwnerAccess(), handle, target, replacement));
            CHECK(probe.written == ErrorCode::Busy);
            CHECK(probe.container.steps.Size() == 0 || probe.removed == ErrorCode::Busy);
            FieldValue read(backing);
            REQUIRE(PathRead(objects, objects.OwnerAccess(), handle, target, read));
            CHECK(HasForm(read.text, TextPersistentForm::Localized, "outer", TitleId, 4));
        }

        With(objects, handle,
             [](TextFieldHolder& value) noexcept { CHECK(value.lines.Size() == 1); });
        holder.Reset();
        REQUIRE(objects.Shutdown(objects.OwnerAccess()));
    }

    CHECK(types.Shutdown());
}

// 세 형태가 모든 자리에서 왕복하고 다시 저장한 byte가 같다.
TEST(TextGraph_FormsRoundTripInEveryPosition) {
    SystemAllocator allocator;
    TypeRegistry types(allocator);
    REQUIRE(RegisterFixture(types));
    {
        ObjectRegistry source(allocator, types, {0x5447, 1});
        ObjectRegistry target(allocator, types, {0x5447, 2});
        {
            RootRef<TextFieldHolder> holder = New(source);
            Fill(source, holder.Handle());
            auto saved = SaveObjectGraph(allocator, source, source.OwnerAccess(), holder.Handle());
            REQUIRE(saved);
            auto loaded = LoadObjectGraph(allocator, target, types, target.OwnerAccess(),
                                          saved.Value().View());
            REQUIRE(loaded);
            CheckFilled(target, loaded.Value().Handle());
            auto again =
                SaveObjectGraph(allocator, target, target.OwnerAccess(), loaded.Value().Handle());
            REQUIRE(again);
            CHECK(test::graph::SameBytes(saved.Value().View(), again.Value().View()));
        }

        REQUIRE(source.Shutdown(source.OwnerAccess()));
        REQUIRE(target.Shutdown(target.OwnerAccess()));
    }

    CHECK(types.Shutdown());
}

// Text::Localized의 거부 집합(textId 0·sourceRevision 0)과 Serialization ReadTextWire·
// WriteTextWire의 거부 집합이 같다(1.2절 B9a3 wire 규칙 — 의존 없이 두 곳에 둔 같은 규칙의 표류
// 방지).
TEST(TextGraph_LocalizedRejectionMatchesWireRules) {
    SystemAllocator allocator;
    const TextId ids[] = {TextId{}, TitleId};
    const uint64 revisions[] = {0, 1, NumericLimits<uint64>::Max()};
    for (const TextId id : ids) {
        for (const uint64 revision : revisions) {
            const bool created = Text::Localized(allocator, id, revision, "a").IsOk();

            ArchiveWriter written(allocator);
            TextWireValue wire;
            wire.form = TextWireForm::Localized;
            wire.source = "a";
            wire.textId = id;
            wire.sourceRevision = revision;
            const bool writes = WriteTextWire(written, wire).IsOk();

            ArchiveWriter raw(allocator);
            REQUIRE(raw.WriteU8(static_cast<uint8>(TextWireForm::Localized)));
            REQUIRE(raw.WriteGuid(id));
            REQUIRE(raw.WriteU64(revision));
            REQUIRE(raw.WriteString("a"));
            auto reader = ArchiveReader::Create(raw.View());
            REQUIRE(reader);
            const bool reads = ReadTextWire(reader.Value()).IsOk();

            CHECK(created == writes);
            CHECK(created == reads);
            CHECK(created == (id != TextId{} && revision != 0));
        }
    }

    CHECK(allocator.Stats().liveAllocations == 0);
}

// Format·Integer 이력은 저장하지 않는다(encode Unsupported). 원문 한도(1 MiB)를 넘는 Text는
// 저장도(CapacityExceeded) 복원도(CapacityExceeded) 하지 않는다.
TEST(TextGraph_UnsupportedHistoriesAndSourceLimit) {
    SystemAllocator allocator;
    TypeRegistry types(allocator);
    REQUIRE(RegisterFixture(types));
    {
        ObjectRegistry objects(allocator, types, {0x5447, 3});
        {
            RootRef<TextFieldHolder> holder = New(objects);
            Allocator& owner = objects.ObjectAllocator();
            With(objects, holder.Handle(), [&](TextFieldHolder& value) noexcept {
                auto number = Text::Integer(owner, 7);
                REQUIRE(number);
                value.title = Move(number.Value());
            });
            auto integer =
                SaveObjectGraph(allocator, objects, objects.OwnerAccess(), holder.Handle());
            CHECK(!integer && integer.Error() == ErrorCode::Unsupported);

            With(objects, holder.Handle(), [&](TextFieldHolder& value) noexcept {
                Text pattern = MakeInvariant(owner, "{n}");
                Text argument = MakeInvariant(owner, "x");
                const TextArgument arguments[] = {{"n", &argument}};
                auto formatted = Text::Format(owner, pattern, arguments);
                REQUIRE(formatted);
                value.title.Reset();
                REQUIRE(value.lines.TryPushBack(Move(formatted.Value())));
            });
            auto format =
                SaveObjectGraph(allocator, objects, objects.OwnerAccess(), holder.Handle());
            CHECK(!format && format.Error() == ErrorCode::Unsupported);

            // 원문 한도를 넘는 값은 Text로 만들 수 없으므로(Invariant가 거부한다) 한도를 키워
            // 만든다.
            const Array<char> big = OversizedSource(allocator);
            TextCreateLimits wide;
            wide.maxSourceBytes = 2 * TextCreateLimits{}.maxSourceBytes;
            With(objects, holder.Handle(), [&](TextFieldHolder& value) noexcept {
                value.lines.Clear();
                auto large = Text::Invariant(owner, StringView(big.Data(), big.Size()), wide);
                REQUIRE(large);
                value.title = Move(large.Value());
            });
            auto tooLong =
                SaveObjectGraph(allocator, objects, objects.OwnerAccess(), holder.Handle());
            CHECK(!tooLong && tooLong.Error() == ErrorCode::CapacityExceeded);
        }

        // 복원 쪽: wire가 맞아도 원문이 한도를 넘으면 CapacityExceeded다.
        const Array<char> big = OversizedSource(allocator);
        ArchiveWriter payload(allocator);
        TextWireValue wire;
        wire.form = TextWireForm::Invariant;
        wire.source = StringView(big.Data(), big.Size());
        REQUIRE(WriteTextWire(payload, wire));
        auto graph = TitleGraph(allocator, payload.View());
        auto loaded =
            LoadObjectGraph(allocator, objects, types, objects.OwnerAccess(), graph.View());
        CHECK(!loaded && loaded.Error() == ErrorCode::CapacityExceeded);
        REQUIRE(objects.Shutdown(objects.OwnerAccess()));
    }

    CHECK(types.Shutdown());
}

// 2단계(게시)에서 Text 노드 할당이 실패하면 그래프 전체가 rollback된다(객체를 남기지 않는다).
TEST(TextGraph_ApplyAllocationFailureRollsBack) {
    SystemAllocator allocator;
    TypeRegistry types(allocator);
    REQUIRE(RegisterFixture(types));
    {
        ObjectRegistry source(allocator, types, {0x5447, 4});
        RootRef<TextFieldHolder> holder = New(source);
        Fill(source, holder.Handle());
        auto saved = SaveObjectGraph(allocator, source, source.OwnerAccess(), holder.Handle());
        REQUIRE(saved);
        holder.Reset();
        REQUIRE(source.Shutdown(source.OwnerAccess()));

        // 객체 allocator의 할당 시도마다 한 번씩 실패시킨다: 실패는 OutOfMemory이고 객체를 남기지
        // 않으며, 실패 지점을 지나면 성공한다.
        test::NFailAllocator failing(allocator);
        bool succeeded = false;
        for (uint64 attempt = 1; attempt <= 4096 && !succeeded; ++attempt) {
            ObjectRegistry target(failing, types, {0x5447, 5});
            {
                failing.FailOnAttempt(attempt);
                auto loaded = LoadObjectGraph(allocator, target, types, target.OwnerAccess(),
                                              saved.Value().View());
                failing.DisableFailure();
                if (loaded) {
                    CheckFilled(target, loaded.Value().Handle());
                    succeeded = true;
                } else {
                    CHECK(loaded.Error() == ErrorCode::OutOfMemory);
                    CHECK(target.Stats().liveObjects == 0);
                }
            }

            REQUIRE(target.Shutdown(target.OwnerAccess()));
        }

        CHECK(succeeded);
    }

    CHECK(types.Shutdown());
}

// FieldValue 읽기는 읽는 쪽 allocator로 DeepClone하고, 다른 Registry에 쓰면 그 Registry의 객체
// allocator로 다시 DeepClone한다 — 원래 Registry(와 allocator)를 해제한 뒤에도 값이 유효하다.
TEST(TextGraph_FieldValueReadAndCrossRegistryCopyDeepClone) {
    SystemAllocator types_allocator;
    TypeRegistry types(types_allocator);
    REQUIRE(RegisterFixture(types));
    SystemAllocator reader;
    SystemAllocator targetObjects;
    {
        ObjectRegistry target(targetObjects, types, {0x5447, 7});
        RootRef<TextFieldHolder> copy = New(target);
        FieldValue read(reader);
        {
            SystemAllocator sourceObjects;
            {
                ObjectRegistry source(sourceObjects, types, {0x5447, 6});
                RootRef<TextFieldHolder> holder = New(source);
                Fill(source, holder.Handle());
                const PathStep title[] = {{FieldOf("title")}};
                REQUIRE(PathRead(source, source.OwnerAccess(), holder.Handle(), PropertyPath{title},
                                 read));
                holder.Reset();
                REQUIRE(source.Shutdown(source.OwnerAccess()));
            }

            CHECK(sourceObjects.Stats().liveAllocations == 0);
        }

        CHECK(read.kind == ValueKind::Text);
        CHECK(HasForm(read.text, TextPersistentForm::Localized, "Title", TitleId, 3));
        const PathStep title[] = {{FieldOf("title")}};
        REQUIRE(PathWrite(target, target.OwnerAccess(), copy.Handle(), PropertyPath{title}, read));
        read.text.Reset();
        CHECK(reader.Stats().liveAllocations == 0);
        With(target, copy.Handle(), [](TextFieldHolder& value) noexcept {
            CHECK(HasForm(value.title, TextPersistentForm::Localized, "Title", TitleId, 3));
        });
        copy.Reset();
        REQUIRE(target.Shutdown(target.OwnerAccess()));
    }

    CHECK(types.Shutdown());
}

// 경로 쓰기: 필드·struct 멤버·Array 원소·정적 배열 원소·Map 값에 세 형태를 쓰고 읽는다. Text가
// 아닌 FieldValue는 TypeMismatch다.
TEST(TextGraph_PathWritesEveryPosition) {
    SystemAllocator allocator;
    TypeRegistry types(allocator);
    REQUIRE(RegisterFixture(types));
    {
        ObjectRegistry objects(allocator, types, {0x5447, 8});
        RootRef<TextFieldHolder> holder = New(objects);
        const ObjectHandle handle = holder.Handle();
        const Guid captionLabel =
            SchemaField(GeneratedType<TextFieldCaption>::Schema(), "label").id;
        const PathStep note[] = {{FieldOf("note")}};
        const PathStep label[] = {{FieldOf("caption")}, {captionLabel}};
        const PathStep fixed[] = {{FieldOf("fixed"), 1, PathSelector::Index}};
        PathStep line[] = {{FieldOf("lines"), 0, PathSelector::Index}};
        // 단일 값 원소의 삽입은 값을 받는다(원소 allocator로 다시 만든다).
        const FieldValue inserted = TextValue(allocator, MakeInvariant(allocator, "i"));
        REQUIRE(PathInsertElement(objects, objects.OwnerAccess(), handle, PropertyPath{line},
                                  &inserted));
        With(objects, handle, [](TextFieldHolder& value) noexcept {
            REQUIRE(value.lines.Size() == 1);
            CHECK(HasForm(value.lines[0], TextPersistentForm::Invariant, "i"));
        });
        REQUIRE(PathWrite(objects, objects.OwnerAccess(), handle, PropertyPath{note},
                          TextValue(allocator, MakeInvariant(allocator, "n"))));
        REQUIRE(PathWrite(objects, objects.OwnerAccess(), handle, PropertyPath{label},
                          TextValue(allocator, MakeLocalized(allocator, TitleId, 2, "l"))));
        REQUIRE(PathWrite(objects, objects.OwnerAccess(), handle, PropertyPath{fixed},
                          TextValue(allocator, MakeInvariant(allocator, "s"))));
        REQUIRE(PathWrite(objects, objects.OwnerAccess(), handle, PropertyPath{line},
                          TextValue(allocator, MakeLocalized(allocator, LineId, 5, "e"))));
        With(objects, handle, [](TextFieldHolder& value) noexcept {
            CHECK(HasForm(value.note, TextPersistentForm::Invariant, "n"));
            CHECK(HasForm(value.caption.label, TextPersistentForm::Localized, "l", TitleId, 2));
            CHECK(HasForm(value.fixed[1], TextPersistentForm::Invariant, "s"));
            REQUIRE(value.lines.Size() == 1);
            CHECK(HasForm(value.lines[0], TextPersistentForm::Localized, "e", LineId, 5));
        });

        // 비어 있음 쓰기와 종류 불일치.
        REQUIRE(PathWrite(objects, objects.OwnerAccess(), handle, PropertyPath{note},
                          TextValue(allocator, Text())));
        With(objects, handle,
             [](TextFieldHolder& value) noexcept { CHECK(!value.note.IsValid()); });
        FieldValue wrong(allocator);
        wrong.kind = ValueKind::Utf8String;
        CHECK(!PathWrite(objects, objects.OwnerAccess(), handle, PropertyPath{note}, wrong));

        FieldValue read(allocator);
        REQUIRE(PathRead(objects, objects.OwnerAccess(), handle, PropertyPath{label}, read));
        CHECK(HasForm(read.text, TextPersistentForm::Localized, "l", TitleId, 2));
        holder.Reset();
        REQUIRE(objects.Shutdown(objects.OwnerAccess()));
    }

    CHECK(types.Shutdown());
}
