#include <ReflectionTestFixtures/NotifyFixture.generated.h>
#include <CoreTypeBindings/CoreValueTypes.generated.h>
#include <Base/Compiler.h>
#include <Core/Containers/OrderedMap.h>
#include <Core/Containers/OrderedSet.h>
#include <Core/Memory/SystemAllocator.h>
#include <Object/ConstructionContext.h>
#include <Object/FieldNotifications.h>
#include <Object/ObjectAccessContext.h>
#include <Object/ObjectRegistry.h>
#include <Object/TypeRegistry.h>
#include <Object/ValueSame.h>
#include <TestSupport/NFailAllocator.h>
#include <TestSupport/Test.h>

#include <Platform/PlatformMath.h>

// 생성 setter·getter와 코드 통지 시험이다(Docs/Design/ChangeNotification.ko.md CN2a — CN-D9·CN-D13·
// CN-D15): 값 같음 규칙(실수 비트·Name 표시 번호·Map 순서·Optional·정적 배열·weak null·Soft·생성
// struct·NATIVE struct·InstancedStruct), 생성 setter의 같은 값 생략·대입·Apply·통지, 통지하지 않는
// 필드, 생성자 안(Registry 없음)의 setter, SetAndNotify(값·할당 값·NATIVE
// struct)·NotifyChanged(필드· virtual 계산 값 함수), 파생 객체의 기반 멤버 통지, 다른 문맥의 대입
// 전 거부.
using namespace rocks;
using reflection_fixture::NotifyCached;
using reflection_fixture::NotifyStyle;
using reflection_fixture::NotifyWidget;
using reflection_fixture::NotifyWidgetChild;

namespace field_notification_setter_tests_file {

namespace {

constexpr uint32 MaxRecorded = 32;

Guid IdOf(TypeRegistry& types, TypeId type, const char* name) noexcept {
    auto lease = types.Acquire(type);
    if (!lease) {
        return Guid{};
    }

    for (const FieldSchema& field : lease.Value().Schema().fields) {
        if (StringView(field.name.Data(), field.name.Size()) == StringView(name)) {
            return field.id;
        }
    }

    for (const FunctionSchema& function : lease.Value().Schema().functions) {
        if (StringView(function.name.Data(), function.name.Size()) == StringView(name)) {
            return function.id;
        }
    }

    return Guid{};
}

template <class T>
ErrorCode ErrorOf(const Result<T>& result) noexcept {
    return result ? ErrorCode::None : result.Error();
}

struct Recorder {
    Guid ids[MaxRecorded]{};
    uint32 count = 0;

    void Add(Guid id) noexcept {
        REQUIRE(count < MaxRecorded);
        ids[count++] = id;
    }

    uint32 CountOf(Guid id) const noexcept {
        uint32 found = 0;
        for (uint32 index = 0; index < count; ++index) {
            found += ids[index] == id ? 1 : 0;
        }

        return found;
    }
};

struct Fixture {
    explicit Fixture(Allocator& allocator_) noexcept
        : allocator(allocator_), types(allocator_), registry(allocator_, types, {0x4e535452, 1}) {
    }

    bool Start() noexcept {
        if (!GeneratedType<reflection_fixture::ValidRange>::Register(types) ||
            !GeneratedType<NotifyStyle>::Register(types) ||
            !GeneratedType<NotifyCached>::Register(types) ||
            !GeneratedType<NotifyWidget>::Register(types) ||
            !GeneratedType<NotifyWidgetChild>::Register(types)) {
            return false;
        }

        const TypeId widget = TypeIdOf<NotifyWidget>();
        label = IdOf(types, widget, "label");
        scale = IdOf(types, widget, "scale");
        style = IdOf(types, widget, "style");
        caption = IdOf(types, widget, "caption");
        count = IdOf(types, widget, "count");
        values = IdOf(types, widget, "values");
        cached = IdOf(types, widget, "cached");
        total = IdOf(types, widget, "Total");
        level = IdOf(types, TypeIdOf<NotifyWidgetChild>(), "level");
        return label != Guid{} && total != Guid{} && level != Guid{};
    }

    template <class T>
    RootRef<T> Make() noexcept {
        auto created = registry.NewObject<T>(access, TypeIdOf<T>(), {}, {});
        return created ? Move(created.Value()) : RootRef<T>();
    }

    FieldNotifyHandle Watch(ObjectHandle object, Recorder& recorder) noexcept {
        auto handle = registry.TrySubscribeAll(
            access, object,
            FieldChangeDelegate::CreateLambda(
                [&recorder](ObjectHandle, Guid id) noexcept { recorder.Add(id); }));
        CHECK(handle);
        return handle ? handle.Value() : FieldNotifyHandle{};
    }

    Allocator& allocator;
    TypeRegistry types;
    ObjectRegistry registry;
    ObjectAccessContext access = registry.OwnerAccess();
    Guid label{};
    Guid scale{};
    Guid style{};
    Guid caption{};
    Guid count{};
    Guid values{};
    Guid cached{};
    Guid total{};
    Guid level{};
};

} // namespace

// 값 같음(CN-D15): 실수는 비트(0.0과 -0.0은 다르고 같은 NaN은 같다), Name은 표시 번호까지,
// Map·Set은 순서까지, Optional은 유무와 값, 정적 배열은 원소별, weak 참조의 null은 서로 같고,
// Soft는 자산과 경로, 생성 struct는 PROPERTY 멤버별, NATIVE 멤버가 있는 struct와 InstancedStruct는
// 늘 다르다.
TEST(FieldNotificationSetter_ValueSameRules) {
    SystemAllocator allocator;
    {
        CHECK(detail::SameValue(1.5f, 1.5f));
        CHECK(!detail::SameValue(0.0f, -0.0f));
        CHECK(!detail::SameValue(0.0, -0.0));
        const float nan = platform::PlatformMath::QuietNaN<float>();
        CHECK(detail::SameValue(nan, nan));
        CHECK(detail::SameValue(int32{7}, int32{7}) && !detail::SameValue(int32{7}, int32{8}));
        CHECK(detail::SameValue(true, true) && !detail::SameValue(true, false));
        CHECK(detail::SameValue(Guid{1, 2}, Guid{1, 2}) &&
              !detail::SameValue(Guid{1, 2}, Guid{1, 3}));

        // 대소문자만 다른 두 표시는 Name ==로 같지만 저장 표시가 다르므로 다르다.
        Name weapon{};
        weapon.domain = 1;
        weapon.id = 5;
        weapon.displayId = 9;
        weapon.kind = NameKind::Normal;
        Name upper = weapon;
        upper.displayId = 10;
        CHECK(weapon == upper);
        CHECK(!detail::SameValue(weapon, upper));
        CHECK(detail::SameValue(weapon, weapon));

        String first(allocator);
        String second(allocator);
        REQUIRE(first.TryAssign("abc") && second.TryAssign("abc"));
        CHECK(detail::SameValue(first, second));
        REQUIRE(second.TryAssign("abd"));
        CHECK(!detail::SameValue(first, second));

        Array<int32> left(allocator);
        Array<int32> right(allocator);
        REQUIRE(left.TryPushBack(1) && right.TryPushBack(1));
        CHECK(detail::SameValue(left, right));
        REQUIRE(right.TryPushBack(2));
        CHECK(!detail::SameValue(left, right));

        OrderedMap<int32, int32> ordered(allocator);
        OrderedMap<int32, int32> reordered(allocator);
        REQUIRE(ordered.Insert(1, 10) && ordered.Insert(2, 20));
        REQUIRE(reordered.Insert(2, 20) && reordered.Insert(1, 10));
        CHECK(!detail::SameValue(ordered, reordered));
        OrderedMap<int32, int32> again(allocator);
        REQUIRE(again.Insert(1, 10) && again.Insert(2, 20));
        CHECK(detail::SameValue(ordered, again));
        OrderedSet<int32> set(allocator);
        OrderedSet<int32> otherSet(allocator);
        REQUIRE(set.Add(1) && set.Add(2) && otherSet.Add(2) && otherSet.Add(1));
        CHECK(!detail::SameValue(set, otherSet));

        Optional<int32> none;
        Optional<int32> some(3);
        CHECK(detail::SameValue(none, Optional<int32>{}) && !detail::SameValue(none, some));
        CHECK(detail::SameValue(some, Optional<int32>(3)));

        const int32 arrayA[3] = {1, 2, 3};
        const int32 arrayB[3] = {1, 2, 4};
        CHECK(detail::SameValue(arrayA, arrayA) && !detail::SameValue(arrayA, arrayB));

        // weak 참조: null인 두 handle(세대만 다른 비정규 null 포함)은 같다.
        WeakObjectRef<NotifyWidget> nullWeak;
        CHECK(detail::SameValue(nullWeak, WeakObjectRef<NotifyWidget>{}));

        SoftObjectRef<NotifyWidget> softA(allocator);
        SoftObjectRef<NotifyWidget> softB(allocator);
        softA.asset = AssetId{Guid{4, 5}};
        softB.asset = AssetId{Guid{4, 5}};
        REQUIRE(softA.objectPath.TryAssign("Root.Child") &&
                softB.objectPath.TryAssign("Root.Child"));
        CHECK(detail::SameValue(softA, softB));
        REQUIRE(softB.objectPath.TryAssign("Root.Other"));
        CHECK(!detail::SameValue(softA, softB));

        NotifyStyle styleA(allocator);
        NotifyStyle styleB(allocator);
        REQUIRE(styleA.font.TryAssign("Serif") && styleB.font.TryAssign("Serif"));
        CHECK(detail::ValueSame<NotifyStyle>::Supported && detail::SameValue(styleA, styleB));
        styleB.size = -0.0f;
        CHECK(!detail::SameValue(styleA, styleB));

        NotifyCached cachedA;
        NotifyCached cachedB;
        CHECK(!detail::ValueSame<NotifyCached>::Supported && !detail::SameValue(cachedA, cachedB));
        CHECK(!detail::ValueSame<InstancedStruct<>>::Supported);
    }

    CHECK(allocator.Stats().liveAllocations == 0);
}

// 생성 setter(CN-D13): 같은 값이면 대입·Apply·통지가 없고, 바뀌면 대입 → Apply → 통지 한 번이다.
// 문자열(Apply 있음)·실수(비트 비교)·struct(멤버별)·Text(이력 비교) setter와 통지하지 않는 필드의
// setter(값만). getter는 필드를 그대로 돌려준다.
TEST(FieldNotificationSetter_GeneratedSetterAssignsAppliesAndNotifies) {
    SystemAllocator allocator;
    {
        Fixture fixture(allocator);
        REQUIRE(fixture.Start());
        auto root = fixture.Make<NotifyWidget>();
        REQUIRE(!root.Handle().IsNull());
        Recorder recorder;
        (void)fixture.Watch(root.Handle(), recorder);
        auto borrowed = fixture.registry.Resolve<NotifyWidget>(fixture.access, root.Handle());
        REQUIRE(borrowed);
        NotifyWidget* widget = borrowed.Value().Get().Value();

        String text(allocator);
        REQUIRE(text.TryAssign("Hello"));
        REQUIRE(widget->SetLabel(text));
        CHECK(widget->GetLabel().View() == "Hello");
        CHECK(widget->relayouts == 1 && recorder.CountOf(fixture.label) == 1);
        REQUIRE(widget->SetLabel(text));
        CHECK(widget->relayouts == 1 && recorder.CountOf(fixture.label) == 1);

        // 생성자가 2.0으로 바꿔 두었다(Registry 없이 값만).
        CHECK(widget->scale == 2.0f);
        REQUIRE(widget->SetScale(0.0f));
        REQUIRE(widget->SetScale(-0.0f));
        CHECK(recorder.CountOf(fixture.scale) == 2);
        REQUIRE(widget->SetScale(platform::PlatformMath::QuietNaN<float>()));
        REQUIRE(widget->SetScale(platform::PlatformMath::QuietNaN<float>()));
        CHECK(recorder.CountOf(fixture.scale) == 3);

        NotifyStyle style(allocator);
        REQUIRE(style.font.TryAssign("Mono"));
        style.size = 14.0f;
        REQUIRE(widget->SetStyle(style));
        CHECK(widget->style.font.View() == "Mono" && widget->style.size == 14.0f);
        REQUIRE(widget->SetStyle(style));
        CHECK(recorder.CountOf(fixture.style) == 1);

        auto caption = Text::Invariant(allocator, "Title");
        auto captionAgain = Text::Invariant(allocator, "Title");
        REQUIRE(caption && captionAgain);
        REQUIRE(widget->SetCaption(caption.Value()));
        REQUIRE(widget->SetCaption(captionAgain.Value()));
        CHECK(recorder.CountOf(fixture.caption) == 1);
        CHECK(widget->caption.IsIdentical(caption.Value()));

        const uint32 before = recorder.count;
        REQUIRE(widget->SetQuiet(5));
        CHECK(widget->quiet == 5 && recorder.count == before);
    }

    CHECK(allocator.Stats().liveAllocations == 0);
}

// 생성자 안(Registry 없음 — CN-D9)의 setter는 통지 없이 값만 바꾸고, 사본 allocator가 필요한
// Text는 Unsupported(필드 그대로)다.
TEST(FieldNotificationSetter_ConstructorSetterHasNoRegistry) {
    SystemAllocator allocator;
    {
        NotifyWidget widget(allocator);
        CHECK(widget.constructorScale && widget.scale == 2.0f);
        CHECK(ErrorOf(widget.constructorCaption) == ErrorCode::Unsupported);
        Text empty;
        CHECK(widget.caption.IsIdentical(empty));
        // Registry 밖 인스턴스의 코드 통지도 Success다.
        CHECK(widget.TouchScale());
        auto changed = widget.PutCount(4);
        CHECK(changed && changed.Value() && widget.count == 4);
    }

    CHECK(allocator.Stats().liveAllocations == 0);
}

// SetAndNotify(CN-D9)는 같으면 false·통지 없음, 바뀌면 true·통지 한 번이고, 할당 값(배열)은 사본 뒤
// 교환한다. NATIVE 멤버가 있는 struct는 늘 다르다. NotifyChanged는 Notify 필드와 virtual 계산 값
// 함수를 알린다.
TEST(FieldNotificationSetter_SetAndNotifyAndNotifyChanged) {
    SystemAllocator allocator;
    {
        Fixture fixture(allocator);
        REQUIRE(fixture.Start());
        auto root = fixture.Make<NotifyWidget>();
        REQUIRE(!root.Handle().IsNull());
        Recorder recorder;
        (void)fixture.Watch(root.Handle(), recorder);
        auto borrowed = fixture.registry.Resolve<NotifyWidget>(fixture.access, root.Handle());
        REQUIRE(borrowed);
        NotifyWidget* widget = borrowed.Value().Get().Value();

        auto unchanged = widget->PutCount(0);
        CHECK(unchanged && !unchanged.Value() && recorder.CountOf(fixture.count) == 0);
        auto changed = widget->PutCount(3);
        CHECK(changed && changed.Value() && widget->GetCount() == 3);
        CHECK(recorder.CountOf(fixture.count) == 1);

        Array<int32> next(allocator);
        REQUIRE(next.TryPushBack(1) && next.TryPushBack(2));
        auto put = widget->PutValues(next);
        CHECK(put && put.Value() && widget->values.Size() == 2 && widget->values[1] == 2);
        auto same = widget->PutValues(next);
        CHECK(same && !same.Value() && recorder.CountOf(fixture.values) == 1);

        NotifyCached cache;
        CHECK(widget->PutCached(cache) && widget->PutCached(cache));
        CHECK(recorder.CountOf(fixture.cached) == 2);

        REQUIRE(widget->TouchTotal());
        REQUIRE(widget->TouchScale());
        CHECK(recorder.CountOf(fixture.total) == 1 && recorder.CountOf(fixture.scale) == 1);
    }

    CHECK(allocator.Stats().liveAllocations == 0);
}

// 파생 객체에서 기반 멤버를 알리면 기반의 생성 NotifyId가 답한다(같은 ID). 재정의한 virtual 계산 값
// 함수는 선언한 기반의 이름으로 알린다. 파생 자기 필드도 알린다.
TEST(FieldNotificationSetter_DerivedObjectNotifiesBaseMembers) {
    SystemAllocator allocator;
    {
        Fixture fixture(allocator);
        REQUIRE(fixture.Start());
        auto root = fixture.Make<NotifyWidgetChild>();
        REQUIRE(!root.Handle().IsNull());
        Recorder recorder;
        (void)fixture.Watch(root.Handle(), recorder);
        auto borrowed = fixture.registry.Resolve<NotifyWidgetChild>(fixture.access, root.Handle());
        REQUIRE(borrowed);
        NotifyWidgetChild* child = borrowed.Value().Get().Value();

        auto base = child->PutBaseCount(2);
        auto own = child->PutLevel(7);
        CHECK(base && base.Value() && own && own.Value());
        REQUIRE(child->TouchBaseTotal());
        REQUIRE(child->SetQuiet(1));
        CHECK(recorder.CountOf(fixture.count) == 1 && recorder.CountOf(fixture.level) == 1);
        CHECK(recorder.CountOf(fixture.total) == 1 && child->Total() == 10);
    }

    CHECK(allocator.Stats().liveAllocations == 0);
}

// 통지할 쓰기는 대입 전에 접근 문맥을 본다(CN-D9 — 다른 문맥이면 WrongContext, 필드·Apply 그대로).
// 통지하지 않는 필드의 setter는 Registry를 보지 않는다.
TEST(FieldNotificationSetter_WrongContextRejectedBeforeAssignment) {
    SystemAllocator allocator;
    {
        Fixture fixture(allocator);
        REQUIRE(fixture.Start());
        auto root = fixture.Make<NotifyWidget>();
        REQUIRE(!root.Handle().IsNull());
        auto borrowed = fixture.registry.Resolve<NotifyWidget>(fixture.access, root.Handle());
        REQUIRE(borrowed);
        NotifyWidget* widget = borrowed.Value().Get().Value();
        String text(allocator);
        REQUIRE(text.TryAssign("Other"));
        {
            ObjectAccessScope wrong(ObjectAccessContext{ContextId{0x4e535452, 9}});
            CHECK(ErrorOf(widget->SetLabel(text)) == ErrorCode::WrongContext);
            CHECK(ErrorOf(widget->PutCount(9)) == ErrorCode::WrongContext);
            CHECK(ErrorOf(widget->TouchTotal()) == ErrorCode::WrongContext);
            CHECK(widget->SetQuiet(3));
        }

        CHECK(widget->label.View().IsEmpty() && widget->relayouts == 0);
        CHECK(widget->count == 0 && widget->quiet == 3);
    }

    CHECK(allocator.Stats().liveAllocations == 0);
}

// 사본 할당이 실패하면(CN-D13) 그 오류이고 필드·Apply·통지가 모두 그대로다: 문자열·struct·Text·
// 배열(SetAndNotify)·Map·Optional<String>. 실패를 풀면 같은 쓰기가 성공한다.
TEST(FieldNotificationSetter_CopyFailureLeavesFieldUnchanged) {
    SystemAllocator system;
    {
        test::NFailAllocator allocator(system);
        Fixture fixture(allocator);
        REQUIRE(fixture.Start());
        auto root = fixture.Make<NotifyWidget>();
        REQUIRE(!root.Handle().IsNull());
        Recorder recorder;
        (void)fixture.Watch(root.Handle(), recorder);
        auto borrowed = fixture.registry.Resolve<NotifyWidget>(fixture.access, root.Handle());
        REQUIRE(borrowed);
        NotifyWidget* widget = borrowed.Value().Get().Value();

        // 짧은 문자열의 내부 버퍼를 넘는 길이라 사본이 할당한다.
        String text(system);
        REQUIRE(text.TryAssign("a label long enough to need a heap buffer for its copy, surely"));
        NotifyStyle style(system);
        REQUIRE(style.font.TryAssign("a font name long enough to need a heap buffer for its copy"));
        auto caption = Text::Invariant(system, "caption");
        REQUIRE(caption);
        Array<int32> values(system);
        REQUIRE(values.TryPushBack(1));
        OrderedMap<int32, int32> scores(system);
        REQUIRE(scores.Insert(1, 10));
        Optional<String> nickname{String(system)};
        REQUIRE(
            nickname.Value().TryAssign("a nickname long enough to need a heap buffer for copy"));

        allocator.FailOnAttempt(1);
        CHECK(ErrorOf(widget->SetLabel(text)) == ErrorCode::OutOfMemory);
        allocator.FailOnAttempt(1);
        CHECK(ErrorOf(widget->SetStyle(style)) == ErrorCode::OutOfMemory);
        allocator.FailOnAttempt(1);
        CHECK(ErrorOf(widget->SetCaption(caption.Value())) == ErrorCode::OutOfMemory);
        allocator.FailOnAttempt(1);
        CHECK(ErrorOf(widget->PutValues(values)) == ErrorCode::OutOfMemory);
        allocator.FailOnAttempt(1);
        CHECK(ErrorOf(widget->SetScores(scores)) == ErrorCode::OutOfMemory);
        allocator.FailOnAttempt(1);
        CHECK(ErrorOf(widget->SetNickname(nickname)) == ErrorCode::OutOfMemory);
        allocator.DisableFailure();
        CHECK(widget->label.View().IsEmpty() && widget->style.font.View().IsEmpty());
        CHECK(!widget->caption.IsValid() && widget->values.Size() == 0);
        CHECK(widget->scores.Count() == 0 && !widget->nickname.HasValue());
        CHECK(widget->relayouts == 0 && recorder.count == 0);

        REQUIRE(widget->SetLabel(text));
        CHECK(widget->label.View() == text.View() && widget->relayouts == 1 && recorder.count == 1);
    }

    CHECK(system.Stats().liveAllocations == 0);
}

// 필드 자신의 allocator로 사본을 만드는 setter(Soft 경로·Optional<String>·Map — CN-D13)와 Map 순서
// 비교(같은 내용의 다른 순서는 바뀐 값), EditorOnly 필드의 setter·getter(통지 없음).
TEST(FieldNotificationSetter_AllocatorPathsAndOrderedMaps) {
    SystemAllocator allocator;
    {
        Fixture fixture(allocator);
        REQUIRE(fixture.Start());
        auto root = fixture.Make<NotifyWidget>();
        REQUIRE(!root.Handle().IsNull());
        Recorder recorder;
        (void)fixture.Watch(root.Handle(), recorder);
        auto borrowed = fixture.registry.Resolve<NotifyWidget>(fixture.access, root.Handle());
        REQUIRE(borrowed);
        NotifyWidget* widget = borrowed.Value().Get().Value();
        const Guid link = IdOf(fixture.types, TypeIdOf<NotifyWidget>(), "link");
        const Guid nickname = IdOf(fixture.types, TypeIdOf<NotifyWidget>(), "nickname");
        const Guid scores = IdOf(fixture.types, TypeIdOf<NotifyWidget>(), "scores");

        SoftObjectRef<reflection_fixture::NotifyModel> soft(allocator);
        soft.asset = AssetId{Guid{7, 8}};
        REQUIRE(soft.objectPath.TryAssign("Root.Model"));
        REQUIRE(widget->SetLink(soft));
        REQUIRE(widget->SetLink(soft));
        CHECK(widget->link.objectPath.View() == "Root.Model" && recorder.CountOf(link) == 1);

        Optional<String> name{String(allocator)};
        REQUIRE(name.Value().TryAssign("Ace"));
        REQUIRE(widget->SetNickname(name));
        REQUIRE(widget->SetNickname(name));
        REQUIRE(widget->SetNickname(Optional<String>{}));
        CHECK(!widget->nickname.HasValue() && recorder.CountOf(nickname) == 2);

        OrderedMap<int32, int32> ordered(allocator);
        OrderedMap<int32, int32> reordered(allocator);
        REQUIRE(ordered.Insert(1, 10) && ordered.Insert(2, 20));
        REQUIRE(reordered.Insert(2, 20) && reordered.Insert(1, 10));
        REQUIRE(widget->SetScores(ordered));
        REQUIRE(widget->SetScores(ordered));
        REQUIRE(widget->SetScores(reordered));
        CHECK(widget->scores.Count() == 2 && recorder.CountOf(scores) == 2);

#if ROCKS_WITH_EDITOR_DATA
        const uint32 before = recorder.count;
        REQUIRE(widget->SetNote(4));
        CHECK(widget->GetNote() == 4 && recorder.count == before);
#endif
    }

    CHECK(allocator.Stats().liveAllocations == 0);
}

// 객체 상태(CN-D9): 게시 전(Reserved — ConstructionContext)과 파괴 요청된 객체의 setter는 값만
// 바꾸고 Success다. 통지 묶음 안의 두 setter는 묶음이 끝날 때 한 번 알린다. 구독자가 같은 setter를
// 다시 부르면(재진입) 중첩 통지가 한 번 더 간다.
TEST(FieldNotificationSetter_ObjectStatesGroupsAndReentry) {
    SystemAllocator allocator;
    {
        Fixture fixture(allocator);
        REQUIRE(fixture.Start());
        {
            ConstructionContext construction(fixture.registry, fixture.access);
            auto created = construction.Create<NotifyWidget>(TypeIdOf<NotifyWidget>());
            REQUIRE(created);
            auto staged = construction.Get<NotifyWidget>(created.Value());
            REQUIRE(staged);
            CHECK(staged.Value()->SetScale(3.0f));
            CHECK(staged.Value()->scale == 3.0f);
            auto committed = construction.Commit();
            REQUIRE(committed);
        }

        auto root = fixture.Make<NotifyWidget>();
        REQUIRE(!root.Handle().IsNull());
        Recorder recorder;
        (void)fixture.Watch(root.Handle(), recorder);
        auto borrowed = fixture.registry.Resolve<NotifyWidget>(fixture.access, root.Handle());
        REQUIRE(borrowed);
        NotifyWidget* widget = borrowed.Value().Get().Value();

        {
            auto group = fixture.registry.BeginNotifyGroup(fixture.access);
            REQUIRE(group);
            REQUIRE(widget->SetScale(4.0f));
            REQUIRE(widget->SetScale(5.0f));
            CHECK(recorder.CountOf(fixture.scale) == 0);
        }

        CHECK(recorder.CountOf(fixture.scale) == 1 && widget->scale == 5.0f);

        // 구독자가 한 번 더 같은 setter를 부른다(다른 값 — 중첩 통지, 그다음은 같은 값이라 멈춘다).
        auto again = fixture.registry.TrySubscribe(
            fixture.access, root.Handle(), {fixture.scale},
            FieldChangeDelegate::CreateLambda(
                [widget](ObjectHandle, Guid) noexcept { (void)widget->SetScale(9.0f); }));
        REQUIRE(again);
        REQUIRE(widget->SetScale(6.0f));
        CHECK(widget->scale == 9.0f && recorder.CountOf(fixture.scale) == 3);
        CHECK(fixture.registry.Unsubscribe(fixture.access, again.Value()));

        REQUIRE(widget->Destroy());
        CHECK(widget->SetScale(7.0f));
        CHECK(widget->scale == 7.0f && recorder.CountOf(fixture.scale) == 3);
    }

    CHECK(allocator.Stats().liveAllocations == 0);
}

// Binds 미러의 실제 Core 값 타입도 생성 특수화로 실수를 비트 비교한다(CN-D15).
TEST(FieldNotificationSetter_BoundCoreValuesCompareBits) {
    CHECK(detail::ValueSame<Vec3>::Supported);
    CHECK(detail::SameValue(Vec3(1.0f, 2.0f, 3.0f), Vec3(1.0f, 2.0f, 3.0f)));
    CHECK(!detail::SameValue(Vec3(0.0f, 0.0f, 0.0f), Vec3(-0.0f, 0.0f, 0.0f)));
    const float nan = platform::PlatformMath::QuietNaN<float>();
    CHECK(detail::SameValue(Vec3(nan, 0.0f, 0.0f), Vec3(nan, 0.0f, 0.0f)));
}

} // namespace field_notification_setter_tests_file
