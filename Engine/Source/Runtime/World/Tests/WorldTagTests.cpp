#include <Base/InitializerList.h>
#include <Core/Memory/SystemAllocator.h>
#include <Core/Memory/UniquePtr.h>
#include <Core/Text/NamePool.h>
#include <Meta/TypeSchema.h>
#include <ObjectSerialization/ObjectGraph.h>
#include <TestSupport/Test.h>
#include <World/Actor.generated.h>
#include <World/Component.generated.h>
#include <World/Level.generated.h>
#include <World/Level.h>
#include <World/SceneComponent.generated.h>
#include <World/World.h>
#include <WorldTestFixtures/SaveFixture.generated.h>
#include <WorldTestFixtures/WorldTestFixtures.module.generated.h>

// Actor·Component 태그와 태그 찾기 시험이다(Docs/Design/EngineRuntime 31.4·31.5·31.7절):
// 더하기(새·이미 있음)·빼기(여럿·없음)·통째 바꾸기(중복 보존), None·다른 pool·옛 토큰 거부(값
// 그대로), pool 없는 World Unsupported, 파괴 대기 Destroyed, 장부 밖 객체 InvalidArgument,
// 찾기(다른 Level·지연 Spawn 포함·파괴 대기 제외·타입 필터·null 타입·None), 리플렉션 필드와 저장
// 왕복.
using namespace rocks;
using world_fixture::SaveActor;
using world_fixture::SaveComponent;
namespace world_tag_tests_file {

namespace {

constexpr uint64 TagContextHigh = 0x5441474full;
constexpr uint64 LoadedContextHigh = 0x5441474cull;

class TagActor final : public Actor {
    ROCKS_NATIVE_TYPE(TagActor, Actor, 0x3c81f5a2e9d04b67ull, 0x7a0e4d91c25bf836ull)

public:
    explicit TagActor(Allocator& allocator) noexcept : Actor(allocator) {
    }
};

class TagPart : public Component {
    ROCKS_NATIVE_TYPE(TagPart, Component, 0xa5d2e81c4f7b3069ull, 0x1e94c07b5d2a8f63ull)

public:
    using Component::Component;
};

class TagPartDerived final : public TagPart {
    ROCKS_NATIVE_TYPE(TagPartDerived, TagPart, 0x6f30b9d27e4c1a85ull, 0xc8572e0a39f1d64bull)

public:
    using TagPart::TagPart;
};

class OtherPart final : public Component {
    ROCKS_NATIVE_TYPE(OtherPart, Component, 0x0b47e6c93d2a5f18ull, 0x94d1a5e7086c3b2full)

public:
    using Component::Component;
};

// 태그 시험 World다. names가 null이면 이름 pool 없는 World다.
struct TagWorld {
    explicit TagWorld(bool withNames = true, uint64 context = 1) noexcept
        : types(allocator), modules(types, allocator), names(allocator), foreign(allocator) {
        registered = names.Initialize().IsOk() && foreign.Initialize().IsOk() &&
                     RegisterWorldTypes(modules).IsOk() &&
                     generated::WorldTestFixtures::RegisterModuleTypes(modules).IsOk() &&
                     Register<TagActor>() && Register<TagPart>() && Register<TagPartDerived>() &&
                     Register<OtherPart>();
        WorldDesc desc;
        desc.name = "Tags";
        desc.context = {TagContextHigh, context};
        desc.timeStep = {1.0 / 60.0, 0.25, 8};
        desc.objects.names = withNames ? &names : nullptr;
        auto created = UniquePtr<World>::TryMake(allocator, allocator, types, desc);
        if (created) {
            world = Move(created.Value());
        }
    }

    ~TagWorld() {
        if (world) {
            CHECK(world->Shutdown().IsOk());
        }

        world.Reset();
        while (tokenCount != 0) {
            CHECK(types.Unregister(tokens[--tokenCount]).IsOk());
        }

        CHECK(modules.Unregister().IsOk());
        CHECK(names.Shutdown().IsOk());
        CHECK(foreign.Shutdown().IsOk());
    }

    template <class T>
    bool Register() noexcept {
        auto token = RegisterNativeClass<T>(types);
        if (!token || tokenCount == MaxTokens) {
            return false;
        }

        tokens[tokenCount++] = token.Value();
        return true;
    }

    bool Ready() noexcept {
        return registered && world && world->Initialize().IsOk();
    }

    Name Intern(const char* text) noexcept {
        auto made = names.TryIntern(text);
        return made ? made.Value() : Name{};
    }

    ObjectHandle Spawn() noexcept {
        auto actor = world->SpawnActor<TagActor>();
        return actor ? actor.Value() : ObjectHandle{};
    }

    template <class T>
    ObjectHandle Add(ObjectHandle actor) noexcept {
        auto component = world->AddComponent<T>(actor);
        return component ? component.Value() : ObjectHandle{};
    }

    // actor에 tags를 차례로 더한다.
    bool TagActorWith(ObjectHandle actor, std::initializer_list<Name> tags) noexcept {
        auto done = world->With<Actor>(actor, [&](Actor& value) noexcept {
            for (const Name tag : tags) {
                if (!value.AddTag(tag)) {
                    return false;
                }
            }

            return true;
        });
        return done && done.Value();
    }

    bool TagComponentWith(ObjectHandle component, std::initializer_list<Name> tags) noexcept {
        auto done = world->With<Component>(component, [&](Component& value) noexcept {
            for (const Name tag : tags) {
                if (!value.AddTag(tag)) {
                    return false;
                }
            }

            return true;
        });
        return done && done.Value();
    }

    static constexpr uint32 MaxTokens = 4;

    SystemAllocator allocator;
    TypeRegistry types;
    ModuleRegistration modules;
    NamePool names;
    NamePool foreign;
    UniquePtr<World> world;
    TypeRegistrationToken tokens[MaxTokens];
    uint32 tokenCount = 0;
    bool registered = false;
};

bool SameTags(Span<const Name> tags, std::initializer_list<Name> expected) noexcept {
    if (tags.Size() != expected.size()) {
        return false;
    }

    usize index = 0;
    for (const Name tag : expected) {
        if (!(tags[index++] == tag)) {
            return false;
        }
    }

    return true;
}

bool Contains(const Array<ObjectHandle>& handles, ObjectHandle handle) noexcept {
    for (const ObjectHandle value : handles) {
        if (value == handle) {
            return true;
        }
    }

    return false;
}

uint32 FieldFlagsOf(TypeRegistry& types, TypeId type, StringView name) noexcept {
    auto lease = types.Acquire(type);
    if (!lease) {
        return NumericLimits<uint32>::Max();
    }

    for (const FieldSchema& field : lease.Value().Schema().fields) {
        if (StringView(field.name.Data(), field.name.Size()) == name) {
            return field.flags;
        }
    }

    return NumericLimits<uint32>::Max();
}

} // namespace

// 31.4절: Actor 태그 더하기·빼기·통째 바꾸기와 HasTag. 거부는 값을 바꾸지 않는다.
TEST(WorldTags_ActorAddRemoveSet) {
    TagWorld fixture;
    REQUIRE(fixture.Ready());
    const Name enemy = fixture.Intern("Enemy");
    const Name boss = fixture.Intern("Boss");
    const Name missing = fixture.Intern("Missing");
    auto foreignTag = fixture.foreign.TryIntern("Enemy");
    REQUIRE(foreignTag);
    // 닫힌 pool의 옛 토큰이다(그 pool은 더 받지 않는다).
    Name staleTag;
    {
        NamePool closed(fixture.allocator);
        REQUIRE(closed.Initialize());
        auto made = closed.TryIntern("Enemy");
        REQUIRE(made);
        staleTag = made.Value();
        REQUIRE(closed.Shutdown());
    }

    const ObjectHandle actor = fixture.Spawn();
    REQUIRE(!actor.IsNull());
    auto checked = fixture.world->With<Actor>(actor, [&](Actor& value) noexcept {
        CHECK(value.Tags().IsEmpty() && !value.HasTag(enemy) && !value.HasTag(Name{}));
        CHECK(value.AddTag(enemy).IsOk());
        CHECK(value.AddTag(enemy).IsOk());
        CHECK(value.AddTag(boss).IsOk());
        CHECK(SameTags(value.Tags(), {enemy, boss}));
        CHECK(value.HasTag(enemy) && value.HasTag(boss) && !value.HasTag(missing));

        // 거부: None·다른 pool 토큰·옛 토큰(값 그대로).
        CHECK(value.AddTag(Name{}).Error() == ErrorCode::InvalidArgument);
        CHECK(value.AddTag(foreignTag.Value()).Error() == ErrorCode::InvalidArgument);
        CHECK(value.AddTag(staleTag).Error() == ErrorCode::InvalidArgument);
        CHECK(value.RemoveTag(Name{}).Error() == ErrorCode::InvalidArgument);
        const Name withForeign[] = {boss, foreignTag.Value()};
        CHECK(value.SetTags(Span<const Name>(withForeign)).Error() == ErrorCode::InvalidArgument);
        CHECK(SameTags(value.Tags(), {enemy, boss}));

        // 통째 바꾸기는 중복을 그대로 둔다. 빼기는 같은 태그를 모두 뺀다.
        const Name replaced[] = {boss, enemy, boss};
        CHECK(value.SetTags(Span<const Name>(replaced)).IsOk());
        CHECK(SameTags(value.Tags(), {boss, enemy, boss}));
        CHECK(value.RemoveTag(boss).IsOk());
        CHECK(SameTags(value.Tags(), {enemy}));
        CHECK(value.RemoveTag(missing).IsOk());
        CHECK(SameTags(value.Tags(), {enemy}));
        CHECK(value.SetTags(Span<const Name>()).IsOk());
        CHECK(value.Tags().IsEmpty());
        return true;
    });
    REQUIRE(checked);
}

// 31.4절: Component 태그는 같은 규칙이다. 파괴 대기 Component·Actor는 Destroyed, World 장부 밖
// 객체는 InvalidArgument, 이름 pool 없는 World는 Unsupported다(빼기는 pool이 없어도 된다).
TEST(WorldTags_ComponentAndRejections) {
    TagWorld fixture;
    REQUIRE(fixture.Ready());
    const Name enemy = fixture.Intern("Enemy");
    const ObjectHandle actor = fixture.Spawn();
    const ObjectHandle part = fixture.Add<TagPart>(actor);
    const ObjectHandle doomed = fixture.Add<TagPart>(actor);
    REQUIRE(!part.IsNull() && !doomed.IsNull());
    auto tagged = fixture.world->With<Component>(part, [&](Component& value) noexcept {
        CHECK(value.AddTag(enemy).IsOk());
        CHECK(value.HasTag(enemy) && SameTags(value.ComponentTags(), {enemy}));
        CHECK(value.RemoveTag(enemy).IsOk());
        CHECK(value.ComponentTags().IsEmpty());
        return true;
    });
    REQUIRE(tagged);

    REQUIRE(fixture.world->DestroyComponent(doomed));
    auto pending = fixture.world->With<Component>(doomed, [&](Component& value) noexcept {
        return value.AddTag(enemy).Error() == ErrorCode::Destroyed &&
               value.RemoveTag(enemy).Error() == ErrorCode::Destroyed &&
               value.SetTags(Span<const Name>()).Error() == ErrorCode::Destroyed;
    });
    REQUIRE(pending);
    CHECK(pending.Value());

    const ObjectHandle doomedActor = fixture.Spawn();
    REQUIRE(fixture.world->DestroyActor(doomedActor));
    auto pendingActor = fixture.world->With<Actor>(doomedActor, [&](Actor& value) noexcept {
        return value.AddTag(enemy).Error() == ErrorCode::Destroyed &&
               value.RemoveTag(enemy).Error() == ErrorCode::Destroyed &&
               value.SetTags(Span<const Name>()).Error() == ErrorCode::Destroyed;
    });
    REQUIRE(pendingActor);
    CHECK(pendingActor.Value());

    // World 장부 밖 객체(Registry에서 직접 만든 Actor).
    ObjectRegistry& objects = fixture.world->Objects();
    auto loose = objects.NewObject<TagActor>(fixture.world->Access(), ObjectTypeIdOf<TagActor>());
    REQUIRE(loose);
    auto outside = fixture.world->With<Actor>(loose.Value().Handle(), [&](Actor& value) noexcept {
        return value.AddTag(enemy).Error() == ErrorCode::InvalidArgument;
    });
    REQUIRE(outside);
    CHECK(outside.Value());

    // 이름 pool 없는 World.
    TagWorld bare(false, 2);
    REQUIRE(bare.Ready());
    const Name bareTag = bare.Intern("Enemy");
    const ObjectHandle bareActor = bare.Spawn();
    auto unsupported = bare.world->With<Actor>(bareActor, [&](Actor& value) noexcept {
        return value.AddTag(bareTag).Error() == ErrorCode::Unsupported &&
               value.RemoveTag(bareTag).IsOk();
    });
    REQUIRE(unsupported);
    CHECK(unsupported.Value());
}

// 31.5절: FindActorsWithTag는 파괴 대기를 빼고, None은 빈 결과다. FindComponentsByTag는 타입(파생
// 포함)· null 타입으로 거르고 components 순서를 지키며 파괴 대기를 뺀다. 이 World의 Actor가 아니면
// StaleHandle이다.
TEST(WorldTags_FindActorsAndComponents) {
    TagWorld fixture;
    REQUIRE(fixture.Ready());
    const Name enemy = fixture.Intern("Enemy");
    const Name boss = fixture.Intern("Boss");
    const ObjectHandle first = fixture.Spawn();
    const ObjectHandle second = fixture.Spawn();
    const ObjectHandle plain = fixture.Spawn();
    const ObjectHandle doomed = fixture.Spawn();
    // 다른 Level의 Actor와 지연 Spawn 중(Constructed) Actor도 찾는다.
    auto sideLevel = fixture.world->CreateLevel("Side");
    REQUIRE(sideLevel);
    SpawnParams inSide;
    inSide.level = sideLevel.Value();
    auto side = fixture.world->SpawnActor<TagActor>(inSide);
    auto deferred = fixture.world->SpawnActorDeferred<TagActor>();
    REQUIRE(side && deferred);
    REQUIRE(fixture.TagActorWith(first, {enemy}));
    REQUIRE(fixture.TagActorWith(second, {boss, enemy}));
    REQUIRE(fixture.TagActorWith(doomed, {enemy}));
    REQUIRE(fixture.TagActorWith(side.Value(), {enemy}));
    REQUIRE(fixture.TagActorWith(deferred.Value(), {enemy}));
    REQUIRE(fixture.world->DestroyActor(doomed));

    Array<ObjectHandle> found(fixture.allocator);
    REQUIRE(fixture.world->FindActorsWithTag(enemy, found));
    CHECK(found.Size() == 4 && Contains(found, first) && Contains(found, second) &&
          Contains(found, side.Value()) && Contains(found, deferred.Value()));
    CHECK(!Contains(found, plain) && !Contains(found, doomed));
    REQUIRE(fixture.world->FindActorsWithTag(boss, found));
    CHECK(found.Size() == 1 && found[0] == second);
    REQUIRE(fixture.world->FindActorsWithTag(Name{}, found));
    CHECK(found.IsEmpty());

    const ObjectHandle part = fixture.Add<TagPart>(first);
    const ObjectHandle derived = fixture.Add<TagPartDerived>(first);
    const ObjectHandle other = fixture.Add<OtherPart>(first);
    const ObjectHandle bossPart = fixture.Add<TagPart>(first);
    const ObjectHandle doomedPart = fixture.Add<TagPart>(first);
    REQUIRE(!part.IsNull() && !derived.IsNull() && !other.IsNull() && !bossPart.IsNull() &&
            !doomedPart.IsNull());
    REQUIRE(fixture.TagComponentWith(part, {enemy}));
    REQUIRE(fixture.TagComponentWith(derived, {enemy}));
    REQUIRE(fixture.TagComponentWith(other, {enemy}));
    REQUIRE(fixture.TagComponentWith(bossPart, {boss}));
    REQUIRE(fixture.TagComponentWith(doomedPart, {enemy}));
    REQUIRE(fixture.world->DestroyComponent(doomedPart));

    const TypeId partType = ObjectTypeIdOf<TagPart>();
    REQUIRE(fixture.world->FindComponentsByTag(first, partType, enemy, found));
    CHECK(found.Size() == 2 && found[0] == part && found[1] == derived);
    REQUIRE(fixture.world->FindComponentsByTag(first, TypeId{}, enemy, found));
    CHECK(found.Size() == 3 && found[0] == part && found[1] == derived && found[2] == other);
    REQUIRE(
        fixture.world->FindComponentsByTag(first, ObjectTypeIdOf<TagPartDerived>(), enemy, found));
    CHECK(found.Size() == 1 && found[0] == derived);
    REQUIRE(fixture.world->FindComponentsByTag(first, partType, Name{}, found));
    CHECK(found.IsEmpty());
    CHECK(fixture.world->FindComponentByTag(first, partType, enemy) == part);
    CHECK(fixture.world->FindComponentByTag(first, TypeId{}, boss) == bossPart);
    CHECK(fixture.world->FindComponentByTag(first, ObjectTypeIdOf<OtherPart>(), enemy) == other);
    CHECK(fixture.world->FindComponentByTag(first, ObjectTypeIdOf<OtherPart>(), boss).IsNull());
    CHECK(fixture.world->FindComponentByTag(first, partType, Name{}).IsNull());

    // 이 World의 Actor가 아니면 StaleHandle(찾기 하나는 null)이다.
    CHECK(fixture.world->FindComponentsByTag(part, partType, enemy, found).Error() ==
          ErrorCode::StaleHandle);
    CHECK(found.IsEmpty());
    CHECK(fixture.world->FindComponentByTag(part, partType, enemy).IsNull());
}

// 31.4·31.7절: 태그는 ReadOnly 영속 필드이고 저장 왕복에서 다시 읽은 Registry pool의 Name이 된다.
TEST(WorldTags_ReflectionAndSaveRoundTrip) {
    TagWorld fixture;
    REQUIRE(fixture.Ready());
    const uint32 readOnly = static_cast<uint32>(FieldFlags::ReadOnly);
    const uint32 actorTags = FieldFlagsOf(fixture.types, ObjectTypeIdOf<Actor>(), "tags_");
    const uint32 componentTags =
        FieldFlagsOf(fixture.types, ObjectTypeIdOf<Component>(), "componentTags_");
    REQUIRE(actorTags != NumericLimits<uint32>::Max());
    REQUIRE(componentTags != NumericLimits<uint32>::Max());
    CHECK((actorTags & readOnly) != 0 && (componentTags & readOnly) != 0);

    World& world = *fixture.world;
    const Name enemy = fixture.Intern("Enemy");
    const Name boss = fixture.Intern("Boss");
    auto actor = world.SpawnActor<SaveActor>();
    REQUIRE(actor);
    auto component = world.AddComponent<SaveComponent>(actor.Value());
    REQUIRE(component);
    REQUIRE(fixture.TagActorWith(actor.Value(), {enemy, boss}));
    REQUIRE(fixture.TagComponentWith(component.Value(), {boss}));

    Array<uint8> saved(fixture.allocator);
    {
        auto bytes = SaveObjectGraph(fixture.allocator, world.Objects(), world.Access(),
                                     world.PersistentLevel());
        REQUIRE(bytes);
        saved = Move(bytes.Value());
    }

    NamePool loadedNames(fixture.allocator);
    REQUIRE(loadedNames.Initialize());
    {
        RegistryOptions options;
        options.names = &loadedNames;
        ObjectRegistry loaded(fixture.allocator, fixture.types, {LoadedContextHigh, 1}, options);
        const auto access = loaded.OwnerAccess();
        auto root = LoadObjectGraph(fixture.allocator, loaded, fixture.types, access, saved.View());
        REQUIRE(root);
        auto loadedEnemy = loadedNames.TryFind("Enemy");
        auto loadedBoss = loadedNames.TryFind("Boss");
        REQUIRE(loadedEnemy && loadedBoss);
        bool checkedActor = false;
        bool checkedComponent = false;
        {
            auto level = loaded.Resolve<Level>(access, root.Value().Handle());
            REQUIRE(level);
            auto levelPointer = level.Value().Get();
            REQUIRE(levelPointer);
            for (const ObjectHandle handle : levelPointer.Value()->Actors()) {
                auto saveActor = loaded.Resolve<SaveActor>(access, handle);
                if (!saveActor) {
                    continue;
                }

                auto pointer = saveActor.Value().Get();
                REQUIRE(pointer);
                checkedActor =
                    SameTags(pointer.Value()->Tags(), {loadedEnemy.Value(), loadedBoss.Value()});
                for (const ObjectHandle part : pointer.Value()->Components()) {
                    auto saveComponent = loaded.Resolve<SaveComponent>(access, part);
                    if (saveComponent) {
                        auto partPointer = saveComponent.Value().Get();
                        REQUIRE(partPointer);
                        checkedComponent =
                            SameTags(partPointer.Value()->ComponentTags(), {loadedBoss.Value()});
                    }
                }
            }
        }

        CHECK(checkedActor && checkedComponent);
        root.Value() = {};
        CHECK(loaded.Collect(access));
        CHECK(loaded.Shutdown(access));
    }

    CHECK(loadedNames.Shutdown());
}

} // namespace world_tag_tests_file
