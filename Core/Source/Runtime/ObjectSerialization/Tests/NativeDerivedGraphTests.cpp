#include <ReflectionTestFixtures/ReflectionTestFixtures.module.generated.h>
#include <Core/Memory/SystemAllocator.h>
#include <Object/NativeClass.h>
#include <ObjectSerialization/ObjectGraph.h>
#include <TestSupport/Test.h>

// 리플렉션 base를 둔 native 파생의 객체 그래프 저장·로드 시험이다(ReflectionFoundations 9.1절 B8a).
// native 파생은 base 필드를 빌려도 자기 C++ 상태를 모르므로 completeFieldLayout = false이고, 그
// 객체를 루트로 저장하거나 그래프가 그 객체에 닿으면 Unsupported, 그 타입 노드를 로드해도
// Unsupported다. 같은 C++ 타입·같은 빌린 필드를 completeFieldLayout만 선언해 다른 ID로 등록하면
// 저장·로드되므로 원인이 레이아웃 선언임을 확인한다. 등록·필드·경로·Trace는 ObjectRuntime
// 시험(NativeReflectedBaseTests)이 본다.
using namespace rocks;
using reflection_fixture::ArrayOwner;
using reflection_fixture::WorldChild;
using reflection_fixture::WorldFixture;

namespace {

class NativeWorldSaver final : public WorldFixture {
    ROCKS_NATIVE_TYPE(NativeWorldSaver, WorldFixture, 0xdc37788ae31d48edull, 0x83e13f6e08b135aaull)

public:
    int32 runtimeOnly = 0;
};

// NativeWorldSaver를 다른 ID로 수동 등록하는 별칭이다(같은 native 타입·빌린 필드·생성 Trace).
constexpr TypeId SaverLayoutId{0x90d21ad8e18542f1ull, 0xb306a68e5cfdf4cfull};

// RegisterNativeClass<NativeWorldSaver>와 completeFieldLayout만 다르게(complete) 등록한다.
Result<TypeRegistrationToken> RegisterSaverLayout(TypeRegistry& types, bool complete) noexcept {
    TypeSchema schema;
    schema.id = SaverLayoutId;
    schema.name = Span<const char>("NativeWorldSaverLayout", 22);
    schema.baseType = TypeIdOf<WorldFixture>();
    schema.flags = static_cast<uint32>(TypeFlags::WorldOnly);
    schema.fields = GeneratedType<WorldFixture>::Schema().fields;
    RuntimeTypeOps ops = MakeRuntimeTypeOps<NativeWorldSaver>();
    ops.fields = GeneratedType<WorldFixture>::Ops().fields;
    ops.nativeBaseType = NativeTypeKey<WorldFixture>();
    ops.trace = [](const Object& object, TraceVisitor& visitor) noexcept {
        GeneratedType<WorldFixture>::Trace(static_cast<const WorldFixture&>(object), visitor);
    };
    ops.completeFieldLayout = complete;
    return types.Register(schema, Move(ops));
}

} // namespace

TEST(NativeReflectedBase_GraphSaveIsUnsupported) {
    SystemAllocator allocator;
    TypeRegistry types(allocator);
    {
        ModuleRegistration registration(types, allocator);
        REQUIRE(generated::ReflectionTestFixtures::RegisterModuleTypes(registration));
        auto token = RegisterNativeClass<NativeWorldSaver>(types);
        REQUIRE(token);
        {
            // 필드·codec은 모두 있고(빌린 생성 thunk) 레이아웃 선언만 없다.
            auto lease = types.Acquire(ObjectTypeIdOf<NativeWorldSaver>());
            REQUIRE(lease);
            CHECK(!lease.Value().Ops().completeFieldLayout);
            CHECK(lease.Value().Schema().fields.Size() == lease.Value().Ops().fields.Size());
            for (const RuntimeFieldOps& field : lease.Value().Ops().fields) {
                CHECK(field.encode != nullptr && field.decode != nullptr);
            }
        }

        {
            ObjectRegistry objects(allocator, types, {0x4e53, 1});
            const auto access = objects.OwnerAccess();
            {
                auto native =
                    objects.NewObject<WorldFixture>(access, ObjectTypeIdOf<NativeWorldSaver>());
                auto child = objects.NewObject<WorldChild>(access, TypeIdOf<WorldChild>());
                auto owner = objects.NewObject<ArrayOwner>(access, TypeIdOf<ArrayOwner>());
                REQUIRE(native && child && owner);

                // native 파생 루트는 Unsupported, 같은 리플렉션 base의 생성 파생은 저장된다.
                CHECK(
                    SaveObjectGraph(allocator, objects, access, native.Value().Handle()).Error() ==
                    ErrorCode::Unsupported);
                REQUIRE(SaveObjectGraph(allocator, objects, access, child.Value().Handle()));

                // 생성 타입의 그래프도 native 파생 객체에 닿으면 Unsupported다(Object 대상 참조
                // 배열).
                REQUIRE(SaveObjectGraph(allocator, objects, access, owner.Value().Handle()));
                {
                    auto borrowed = objects.Resolve<ArrayOwner>(access, owner.Value().Handle());
                    REQUIRE(borrowed);
                    auto pointer = borrowed.Value().Get();
                    REQUIRE(pointer);
                    REQUIRE(objects.AddReference(access, owner.Value().Handle(),
                                                 pointer.Value()->required,
                                                 native.Value().Handle()));
                }

                CHECK(SaveObjectGraph(allocator, objects, access, owner.Value().Handle()).Error() ==
                      ErrorCode::Unsupported);
            }

            REQUIRE(objects.Shutdown(access));
        }

        REQUIRE(types.Unregister(token.Value()));
        REQUIRE(registration.Unregister());
    }

    CHECK(types.Shutdown());
}

// 원인은 레이아웃 선언이다: 같은 C++ 타입·같은 빌린 필드를 completeFieldLayout = true로 등록한
// 별칭은 저장되고, 그 그래프를 레이아웃 선언이 없는 등록(RegisterNativeClass와 같은 모양)으로
// 로드하면 Unsupported, 선언이 있으면 값까지 복원된다.
TEST(NativeReflectedBase_GraphLoadIsUnsupportedWithoutLayout) {
    SystemAllocator allocator;
    TypeRegistry source(allocator);
    TypeRegistry target(allocator);
    {
        ModuleRegistration sourceModules(source, allocator);
        ModuleRegistration targetModules(target, allocator);
        REQUIRE(generated::ReflectionTestFixtures::RegisterModuleTypes(sourceModules));
        REQUIRE(generated::ReflectionTestFixtures::RegisterModuleTypes(targetModules));
        auto complete = RegisterSaverLayout(source, true);
        REQUIRE(complete);
        Array<uint8> graph(allocator);
        {
            ObjectRegistry objects(allocator, source, {0x4e53, 2});
            const auto access = objects.OwnerAccess();
            {
                auto saver = objects.NewObject<NativeWorldSaver>(access, SaverLayoutId);
                REQUIRE(saver);
                {
                    auto borrowed =
                        objects.Resolve<NativeWorldSaver>(access, saver.Value().Handle());
                    REQUIRE(borrowed);
                    auto pointer = borrowed.Value().Get();
                    REQUIRE(pointer);
                    pointer.Value()->note = 5;
                }

                auto saved = SaveObjectGraph(allocator, objects, access, saver.Value().Handle());
                REQUIRE(saved);
                graph = Move(saved.Value());
            }

            REQUIRE(objects.Shutdown(access));
        }

        {
            auto incomplete = RegisterSaverLayout(target, false);
            REQUIRE(incomplete);
            {
                ObjectRegistry objects(allocator, target, {0x4e53, 3});
                CHECK(
                    LoadObjectGraph(allocator, objects, target, objects.OwnerAccess(), graph.View())
                        .Error() == ErrorCode::Unsupported);
                CHECK(objects.Stats().liveObjects == 0);
                REQUIRE(objects.Shutdown(objects.OwnerAccess()));
            }

            REQUIRE(target.Unregister(incomplete.Value()));
        }

        {
            auto declared = RegisterSaverLayout(target, true);
            REQUIRE(declared);
            {
                ObjectRegistry objects(allocator, target, {0x4e53, 4});
                const auto access = objects.OwnerAccess();
                {
                    auto loaded = LoadObjectGraph(allocator, objects, target, access, graph.View());
                    REQUIRE(loaded);
                    auto borrowed =
                        objects.Resolve<NativeWorldSaver>(access, loaded.Value().Handle());
                    REQUIRE(borrowed);
                    auto pointer = borrowed.Value().Get();
                    REQUIRE(pointer);
                    CHECK(pointer.Value()->note == 5);
                }

                REQUIRE(objects.Shutdown(access));
            }

            REQUIRE(target.Unregister(declared.Value()));
        }

        REQUIRE(source.Unregister(complete.Value()));
        REQUIRE(sourceModules.Unregister());
        REQUIRE(targetModules.Unregister());
    }

    CHECK(source.Shutdown());
    CHECK(target.Shutdown());
}
