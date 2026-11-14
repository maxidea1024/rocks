#include <ReflectionTestFixtures/ReflectionTestFixtures.module.generated.h>
#include <Core/Memory/SystemAllocator.h>
#include <Object/NativeClass.h>
#include <Object/PropertyPath.h>
#include <Platform/Fatal.h>
#include <TestSupport/Test.h>

// 리플렉션 base를 둔 native 파생 시험이다(Docs/Design/ReflectionFoundations.ko.md 9.1절 B8a). .cpp
// 안의 ROCKS_NATIVE_TYPE 클래스가 생성 fixture(WorldOnly WorldFixture, 그 위의 두 단계 리플렉션
// 사슬 SceneFixture)를 base로 두고 RegisterNativeClass로 등록한다: WorldOnly 상속, base 필드·함수표
// 빌리기(읽기·쓰기·경로 접근·struct 의존과 그 미등록 NotFound), WorldOnly 아닌·Abstract 리플렉션
// base(WorldOnly만 물려받음), 손 Trace와 생성 Trace 모두 방문(GC 생존, 이중 방문 없음 —
// private·protected 손 Trace 포함), 두 단계 native 사슬, base 해제 Busy, WorldOnly를 빠뜨린 수동
// 등록 거부, ObjectTypeIdOf 두 경로. 그래프 저장·로드 Unsupported는 ObjectSerialization
// 시험(NativeDerivedGraphTests)이, 잘못된 Trace 선언의 컴파일 거부는 ToolTests meta 컴파일 probe가
// 본다.
using namespace rocks;
using reflection_fixture::AbstractBase;
using reflection_fixture::AbstractByPolicy;
using reflection_fixture::Base;
using reflection_fixture::Node;
using reflection_fixture::SceneFixture;
using reflection_fixture::WorldFixture;

namespace {

// WorldOnly 리플렉션 base를 둔 native 파생이다. 손 Trace는 자기 간선만 방문한다(base 간선은 생성
// Trace).
class NativeWorldActor : public WorldFixture {
    ROCKS_NATIVE_TYPE(NativeWorldActor, WorldFixture, 0x9e9bd6ca45c341e0ull, 0x9a8af213ef8632a4ull)

public:
    void Trace(TraceVisitor& visitor) const noexcept {
        visitor.Visit(extra);
    }

    ObjectRef<Node> extra;
    int32 nativeState = 0;
};

// 두 단계 native 사슬(native U : native T : 리플렉션 R)이다. Super::Trace를 부르지 않는다.
class NativeWorldLeaf final : public NativeWorldActor {
    ROCKS_NATIVE_TYPE(NativeWorldLeaf, NativeWorldActor, 0xe1020696aceb498cull,
                      0x84834a8ceb18a844ull)

public:
    void Trace(TraceVisitor& visitor) const noexcept {
        visitor.Visit(leaf);
    }

    ObjectRef<Node> leaf;
};

// 리플렉션 두 단계 상속(SceneFixture : WorldFixture — SceneComponent형) 위의 native 파생이다. 손
// Trace가 없다.
class NativeSceneActor final : public SceneFixture {
    ROCKS_NATIVE_TYPE(NativeSceneActor, SceneFixture, 0xa3c84c8b47a046d7ull, 0x8eff1b6ac3b3c8dbull)

public:
    int32 frame = 0;
};

// 리플렉션 조상이 없는 native 클래스다(ReflectedAncestor가 void).
class NativePlain final : public Object {
    ROCKS_NATIVE_TYPE(NativePlain, Object, 0xfaae79f3597941e7ull, 0x89236be0eb959c6dull)
};

// WorldOnly가 아닌 리플렉션 base(Base — key·count) 위의 native 파생이다.
class NativeOnBase final : public Base {
    ROCKS_NATIVE_TYPE(NativeOnBase, Base, 0xb59a29e87f1f4c5cull, 0xaf4bda4b88c8143bull)
};

// 선언 정책 Abstract인 리플렉션 base 위의 구체 native 파생이다(Abstract 비트는 물려받지 않는다).
class NativeOnPolicyAbstract final : public AbstractByPolicy {
    ROCKS_NATIVE_TYPE(NativeOnPolicyAbstract, AbstractByPolicy, 0xe4b433cdf1bd4928ull,
                      0x8d9f98c9d944c3f8ull)
};

// C++ 추상 리플렉션 base(private 필드 baseValue) 위의 구체 native 파생이다.
class NativeOnAbstract final : public AbstractBase {
    ROCKS_NATIVE_TYPE(NativeOnAbstract, AbstractBase, 0x8ad3f59065644ba8ull, 0xaa9f3f9c24fd7da2ull)

public:
    int32 ClassValue() const noexcept override {
        return 5;
    }
};

// private Trace를 둔 리플렉션 base 위 native 파생이다(접근자가 friend라 등록이 찾아 부른다).
class NativePrivateTrace final : public WorldFixture {
    ROCKS_NATIVE_TYPE(NativePrivateTrace, WorldFixture, 0x3d1f1a46e9e5404dull,
                      0x80d55c79de10abb7ull)

public:
    ObjectRef<Node> hidden;

private:
    void Trace(TraceVisitor& visitor) const noexcept {
        visitor.Visit(hidden);
    }
};

// protected Trace를 둔 native 기반이다.
class NativeProtectedTrace : public Object {
    ROCKS_NATIVE_TYPE(NativeProtectedTrace, Object, 0x1a3efdaae8cf47f8ull, 0xb55c0b7770ce0947ull)

public:
    ObjectRef<Node> guarded;

protected:
    void Trace(TraceVisitor& visitor) const noexcept {
        visitor.Visit(guarded);
    }
};

// protected Trace 기반의 파생이다: 자기 Trace를 private으로 선언한다(기반 Trace는 사슬이 부른다).
class NativeProtectedLeaf final : public NativeProtectedTrace {
    ROCKS_NATIVE_TYPE(NativeProtectedLeaf, NativeProtectedTrace, 0x3ce984d542d34826ull,
                      0xa41017196e451d3eull)

public:
    ObjectRef<Node> own;

private:
    void Trace(TraceVisitor& visitor) const noexcept {
        visitor.Visit(own);
    }
};

// protected Trace 기반의 파생이다: Trace를 선언하지 않는다(물려받은 이름은 기반 단계가 부른다).
class NativeProtectedPlain final : public NativeProtectedTrace {
    ROCKS_NATIVE_TYPE(NativeProtectedPlain, NativeProtectedTrace, 0x5c1656f7d1554964ull,
                      0xb8666ef6f2dbd0fcull)
};

// WorldOnly를 빠뜨린 수동 등록에 쓰는 ID다.
constexpr TypeId ManualWorldActorId{0x8176cd44043c4327ull, 0x844cb27d44fb0022ull};

// 손 Trace의 판정은 접근 지정과 무관하고, 물려받은 Trace는 그 기반 단계의 몫이다.
using TraceDeclaration = detail::NativeClassAccess::TraceDeclaration;
static_assert(detail::NativeClassAccess::TraceOf<NativeWorldActor>() == TraceDeclaration::Own);
static_assert(detail::NativeClassAccess::TraceOf<NativePrivateTrace>() == TraceDeclaration::Own);
static_assert(detail::NativeClassAccess::TraceOf<NativeProtectedTrace>() == TraceDeclaration::Own);
static_assert(detail::NativeClassAccess::TraceOf<NativeProtectedLeaf>() == TraceDeclaration::Own);
static_assert(detail::NativeClassAccess::TraceOf<NativeProtectedPlain>() == TraceDeclaration::None);
static_assert(detail::NativeClassAccess::TraceOf<NativeSceneActor>() == TraceDeclaration::None);

// 두 경로(native는 T::StaticTypeId, 리플렉션은 생성 ID)와 리플렉션 조상 판정은 컴파일 시 값이다.
static_assert(ObjectTypeIdOf<NativeWorldActor>() == NativeWorldActor::StaticTypeId);
static_assert(ObjectTypeIdOf<NativeWorldLeaf>() == NativeTypeIdOf<NativeWorldLeaf>());
static_assert(ObjectTypeIdOf<WorldFixture>() == TypeIdOf<WorldFixture>());
static_assert(ObjectTypeIdOf<SceneFixture>() == GeneratedType<SceneFixture>::Id());
static_assert(__is_same(detail::ReflectedAncestor<NativeWorldActor>, WorldFixture));
static_assert(__is_same(detail::ReflectedAncestor<NativeWorldLeaf>, WorldFixture));
static_assert(__is_same(detail::ReflectedAncestor<NativeSceneActor>, SceneFixture));
static_assert(__is_same(detail::ReflectedAncestor<NativePlain>, void));
static_assert(NativeClassType<NativeWorldActor> && !NativeClassType<WorldFixture>);

template <class T>
T* Get(Borrow<T>& borrow) noexcept {
    auto pointer = borrow.Get();
    return pointer ? pointer.Value() : nullptr;
}

const FieldSchema& SchemaField(const TypeSchema& schema, StringView name) noexcept {
    for (const FieldSchema& field : schema.fields) {
        if (StringView(field.name.Data(), field.name.Size()) == name) {
            return field;
        }
    }

    platform::Fatal("fixture field missing");
}

FieldValue Signed(Allocator& allocator, int64 value) noexcept {
    FieldValue result(allocator);
    result.kind = ValueKind::Int32;
    result.signedInteger = value;
    return result;
}

FieldValue Float64(Allocator& allocator, double value) noexcept {
    FieldValue result(allocator);
    result.kind = ValueKind::Float64;
    result.float64 = value;
    return result;
}

FieldValue Reference(Allocator& allocator, ObjectHandle handle) noexcept {
    FieldValue result(allocator);
    result.kind = ValueKind::ObjectReference;
    result.reference = handle;
    return result;
}

// 모듈 타입(fixture·CoreTypeBindings)과 이 TU의 native 파생 셋을 등록한다. native 토큰은 해제
// 순서(파생 먼저)를 위해 호출자가 쥔다.
struct NativeTokens {
    TypeRegistrationToken actor{};
    TypeRegistrationToken leaf{};
    TypeRegistrationToken scene{};
};

bool RegisterAll(ModuleRegistration& registration, TypeRegistry& types,
                 NativeTokens& tokens) noexcept {
    if (!generated::ReflectionTestFixtures::RegisterModuleTypes(registration)) {
        return false;
    }

    auto actor = RegisterNativeClass<NativeWorldActor>(types);
    auto leaf = actor ? RegisterNativeClass<NativeWorldLeaf>(types)
                      : Result<TypeRegistrationToken>::Failure(actor.Error());
    auto scene = RegisterNativeClass<NativeSceneActor>(types);
    if (!actor || !leaf || !scene) {
        return false;
    }

    tokens.actor = actor.Value();
    tokens.leaf = leaf.Value();
    tokens.scene = scene.Value();
    return true;
}

// native 토큰을 파생부터 해제한 뒤 모듈 기록을 해제한다.
bool UnregisterAll(ModuleRegistration& registration, TypeRegistry& types,
                   const NativeTokens& tokens) noexcept {
    return types.Unregister(tokens.leaf).IsOk() && types.Unregister(tokens.actor).IsOk() &&
           types.Unregister(tokens.scene).IsOk() && registration.Unregister().IsOk();
}

// Trace가 방문한 handle과 횟수를 센다.
struct VisitLog {
    ObjectHandle handles[8];
    uint32 counts[8] = {};
    usize size = 0;
    uint32 total = 0;

    static void Record(void* state, ObjectHandle handle) noexcept {
        auto& log = *static_cast<VisitLog*>(state);
        ++log.total;
        for (usize index = 0; index < log.size; ++index) {
            if (log.handles[index] == handle) {
                ++log.counts[index];
                return;
            }
        }

        if (log.size == 8) {
            platform::Fatal("too many traced handles");
        }

        log.handles[log.size] = handle;
        log.counts[log.size++] = 1;
    }

    uint32 CountOf(ObjectHandle handle) const noexcept {
        for (usize index = 0; index < size; ++index) {
            if (handles[index] == handle) {
                return counts[index];
            }
        }

        return 0;
    }
};

} // namespace

// 등록: 스키마는 리플렉션 조상의 필드를 같은 ID로 담고(함수표도 빌린다) WorldOnly를 물려받으며
// 그래프 저장·병렬 Trace 선언은 끈다. 두 단계 리플렉션 사슬 위의 native는 struct 필드의 의존(등록
// struct)도 같은 레코드로 푼다. native 파생은 base(리플렉션 모듈 타입·native 기반)가 먼저
// 등록되어야 한다.
TEST(NativeReflectedBase_RegistersBorrowedFieldsAndInheritsWorldOnly) {
    SystemAllocator allocator;
    TypeRegistry types(allocator);
    {
        // 리플렉션 base가 아직 없으면 NotFound, native 기반보다 먼저 파생을 등록해도 NotFound다.
        CHECK(RegisterNativeClass<NativeWorldActor>(types).Error() == ErrorCode::NotFound);
        ModuleRegistration registration(types, allocator);
        REQUIRE(generated::ReflectionTestFixtures::RegisterModuleTypes(registration));
        CHECK(RegisterNativeClass<NativeWorldLeaf>(types).Error() == ErrorCode::NotFound);
        auto actorToken = RegisterNativeClass<NativeWorldActor>(types);
        auto leafToken = RegisterNativeClass<NativeWorldLeaf>(types);
        auto sceneToken = RegisterNativeClass<NativeSceneActor>(types);
        auto plainToken = RegisterNativeClass<NativePlain>(types);
        REQUIRE(actorToken && leafToken && sceneToken && plainToken);
        CHECK(RegisterNativeClass<NativeWorldActor>(types).Error() == ErrorCode::AlreadyExists);
        {
            auto worldLease = types.Acquire(TypeIdOf<WorldFixture>());
            auto sceneBase = types.Acquire(TypeIdOf<SceneFixture>());
            auto actor = types.Acquire(ObjectTypeIdOf<NativeWorldActor>());
            auto leaf = types.Acquire(ObjectTypeIdOf<NativeWorldLeaf>());
            auto scene = types.Acquire(ObjectTypeIdOf<NativeSceneActor>());
            auto plain = types.Acquire(ObjectTypeIdOf<NativePlain>());
            REQUIRE(worldLease && sceneBase && actor && leaf && scene && plain);
            const auto& world = worldLease.Value();

            // 리플렉션 조상의 필드를 같은 순서·ID로 담고 함수표도 같은 필드마다 있다.
            const auto& actorSchema = actor.Value().Schema();
            REQUIRE(actorSchema.fields.Size() == world.Schema().fields.Size());
            CHECK(actorSchema.fields.Size() == 2);
            for (usize index = 0; index < actorSchema.fields.Size(); ++index) {
                CHECK(actorSchema.fields[index].id == world.Schema().fields[index].id);
                CHECK(actor.Value().FieldOps(index) != nullptr);
            }

            CHECK(actor.Value().Ops().fields.Size() == world.Ops().fields.Size());
            CHECK(actorSchema.baseType == TypeIdOf<WorldFixture>());
            CHECK(actor.Value().Name() == StringView("NativeWorldActor"));
            CHECK(actor.Value().IsA(TypeIdOf<WorldFixture>()));
            CHECK(actor.Value().IsNativeA(NativeTypeKey<WorldFixture>()));
            CHECK(!actor.Value().IsNativeA(NativeTypeKey<SceneFixture>()));

            // WorldOnly는 물려받고(리플렉션 base가 WorldOnly) 그래프 저장·병렬 Trace는 선언하지
            // 않는다.
            CHECK((world.Schema().flags & static_cast<uint32>(TypeFlags::WorldOnly)) != 0);
            CHECK(actorSchema.flags == static_cast<uint32>(TypeFlags::WorldOnly));
            CHECK(!actor.Value().Ops().completeFieldLayout);
            CHECK(!actor.Value().Ops().parallelTraceSafe);
            CHECK(world.Ops().completeFieldLayout && world.Ops().parallelTraceSafe);

            // 두 단계 native 사슬: base는 native 기반이고 필드·WorldOnly는 사슬 위 리플렉션
            // 조상에서 온다.
            const auto& leafSchema = leaf.Value().Schema();
            CHECK(leafSchema.baseType == NativeWorldActor::StaticTypeId);
            CHECK(leafSchema.flags == static_cast<uint32>(TypeFlags::WorldOnly));
            CHECK(leafSchema.fields.Size() == world.Schema().fields.Size());
            CHECK(leaf.Value().IsA(TypeIdOf<WorldFixture>()));
            CHECK(leaf.Value().IsNativeA(NativeTypeKey<NativeWorldActor>()));
            CHECK(!leaf.Value().Ops().completeFieldLayout);

            // 리플렉션 두 단계 상속 위: WorldOnly는 R1(WorldFixture)에서 R2(SceneFixture)를 거쳐
            // 오고, R2의 Transform 필드는 같은 등록 struct를 의존으로 붙잡는다(struct 의존 필드
            // 복사).
            const auto& sceneSchema = scene.Value().Schema();
            CHECK(sceneSchema.baseType == TypeIdOf<SceneFixture>());
            CHECK(sceneSchema.flags == static_cast<uint32>(TypeFlags::WorldOnly));
            REQUIRE(sceneSchema.fields.Size() == sceneBase.Value().Schema().fields.Size());
            CHECK(sceneSchema.fields.Size() == 3);
            const Guid relativeId = SchemaField(TypeOf<SceneFixture>(), "relative").id;
            const usize relative = scene.Value().FieldIndex(relativeId);
            REQUIRE(relative != TypeLease::NoField);
            const StructView transform = scene.Value().FieldStruct(relative);
            REQUIRE(!transform.IsEmpty());
            CHECK(transform.Schema().id == TypeIdOf<Transform>());
            CHECK(transform.Schema().id ==
                  sceneBase.Value()
                      .FieldStruct(sceneBase.Value().FieldIndex(relativeId))
                      .Schema()
                      .id);
            CHECK(scene.Value().FieldOps(relative) != nullptr);
            CHECK(scene.Value().FieldOps(relative)->path != nullptr);

            // 리플렉션 조상이 없는 native는 필드 없는 스키마다(기존 동작).
            CHECK(plain.Value().Schema().fields.Size() == 0);
            CHECK(plain.Value().Schema().flags == 0);
            CHECK(plain.Value().Schema().baseType == TypeId{});
        }

        // 빌린 thunk의 코드 수명은 base 레코드가 지킨다: native 파생이 남아 있으면 base 해제는
        // Busy이고 남은 기록은 보존되어 다시 호출할 수 있다.
        auto busy = registration.Unregister();
        CHECK(!busy && busy.Error() == ErrorCode::Busy);
        CHECK(static_cast<bool>(types.Acquire(TypeIdOf<WorldFixture>())));
        CHECK(types.Unregister(actorToken.Value()).Error() == ErrorCode::Busy);
        REQUIRE(types.Unregister(leafToken.Value()));
        REQUIRE(types.Unregister(actorToken.Value()));
        REQUIRE(types.Unregister(sceneToken.Value()));
        REQUIRE(types.Unregister(plainToken.Value()));
        REQUIRE(registration.Unregister());
    }

    CHECK(types.Shutdown());
}

// base 필드 읽기·쓰기·경로 접근: 빌린 생성 thunk(Resolve<R>·IsNativeA 판정)가 native 파생 객체의
// base 필드를 정적 타입으로 다룬다. 파생 lease·base lease 어느 쪽의 함수표·경로로도 같은 필드를
// 가리킨다.
TEST(NativeReflectedBase_BaseFieldsReadWriteAndPathAccess) {
    SystemAllocator allocator;
    TypeRegistry types(allocator);
    {
        ModuleRegistration registration(types, allocator);
        NativeTokens tokens;
        REQUIRE(RegisterAll(registration, types, tokens));
        {
            ObjectRegistry objects(allocator, types, {0x4e52, 1});
            const auto access = objects.OwnerAccess();
            {
                auto actorLease = types.Acquire(ObjectTypeIdOf<NativeWorldActor>());
                auto sceneLease = types.Acquire(ObjectTypeIdOf<NativeSceneActor>());
                auto worldLease = types.Acquire(TypeIdOf<WorldFixture>());
                auto actor =
                    objects.NewObject<NativeWorldActor>(access, ObjectTypeIdOf<NativeWorldActor>());
                auto scene =
                    objects.NewObject<NativeSceneActor>(access, ObjectTypeIdOf<NativeSceneActor>());
                auto node = objects.NewObject<Node>(access, TypeIdOf<Node>());
                REQUIRE(actorLease && sceneLease && worldLease && actor && scene && node);
                const ObjectHandle self = actor.Value().Handle();

                // 파생 lease의 함수표로 base 필드를 쓰고 읽는다.
                const Guid noteId = SchemaField(TypeOf<WorldFixture>(), "note").id;
                const Guid targetId = SchemaField(TypeOf<WorldFixture>(), "target").id;
                const RuntimeFieldOps* note =
                    actorLease.Value().FieldOps(actorLease.Value().FieldIndex(noteId));
                const RuntimeFieldOps* target =
                    actorLease.Value().FieldOps(actorLease.Value().FieldIndex(targetId));
                REQUIRE(note != nullptr && target != nullptr);
                REQUIRE(note->write(objects, access, self, Signed(allocator, 41)));
                REQUIRE(target->write(objects, access, self,
                                      Reference(allocator, node.Value().Handle())));
                FieldValue read(allocator);
                REQUIRE(note->read(objects, access, self, read));
                CHECK(read.kind == ValueKind::Int32 && read.signedInteger == 41);
                REQUIRE(target->read(objects, access, self, read));
                CHECK(read.kind == ValueKind::ObjectReference &&
                      read.reference == node.Value().Handle());
                {
                    auto borrowed = objects.Resolve<NativeWorldActor>(access, self);
                    REQUIRE(borrowed);
                    NativeWorldActor* pointer = Get(borrowed.Value());
                    REQUIRE(pointer != nullptr);
                    CHECK(pointer->note == 41);
                    CHECK(pointer->target.Handle() == node.Value().Handle());
                }

                // base lease의 함수표도 파생 객체에 쓴다(상속 필드는 같은 ID).
                const RuntimeFieldOps* baseNote =
                    worldLease.Value().FieldOps(worldLease.Value().FieldIndex(noteId));
                REQUIRE(baseNote != nullptr);
                REQUIRE(baseNote->write(objects, access, self, Signed(allocator, 42)));
                REQUIRE(note->read(objects, access, self, read));
                CHECK(read.signedInteger == 42);

                // 경로 접근(B6): 도구 이름(Name="Label")으로 해석하고 읽고 쓴다.
                PropertyPathBuffer path;
                REQUIRE(ParsePropertyPath(actorLease.Value(), StringView("label"), path));
                REQUIRE(path.count == 1 && path.steps[0].fieldId == noteId);
                FieldValue previous(allocator);
                REQUIRE(
                    PathWrite(objects, access, self, path.View(), Signed(allocator, 7), &previous));
                CHECK(previous.signedInteger == 42);
                REQUIRE(PathRead(objects, access, self, path.View(), read));
                CHECK(read.signedInteger == 7);

                // 두 단계 리플렉션 사슬의 struct 필드(다른 모듈 Binds 미러)를 경로로 쓴다.
                const ObjectHandle placed = scene.Value().Handle();
                REQUIRE(ParsePropertyPath(sceneLease.Value(), StringView("relative.translation.x"),
                                          path));
                CHECK(path.count == 3);
                REQUIRE(PathWrite(objects, access, placed, path.View(), Float64(allocator, 2.5)));
                REQUIRE(PathRead(objects, access, placed, path.View(), read));
                CHECK(read.kind == ValueKind::Float64 && read.float64 == 2.5);
                String text(allocator);
                REQUIRE(FormatPropertyPath(sceneLease.Value(), path.View(), text));
                CHECK(text.View() == StringView("relative.translation.x"));
                REQUIRE(ParsePropertyPath(sceneLease.Value(), StringView("Label"), path));
                REQUIRE(PathWrite(objects, access, placed, path.View(), Signed(allocator, 9)));
                {
                    auto borrowed = objects.Resolve<NativeSceneActor>(access, placed);
                    REQUIRE(borrowed);
                    NativeSceneActor* pointer = Get(borrowed.Value());
                    REQUIRE(pointer != nullptr);
                    CHECK(pointer->relative.translation.x == 2.5);
                    CHECK(pointer->note == 9);
                }

                // struct 필드를 통째로 읽는 것은 리플렉션 클래스와 같이 Unsupported다.
                PropertyPathFailure failure;
                REQUIRE(ParsePropertyPath(sceneLease.Value(), StringView("relative"), path));
                CHECK(PathRead(objects, access, placed, path.View(), read, &failure).Error() ==
                      ErrorCode::Unsupported);
                CHECK(failure.reason == PropertyPathError::AggregateValue);
            }

            REQUIRE(objects.Shutdown(access));
        }

        REQUIRE(UnregisterAll(registration, types, tokens));
    }

    CHECK(types.Shutdown());
}

// Trace: native 사슬의 손 Trace(U·T)와 리플렉션 조상의 생성 Trace(R — 상속 필드 포함)를 모두
// 부르고, 각 간선을 한 번씩만 방문한다. 간선으로만 닿는 객체는 수집 뒤에도 산다.
TEST(NativeReflectedBase_TraceVisitsHandAndGeneratedEdges) {
    SystemAllocator allocator;
    {
        TypeRegistry types(allocator);
        ModuleRegistration registration(types, allocator);
        NativeTokens tokens;
        REQUIRE(RegisterAll(registration, types, tokens));
        {
            ObjectRegistry objects(allocator, types, {0x4e52, 2});
            const auto access = objects.OwnerAccess();
            auto leaf =
                objects.NewObject<NativeWorldActor>(access, ObjectTypeIdOf<NativeWorldLeaf>());
            auto scene =
                objects.NewObject<NativeSceneActor>(access, ObjectTypeIdOf<NativeSceneActor>());
            REQUIRE(leaf && scene);
            const ObjectHandle owner = leaf.Value().Handle();
            const ObjectHandle placed = scene.Value().Handle();
            ObjectHandle viaGenerated;
            ObjectHandle viaBase;
            ObjectHandle viaLeaf;
            ObjectHandle viaScene;
            {
                auto first = objects.NewObject<Node>(access, TypeIdOf<Node>());
                auto second = objects.NewObject<Node>(access, TypeIdOf<Node>());
                auto third = objects.NewObject<Node>(access, TypeIdOf<Node>());
                auto fourth = objects.NewObject<Node>(access, TypeIdOf<Node>());
                REQUIRE(first && second && third && fourth);
                viaGenerated = first.Value().Handle();
                viaBase = second.Value().Handle();
                viaLeaf = third.Value().Handle();
                viaScene = fourth.Value().Handle();
                auto borrowed = objects.Resolve<NativeWorldLeaf>(access, owner);
                REQUIRE(borrowed);
                NativeWorldLeaf* pointer = Get(borrowed.Value());
                REQUIRE(pointer != nullptr);
                REQUIRE(objects.SetReference(access, owner, pointer->target, viaGenerated));
                REQUIRE(objects.SetReference(access, owner, pointer->extra, viaBase));
                REQUIRE(objects.SetReference(access, owner, pointer->leaf, viaLeaf));

                // 등록된 trace를 직접 불러 세 간선이 한 번씩만 방문됨을 본다.
                auto lease = types.Acquire(ObjectTypeIdOf<NativeWorldLeaf>());
                REQUIRE(lease);
                VisitLog log;
                TraceVisitor visitor(&log, &VisitLog::Record);
                lease.Value().Ops().trace(*pointer, visitor);
                CHECK(log.total == 3);
                CHECK(log.CountOf(viaGenerated) == 1);
                CHECK(log.CountOf(viaBase) == 1);
                CHECK(log.CountOf(viaLeaf) == 1);

                auto sceneBorrowed = objects.Resolve<NativeSceneActor>(access, placed);
                REQUIRE(sceneBorrowed);
                NativeSceneActor* scenePointer = Get(sceneBorrowed.Value());
                REQUIRE(scenePointer != nullptr);
                REQUIRE(objects.SetReference(access, placed, scenePointer->target, viaScene));

                // 손 Trace가 없는 native(두 단계 리플렉션 사슬 위)는 생성 Trace만 — 상속 참조
                // 하나를 한 번(Transform 필드는 참조가 없다).
                auto sceneLease = types.Acquire(ObjectTypeIdOf<NativeSceneActor>());
                REQUIRE(sceneLease);
                VisitLog sceneLog;
                TraceVisitor sceneVisitor(&sceneLog, &VisitLog::Record);
                sceneLease.Value().Ops().trace(*scenePointer, sceneVisitor);
                CHECK(sceneLog.total == 1);
                CHECK(sceneLog.CountOf(viaScene) == 1);
            }

            // 루트를 놓은 Node 넷은 native 파생의 간선(생성 Trace 둘·손 Trace 둘)으로만 닿는다.
            REQUIRE(objects.Collect(access));
            CHECK(objects.Stats().liveObjects == 6);
            CHECK(objects.Resolve<Node>(access, viaGenerated).IsOk());
            CHECK(objects.Resolve<Node>(access, viaBase).IsOk());
            CHECK(objects.Resolve<Node>(access, viaLeaf).IsOk());
            CHECK(objects.Resolve<Node>(access, viaScene).IsOk());

            leaf.Value().Reset();
            scene.Value().Reset();
            REQUIRE(objects.Collect(access));
            CHECK(objects.Stats().liveObjects == 0);
            REQUIRE(objects.Shutdown(access));
        }

        REQUIRE(UnregisterAll(registration, types, tokens));
        CHECK(types.Shutdown());
    }

    CHECK(allocator.Stats().liveAllocations == 0);
}

// WorldOnly base의 파생을 수동 함수표로 등록할 때 WorldOnly를 빠뜨리면 InvalidArgument, 상속 필드를
// 담지 않으면 TypeMismatch다(RegisterNativeClass가 대신 채우는 두 조건).
TEST(NativeReflectedBase_ManualRegistrationMissingWorldOnlyIsRejected) {
    SystemAllocator allocator;
    TypeRegistry types(allocator);
    {
        ModuleRegistration registration(types, allocator);
        REQUIRE(generated::ReflectionTestFixtures::RegisterModuleTypes(registration));
        auto manual = [&](uint32 flags, bool withFields) noexcept {
            TypeSchema schema;
            schema.id = ManualWorldActorId;
            schema.name = Span<const char>("ManualWorldActor", 16);
            schema.baseType = TypeIdOf<WorldFixture>();
            schema.flags = flags;
            RuntimeTypeOps ops = MakeRuntimeTypeOps<NativeWorldActor>();
            ops.nativeBaseType = NativeTypeKey<WorldFixture>();
            if (withFields) {
                schema.fields = GeneratedType<WorldFixture>::Schema().fields;
                ops.fields = GeneratedType<WorldFixture>::Ops().fields;
            }

            return types.Register(schema, Move(ops));
        };
        const uint32 worldOnly = static_cast<uint32>(TypeFlags::WorldOnly);
        CHECK(manual(0, true).Error() == ErrorCode::InvalidArgument);
        CHECK(manual(worldOnly, false).Error() == ErrorCode::TypeMismatch);
        auto accepted = manual(worldOnly, true);
        REQUIRE(accepted);
        REQUIRE(types.Unregister(accepted.Value()));
        REQUIRE(registration.Unregister());
    }

    CHECK(types.Shutdown());
}

// ObjectTypeIdOf: native는 ROCKS_NATIVE_TYPE의 StaticTypeId, 리플렉션 타입은 생성 ID다(위
// static_assert가 컴파일 시 값을, 여기서는 등록된 레코드와의 대응을 본다).
TEST(NativeReflectedBase_ObjectTypeIdOfCoversNativeAndReflectedTypes) {
    SystemAllocator allocator;
    TypeRegistry types(allocator);
    {
        ModuleRegistration registration(types, allocator);
        NativeTokens tokens;
        REQUIRE(RegisterAll(registration, types, tokens));
        CHECK(ObjectTypeIdOf<NativeWorldActor>() == NativeTypeIdOf<NativeWorldActor>());
        CHECK(ObjectTypeIdOf<SceneFixture>() == TypeIdOf<SceneFixture>());
        {
            auto native = types.Acquire(ObjectTypeIdOf<NativeSceneActor>());
            auto reflected = types.Acquire(ObjectTypeIdOf<SceneFixture>());
            REQUIRE(native && reflected);
            CHECK(native.Value().Ops().nativeType == NativeTypeKey<NativeSceneActor>());
            CHECK(reflected.Value().Ops().nativeType == NativeTypeKey<SceneFixture>());
            CHECK(native.Value().Schema().baseType == reflected.Value().Id());
        }

        REQUIRE(UnregisterAll(registration, types, tokens));
    }

    CHECK(types.Shutdown());
}

// WorldOnly가 아닌 리플렉션 base 위 native는 flags 0이고, Abstract(선언 정책·C++ 추상) 리플렉션
// base 위의 구체 native는 Abstract 비트를 물려받지 않아 생성할 수 있다. 추상 base의 private 필드도
// 빌린 생성 thunk(GENERATED_BODY friend)로 읽는다.
TEST(NativeReflectedBase_InheritsOnlyWorldOnlyFromReflectedBase) {
    SystemAllocator allocator;
    TypeRegistry types(allocator);
    {
        ModuleRegistration registration(types, allocator);
        REQUIRE(generated::ReflectionTestFixtures::RegisterModuleTypes(registration));
        auto onBase = RegisterNativeClass<NativeOnBase>(types);
        auto onPolicy = RegisterNativeClass<NativeOnPolicyAbstract>(types);
        auto onAbstract = RegisterNativeClass<NativeOnAbstract>(types);
        REQUIRE(onBase && onPolicy && onAbstract);
        const uint32 abstract = static_cast<uint32>(TypeFlags::Abstract);
        CHECK(TypeOf<Base>().flags == 0);
        CHECK((TypeOf<AbstractByPolicy>().flags & abstract) != 0);
        CHECK((TypeOf<AbstractBase>().flags & abstract) != 0);
        {
            auto base = types.Acquire(ObjectTypeIdOf<NativeOnBase>());
            auto policy = types.Acquire(ObjectTypeIdOf<NativeOnPolicyAbstract>());
            auto concrete = types.Acquire(ObjectTypeIdOf<NativeOnAbstract>());
            REQUIRE(base && policy && concrete);
            CHECK(base.Value().Schema().flags == 0);
            CHECK(base.Value().Schema().fields.Size() == TypeOf<Base>().fields.Size());
            CHECK(policy.Value().Schema().flags == 0);
            CHECK(concrete.Value().Schema().flags == 0);
            CHECK(concrete.Value().Schema().fields.Size() == TypeOf<AbstractBase>().fields.Size());
            CHECK(policy.Value().Ops().construct != nullptr);
            CHECK(concrete.Value().Ops().construct != nullptr);
        }

        {
            ObjectRegistry objects(allocator, types, {0x4e52, 3});
            const auto access = objects.OwnerAccess();
            {
                const uint32 policyConstructions = AbstractByPolicy::constructions;
                auto policy = objects.NewObject<AbstractByPolicy>(
                    access, ObjectTypeIdOf<NativeOnPolicyAbstract>());
                auto concrete =
                    objects.NewObject<AbstractBase>(access, ObjectTypeIdOf<NativeOnAbstract>());
                REQUIRE(policy && concrete);
                CHECK(AbstractByPolicy::constructions == policyConstructions + 1);
                // 리플렉션 base 자체는 여전히 생성할 수 없다.
                CHECK(objects.NewObject<AbstractBase>(access, TypeIdOf<AbstractBase>()).Error() ==
                      ErrorCode::Unsupported);

                auto lease = types.Acquire(ObjectTypeIdOf<NativeOnAbstract>());
                REQUIRE(lease);
                const Guid baseValueId = SchemaField(TypeOf<AbstractBase>(), "baseValue").id;
                const RuntimeFieldOps* baseValue =
                    lease.Value().FieldOps(lease.Value().FieldIndex(baseValueId));
                REQUIRE(baseValue != nullptr);
                FieldValue read(allocator);
                REQUIRE(baseValue->read(objects, access, concrete.Value().Handle(), read));
                CHECK(read.kind == ValueKind::Int32 && read.signedInteger == 17);
                CHECK(baseValue
                          ->write(objects, access, concrete.Value().Handle(), Signed(allocator, 3))
                          .Error() == ErrorCode::Unsupported);
                auto borrowed = objects.Resolve<AbstractBase>(access, concrete.Value().Handle());
                REQUIRE(borrowed);
                AbstractBase* pointer = Get(borrowed.Value());
                REQUIRE(pointer != nullptr);
                CHECK(pointer->ClassValue() == 5);
            }

            REQUIRE(objects.Shutdown(access));
        }

        REQUIRE(types.Unregister(onBase.Value()));
        REQUIRE(types.Unregister(onPolicy.Value()));
        REQUIRE(types.Unregister(onAbstract.Value()));
        REQUIRE(registration.Unregister());
    }

    CHECK(types.Shutdown());
}

// 리플렉션 조상의 struct 필드가 가리키는 struct가 등록되지 않았으면 base가 있어도 NotFound다. base
// SceneFixture 자리를 Transform 필드 없이 수동 등록해 struct만 빠진 상태를 만들고,
// CoreTypeBindings를 등록하면 같은 native 등록이 struct 의존을 붙잡고 성공한다.
TEST(NativeReflectedBase_MissingStructDependencyIsNotFound) {
    SystemAllocator allocator;
    TypeRegistry types(allocator);
    {
        auto world = GeneratedType<WorldFixture>::Register(types);
        REQUIRE(world);
        TypeSchema schema;
        schema.id = TypeIdOf<SceneFixture>();
        schema.name = Span<const char>("SceneFixture", 12);
        schema.baseType = TypeIdOf<WorldFixture>();
        schema.flags = static_cast<uint32>(TypeFlags::WorldOnly);
        schema.fields = TypeOf<WorldFixture>().fields;
        RuntimeTypeOps ops = MakeRuntimeTypeOps<SceneFixture>();
        ops.fields = GeneratedType<WorldFixture>::Ops().fields;
        ops.nativeBaseType = NativeTypeKey<WorldFixture>();
        ops.trace = [](const Object& object, TraceVisitor& visitor) noexcept {
            GeneratedType<WorldFixture>::Trace(static_cast<const WorldFixture&>(object), visitor);
        };
        auto partial = types.Register(schema, Move(ops));
        REQUIRE(partial);
        CHECK(!types.AcquireValueType(TypeIdOf<Transform>(), TypeKind::Struct));
        CHECK(RegisterNativeClass<NativeSceneActor>(types).Error() == ErrorCode::NotFound);
        {
            ModuleRegistration bindings(types, allocator);
            REQUIRE(generated::CoreTypeBindings::RegisterModuleTypes(bindings));
            auto scene = RegisterNativeClass<NativeSceneActor>(types);
            REQUIRE(scene);
            {
                auto lease = types.Acquire(ObjectTypeIdOf<NativeSceneActor>());
                REQUIRE(lease);
                const Guid relativeId = SchemaField(TypeOf<SceneFixture>(), "relative").id;
                const StructView transform =
                    lease.Value().FieldStruct(lease.Value().FieldIndex(relativeId));
                REQUIRE(!transform.IsEmpty());
                CHECK(transform.Schema().id == TypeIdOf<Transform>());
            }

            // native 등록이 Transform struct를 의존으로 붙잡으므로 그동안 struct 해제는 Busy다.
            auto busy = bindings.Unregister();
            CHECK(!busy && busy.Error() == ErrorCode::Busy);
            REQUIRE(types.Unregister(scene.Value()));
            REQUIRE(bindings.Unregister());
        }

        REQUIRE(types.Unregister(partial.Value()));
        REQUIRE(types.Unregister(world.Value()));
    }

    CHECK(types.Shutdown());
}

// private·protected 손 Trace도 등록이 찾아 부른다(ROCKS_NATIVE_TYPE이 접근자를 friend로 선언한다 —
// 공개 이름만 보면 조용히 건너뛰어 GC 구멍이 된다). 기반의 protected Trace는 파생이 자기 Trace를
// 선언하든 안 하든 사슬이 한 번 부르고, 간선으로만 닿는 객체는 수집 뒤에도 산다.
TEST(NativeClass_PrivateAndProtectedTraceAreVisited) {
    SystemAllocator allocator;
    {
        TypeRegistry types(allocator);
        ModuleRegistration registration(types, allocator);
        REQUIRE(generated::ReflectionTestFixtures::RegisterModuleTypes(registration));
        auto privateToken = RegisterNativeClass<NativePrivateTrace>(types);
        auto protectedToken = RegisterNativeClass<NativeProtectedTrace>(types);
        auto leafToken = RegisterNativeClass<NativeProtectedLeaf>(types);
        auto plainToken = RegisterNativeClass<NativeProtectedPlain>(types);
        REQUIRE(privateToken && protectedToken && leafToken && plainToken);
        {
            ObjectRegistry objects(allocator, types, {0x4e52, 4});
            const auto access = objects.OwnerAccess();
            auto hiddenOwner =
                objects.NewObject<NativePrivateTrace>(access, ObjectTypeIdOf<NativePrivateTrace>());
            auto leafOwner = objects.NewObject<NativeProtectedLeaf>(
                access, ObjectTypeIdOf<NativeProtectedLeaf>());
            auto plainOwner = objects.NewObject<NativeProtectedPlain>(
                access, ObjectTypeIdOf<NativeProtectedPlain>());
            REQUIRE(hiddenOwner && leafOwner && plainOwner);
            ObjectHandle targets[5];
            for (ObjectHandle& target : targets) {
                auto created = objects.NewObject<Node>(access, TypeIdOf<Node>());
                REQUIRE(created);
                target = created.Value().Handle();
            }

            {
                auto hidden =
                    objects.Resolve<NativePrivateTrace>(access, hiddenOwner.Value().Handle());
                auto leaf =
                    objects.Resolve<NativeProtectedLeaf>(access, leafOwner.Value().Handle());
                auto plain =
                    objects.Resolve<NativeProtectedPlain>(access, plainOwner.Value().Handle());
                REQUIRE(hidden && leaf && plain);
                NativePrivateTrace* hiddenPointer = Get(hidden.Value());
                NativeProtectedLeaf* leafPointer = Get(leaf.Value());
                NativeProtectedPlain* plainPointer = Get(plain.Value());
                REQUIRE(hiddenPointer && leafPointer && plainPointer);
                REQUIRE(objects.SetReference(access, hiddenOwner.Value().Handle(),
                                             hiddenPointer->target, targets[0]));
                REQUIRE(objects.SetReference(access, hiddenOwner.Value().Handle(),
                                             hiddenPointer->hidden, targets[1]));
                REQUIRE(objects.SetReference(access, leafOwner.Value().Handle(),
                                             leafPointer->guarded, targets[2]));
                REQUIRE(objects.SetReference(access, leafOwner.Value().Handle(), leafPointer->own,
                                             targets[3]));
                REQUIRE(objects.SetReference(access, plainOwner.Value().Handle(),
                                             plainPointer->guarded, targets[4]));

                // 생성 Trace(target) + private 손 Trace(hidden), 기반 protected + 파생 private,
                // 기반 protected만 — 각 간선을 한 번씩.
                auto visit = [&](TypeId type, const Object& object, VisitLog& log) noexcept {
                    auto lease = types.Acquire(type);
                    if (lease) {
                        TraceVisitor visitor(&log, &VisitLog::Record);
                        lease.Value().Ops().trace(object, visitor);
                    }
                };
                VisitLog hiddenLog;
                visit(ObjectTypeIdOf<NativePrivateTrace>(), *hiddenPointer, hiddenLog);
                CHECK(hiddenLog.total == 2);
                CHECK(hiddenLog.CountOf(targets[0]) == 1 && hiddenLog.CountOf(targets[1]) == 1);
                VisitLog leafLog;
                visit(ObjectTypeIdOf<NativeProtectedLeaf>(), *leafPointer, leafLog);
                CHECK(leafLog.total == 2);
                CHECK(leafLog.CountOf(targets[2]) == 1 && leafLog.CountOf(targets[3]) == 1);
                VisitLog plainLog;
                visit(ObjectTypeIdOf<NativeProtectedPlain>(), *plainPointer, plainLog);
                CHECK(plainLog.total == 1);
                CHECK(plainLog.CountOf(targets[4]) == 1);
            }

            // Node 다섯은 루트를 놓았고 세 소유자의 간선으로만 닿는다.
            REQUIRE(objects.Collect(access));
            CHECK(objects.Stats().liveObjects == 8);
            for (ObjectHandle target : targets) {
                CHECK(objects.Resolve<Node>(access, target).IsOk());
            }

            hiddenOwner.Value().Reset();
            leafOwner.Value().Reset();
            plainOwner.Value().Reset();
            REQUIRE(objects.Collect(access));
            CHECK(objects.Stats().liveObjects == 0);
            REQUIRE(objects.Shutdown(access));
        }

        REQUIRE(types.Unregister(leafToken.Value()));
        REQUIRE(types.Unregister(plainToken.Value()));
        REQUIRE(types.Unregister(protectedToken.Value()));
        REQUIRE(types.Unregister(privateToken.Value()));
        REQUIRE(registration.Unregister());
        CHECK(types.Shutdown());
    }

    CHECK(allocator.Stats().liveAllocations == 0);
}
