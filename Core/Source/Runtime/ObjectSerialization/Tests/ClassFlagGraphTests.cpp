#include <ReflectionTestFixtures/ReflectionTestFixtures.module.generated.h>
#include "MapTestSupport.h"

// 리플렉션 B9h2 클래스 플래그 시험이다(Docs/Design/ReflectionExtensions.ko.md 9.2절, fixture
// ReflectionTestFixtures/ClassFlagFixture.h): 생성 스키마의 Transient·Deprecated 상속과 Within,
// 등록의 상속 검사, 저장 정책 Exclude(저장 불가 객체를 가리키는 강한·weak 참조 null + 진단 3·4,
// Required 위치, 집합·root 거부)와 Keep(보통 노드), 로드 진단(TransientType·DeprecatedType),
// ClassRef 쓰기의 Deprecated 거부와 decode 진단·재저장, Within 생성 규칙(Registry 생성·그래프 1단계
// 노드 Outer·기준 Outer).
using namespace rocks;
using namespace rocks::test::map;
using reflection_fixture::ClassFlagHolder;
using reflection_fixture::DeprecatedThing;
using reflection_fixture::KindBase;
using reflection_fixture::TransientChild;
using reflection_fixture::TransientThing;
using reflection_fixture::WithinInner;
using reflection_fixture::WithinInnerChild;
using reflection_fixture::WithinOuter;
using reflection_fixture::WithinOuterChild;

namespace {

// 모듈 집계 등록 묶음이다.
struct ClassFlagFixture {
    explicit ClassFlagFixture(Allocator& allocator) noexcept
        : types(allocator), registration(types, allocator) {
        REQUIRE(generated::ReflectionTestFixtures::RegisterModuleTypes(registration));
    }

    ~ClassFlagFixture() noexcept {
        CHECK(registration.Unregister());
        CHECK(types.Shutdown());
    }

    TypeRegistry types;
    ModuleRegistration registration;
};

// 진단을 종류·값별로 센다.
class ClassFlagDiagnostics final : public FieldCodecHooks {
public:
    Result<void> Report(const FieldDiagnostic& diagnostic) noexcept override {
        if (diagnostic.kind == FieldDiagnosticKind::ExternalReference) {
            external3 += diagnostic.value == 3 ? 1 : 0;
            external4 += diagnostic.value == 4 ? 1 : 0;
        } else if (diagnostic.kind == FieldDiagnosticKind::TransientType) {
            ++transient;
        } else if (diagnostic.kind == FieldDiagnosticKind::DeprecatedType) {
            ++deprecated;
            lastDeprecated = diagnostic.valueType;
        } else if (diagnostic.kind == FieldDiagnosticKind::RequiredMissing) {
            ++missing;
        }

        return Result<void>::Success();
    }

    usize external3 = 0;
    usize external4 = 0;
    usize transient = 0;
    usize deprecated = 0;
    usize missing = 0;
    TypeId lastDeprecated{};
};

Result<Array<uint8>> Save(Allocator& allocator, ObjectRegistry& objects, ObjectHandle root,
                          UnsaveablePolicy unsaveable, FieldCodecHooks* hooks = nullptr,
                          bool rejectMissingRequired = false) noexcept {
    ObjectGraphSaveOptions options;
    options.unsaveable = unsaveable;
    options.hooks = hooks;
    options.policy.rejectMissingRequired = rejectMissingRequired;
    return SaveObjectGraph(allocator, objects, objects.OwnerAccess(), root, options);
}

// outer(WithinOuterChild)와 그 Inner(WithinInner)를 집합 저장한 그래프다. replaceOuter면
// NodeTable의 첫 노드 타입(WithinOuterChild의 Guid wire)을 KindBase로 바꾼다(노드 Outer의 타입이
// Within 밖인 손상 파일).
Array<uint8> WithinGraph(Allocator& allocator, ObjectRegistry& objects, ObjectHandle outer,
                         ObjectHandle inner, bool replaceOuter) noexcept {
    const ObjectHandle set[] = {outer, inner};
    ObjectGraphSaveOptions options;
    options.writeNodeTable = true;
    auto saved = SaveObjectGraphSet(allocator, objects, objects.OwnerAccess(),
                                    Span<const ObjectHandle>(set, 2), options);
    if (!saved) {
        platform::Fatal("test graph save failed");
    }

    Array<uint8> bytes = saved.Value().TakeBytes();
    if (replaceOuter) {
        ArchiveWriter from(allocator);
        ArchiveWriter to(allocator);
        if (!from.WriteGuid(TypeIdOf<WithinOuterChild>()) || !to.WriteGuid(TypeIdOf<KindBase>())) {
            platform::Fatal("test guid write failed");
        }

        const Array<uint8> pattern = from.TakeBytes();
        const Array<uint8> replacement = to.TakeBytes();
        usize found = 0;
        for (usize at = 0; at + pattern.Size() <= bytes.Size(); ++at) {
            bool same = true;
            for (usize index = 0; index < pattern.Size() && same; ++index) {
                same = bytes[at + index] == pattern[index];
            }

            if (same) {
                for (usize index = 0; index < pattern.Size(); ++index) {
                    bytes[at + index] = replacement[index];
                }

                ++found;
            }
        }

        if (found == 0) {
            platform::Fatal("test node type not found");
        }
    }

    return bytes;
}

} // namespace

// 생성 스키마: Transient·Deprecated 비트와 상속(TransientChild), Within(WithinInner와 그 파생).
// 등록은 상속 플래그·Within을 빠뜨린 파생과 Within을 단 struct를 InvalidArgument로 거부한다.
TEST(ClassFlagGraph_SchemaFlagsInheritanceAndRegistry) {
    CHECK(HasFlag(TypeOf<TransientThing>().flags, TypeFlags::Transient));
    CHECK(HasFlag(TypeOf<TransientChild>().flags, TypeFlags::Transient));
    CHECK(HasFlag(TypeOf<DeprecatedThing>().flags, TypeFlags::Deprecated));
    CHECK((TypeOf<KindBase>().flags & UnsaveableTypeFlags) == 0);
    CHECK(TypeOf<WithinInner>().withinType == TypeIdOf<WithinOuter>());
    CHECK(TypeOf<WithinInnerChild>().withinType == TypeIdOf<WithinOuter>());
    CHECK(TypeOf<WithinOuter>().withinType == TypeId{});

    SystemAllocator allocator;
    TypeRegistry types(allocator);
    REQUIRE(GeneratedType<KindBase>::Register(types));
    REQUIRE(GeneratedType<TransientThing>::Register(types));
    REQUIRE(GeneratedType<WithinInner>::Register(types));
    TypeSchema child = GeneratedType<TransientChild>::Schema();
    child.flags &= ~static_cast<uint32>(TypeFlags::Transient);
    CHECK(types.Register(child, GeneratedType<TransientChild>::Ops()).Error() ==
          ErrorCode::InvalidArgument);
    TypeSchema inner = GeneratedType<WithinInnerChild>::Schema();
    inner.withinType = TypeId{};
    CHECK(types.Register(inner, GeneratedType<WithinInnerChild>::Ops()).Error() ==
          ErrorCode::InvalidArgument);
    CHECK(GeneratedType<TransientChild>::Register(types));
    CHECK(GeneratedType<WithinInnerChild>::Register(types));
    CHECK(types.Shutdown());
}

// 저장 정책 Exclude(기본): 저장 불가 객체를 가리키는 강한·weak 참조는 null로 쓰고 진단
// 3(Transient)· 4(Deprecated)를 남긴다(Include도 그 객체를 넣지 않는다). Required 위치면
// rejectMissingRequired가 InvalidArgument다. 저장 불가 root는 InvalidArgument다. Keep은 같은 객체를
// 보통 노드로 저장하고 로드가 TransientType 진단을 낸다(노드는 정상으로 만든다).
TEST(ClassFlagGraph_ExcludeAndKeepPolicies) {
    SystemAllocator allocator;
    ClassFlagFixture fixture(allocator);
    ObjectRegistry objects(allocator, fixture.types, {0x434647, 1});
    {
        RootRef<ClassFlagHolder> holder = New<ClassFlagHolder>(objects);
        RootRef<TransientChild> transient = New<TransientChild>(objects);
        RootRef<DeprecatedThing> deprecated = New<DeprecatedThing>(objects);
        RootRef<KindBase> plain = New<KindBase>(objects);
        With<ClassFlagHolder>(objects, holder.Handle(), [&](ClassFlagHolder& value) noexcept {
            const auto access = objects.OwnerAccess();
            CHECK(objects.SetReference(access, holder.Handle(), value.strong, transient.Handle()));
            CHECK(objects.SetReference(access, holder.Handle(), value.needed, plain.Handle()));
            value.weak = WeakRef<Object>(deprecated.Handle());
        });

        ClassFlagDiagnostics saved;
        auto bytes = Save(allocator, objects, holder.Handle(), UnsaveablePolicy::Exclude, &saved);
        REQUIRE(bytes);
        CHECK(saved.external3 == 1 && saved.external4 == 1);
        {
            ObjectRegistry target(allocator, fixture.types, {0x434647, 2});
            {
                auto loaded = LoadObjectGraph(allocator, target, fixture.types,
                                              target.OwnerAccess(), bytes.Value().View());
                REQUIRE(loaded);
                With<ClassFlagHolder>(target, loaded.Value().Handle(),
                                      [](ClassFlagHolder& value) noexcept {
                                          CHECK(value.strong.Handle().IsNull());
                                          CHECK(value.weak.Handle().IsNull());
                                          CHECK(!value.needed.Handle().IsNull());
                                      });
                REQUIRE(target.Collect(target.OwnerAccess()));
                CHECK(target.Stats().liveObjects == 2);
            }

            REQUIRE(target.Collect(target.OwnerAccess()));
            REQUIRE(target.Shutdown(target.OwnerAccess()));
        }

        // Required 위치의 저장 불가 대상은 rejectMissingRequired면 InvalidArgument다.
        With<ClassFlagHolder>(objects, holder.Handle(), [&](ClassFlagHolder& value) noexcept {
            CHECK(objects.SetReference(objects.OwnerAccess(), holder.Handle(), value.needed,
                                       deprecated.Handle()));
        });
        ClassFlagDiagnostics strict;
        auto rejected =
            Save(allocator, objects, holder.Handle(), UnsaveablePolicy::Exclude, &strict, true);
        CHECK(!rejected && rejected.Error() == ErrorCode::InvalidArgument);
        CHECK(strict.missing == 1);
        CHECK(Save(allocator, objects, holder.Handle(), UnsaveablePolicy::Exclude).IsOk());

        // 저장 불가 root는 Exclude에서 InvalidArgument이고 Keep은 저장한다.
        auto root = Save(allocator, objects, transient.Handle(), UnsaveablePolicy::Exclude);
        CHECK(!root && root.Error() == ErrorCode::InvalidArgument);

        // Keep: 저장 불가 객체도 보통 노드다. 로드는 노드를 만들고 진단을 낸다.
        ClassFlagDiagnostics kept;
        auto snapshot = Save(allocator, objects, holder.Handle(), UnsaveablePolicy::Keep, &kept);
        REQUIRE(snapshot);
        CHECK(kept.external3 == 0 && kept.external4 == 0);
        {
            ObjectRegistry target(allocator, fixture.types, {0x434647, 3});
            {
                ClassFlagDiagnostics loads;
                ObjectGraphLoadOptions options;
                options.hooks = &loads;
                auto loaded =
                    LoadObjectGraph(allocator, target, fixture.types, target.OwnerAccess(),
                                    snapshot.Value().View(), options);
                REQUIRE(loaded);
                CHECK(loads.transient == 1 && loads.deprecated == 1);
                With<ClassFlagHolder>(target, loaded.Value().Handle(),
                                      [&](ClassFlagHolder& value) noexcept {
                                          CHECK(target
                                                    .Resolve<TransientChild>(target.OwnerAccess(),
                                                                             value.strong.Handle())
                                                    .IsOk());
                                          CHECK(target
                                                    .Resolve<DeprecatedThing>(target.OwnerAccess(),
                                                                              value.needed.Handle())
                                                    .IsOk());
                                      });
            }

            REQUIRE(target.Collect(target.OwnerAccess()));
            REQUIRE(target.Shutdown(target.OwnerAccess()));
        }
    }

    REQUIRE(objects.Collect(objects.OwnerAccess()));
    REQUIRE(objects.Shutdown(objects.OwnerAccess()));
}

// ClassRef 쓰기(경로 쓰기 — 등록된 write의 FieldWriteOperation)는 Deprecated 클래스를
// InvalidArgument로 거부하고 값은 그대로다(Transient 클래스는 받는다). C++가 직접 넣은 Deprecated
// 값은 저장되고, 로드가 DeprecatedType 진단(valueType = 그 클래스)을 내며 값을 그대로 읽는다.
TEST(ClassFlagGraph_ClassRefWritesRejectDeprecatedAndDecodeReports) {
    SystemAllocator allocator;
    ClassFlagFixture fixture(allocator);
    ObjectRegistry objects(allocator, fixture.types, {0x434652, 1});
    {
        auto lease = fixture.types.Acquire(TypeIdOf<ClassFlagHolder>());
        REQUIRE(lease);
        RootRef<ClassFlagHolder> holder = New<ClassFlagHolder>(objects);
        const auto write = [&](TypeId type) noexcept {
            FieldValue value(allocator);
            value.kind = ValueKind::ClassReference;
            value.guid = type;
            return ErrorOf(PathWrite(objects, objects.OwnerAccess(), holder.Handle(),
                                     PathText(lease.Value(), "kind").View(), value));
        };

        CHECK(write(TypeIdOf<TransientThing>()) == ErrorCode::None);
        CHECK(write(TypeIdOf<DeprecatedThing>()) == ErrorCode::InvalidArgument);
        With<ClassFlagHolder>(objects, holder.Handle(), [](ClassFlagHolder& value) noexcept {
            CHECK(value.kind.Id() == TypeIdOf<TransientThing>());
            value.kind = ClassRef<KindBase>(TypeIdOf<DeprecatedThing>());
        });

        auto bytes = Save(allocator, objects, holder.Handle(), UnsaveablePolicy::Exclude);
        REQUIRE(bytes);
        ObjectRegistry target(allocator, fixture.types, {0x434652, 2});
        {
            ClassFlagDiagnostics loads;
            ObjectGraphLoadOptions options;
            options.hooks = &loads;
            auto loaded = LoadObjectGraph(allocator, target, fixture.types, target.OwnerAccess(),
                                          bytes.Value().View(), options);
            REQUIRE(loaded);
            CHECK(loads.deprecated == 1 && loads.lastDeprecated == TypeIdOf<DeprecatedThing>());
            With<ClassFlagHolder>(target, loaded.Value().Handle(),
                                  [](ClassFlagHolder& value) noexcept {
                                      CHECK(value.kind.Id() == TypeIdOf<DeprecatedThing>());
                                  });
            auto resaved =
                Save(allocator, target, loaded.Value().Handle(), UnsaveablePolicy::Exclude);
            CHECK(resaved.IsOk());
        }

        REQUIRE(target.Collect(target.OwnerAccess()));
        REQUIRE(target.Shutdown(target.OwnerAccess()));
    }

    REQUIRE(objects.Collect(objects.OwnerAccess()));
    REQUIRE(objects.Shutdown(objects.OwnerAccess()));
}

// Within: Registry 생성은 Outer가 Within 클래스 또는 파생일 때만 받고(파생 클래스도 이어받는다),
// 다른 Outer·Outer 없음은 InvalidArgument다. 그래프 1단계는 노드 Outer의 타입으로, 기준 Outer는
// 실제 객체 타입으로 같은 규칙을 판정한다.
TEST(ClassFlagGraph_WithinOuterRule) {
    SystemAllocator allocator;
    ClassFlagFixture fixture(allocator);
    ObjectRegistry objects(allocator, fixture.types, {0x434657, 1});
    {
        const auto access = objects.OwnerAccess();
        RootRef<WithinOuter> outer = New<WithinOuter>(objects);
        RootRef<WithinOuterChild> derived = New<WithinOuterChild>(objects);
        RootRef<KindBase> wrong = New<KindBase>(objects);
        const auto create = [&](TypeId type, ObjectHandle within) noexcept {
            auto created = objects.NewObject<Object>(access, type, within, "inner");
            return created ? ErrorCode::None : created.Error();
        };

        CHECK(create(TypeIdOf<WithinInner>(), outer.Handle()) == ErrorCode::None);
        CHECK(create(TypeIdOf<WithinInnerChild>(), derived.Handle()) == ErrorCode::None);
        CHECK(create(TypeIdOf<WithinInner>(), wrong.Handle()) == ErrorCode::InvalidArgument);
        CHECK(create(TypeIdOf<WithinInnerChild>(), wrong.Handle()) == ErrorCode::InvalidArgument);
        auto orphan = objects.NewObject<Object>(access, TypeIdOf<WithinInner>());
        CHECK(!orphan && orphan.Error() == ErrorCode::InvalidArgument);

        auto graphInner =
            objects.NewObject<Object>(access, TypeIdOf<WithinInner>(), derived.Handle(), "graph");
        REQUIRE(graphInner);
        Array<uint8> good =
            WithinGraph(allocator, objects, derived.Handle(), graphInner.Value().Handle(), false);
        // 그래프 안 Outer는 1단계(Validate)가 노드 표의 타입으로 판정한다(편의 복원은 참조로 닿지
        // 않는 Inner를 고아로 거부하므로 단계형 1단계로 본다).
        const auto validate = [&](const Array<uint8>& bytes) noexcept {
            ObjectGraphStage stage(allocator, objects, fixture.types, objects.OwnerAccess(), {});
            ObjectGraphInput input;
            input.bytes = bytes.View();
            auto validated = stage.Validate(input);
            return validated ? ErrorCode::None : validated.Error();
        };

        CHECK(validate(good) == ErrorCode::None);
        Array<uint8> bad =
            WithinGraph(allocator, objects, derived.Handle(), graphInner.Value().Handle(), true);
        CHECK(validate(bad) == ErrorCode::InvalidArgument);

        // 기준 Outer(outer 0 노드의 Outer)는 실제 객체 타입으로 판정한다.
        Array<uint8> record = RecordOf(allocator, TypeIdOf<WithinInner>(), 1, {});
        const test::graph::Node node{TypeIdOf<WithinInner>(), 0, "based", record.View()};
        test::graph::Header header;
        header.flags = test::graph::NodeTableFlag | test::graph::BaseOuterFlag;
        auto based = test::graph::Build(allocator, Span<const test::graph::Node>(&node, 1), header);
        REQUIRE(based);
        ObjectGraphLoadOptions options;
        options.baseOuter = outer.Handle();
        CHECK(LoadCode(allocator, objects, fixture.types, based.Value().View(), options) ==
              ErrorCode::None);
        options.baseOuter = wrong.Handle();
        CHECK(LoadCode(allocator, objects, fixture.types, based.Value().View(), options) ==
              ErrorCode::InvalidArgument);
    }

    REQUIRE(objects.Collect(objects.OwnerAccess()));
    REQUIRE(objects.Shutdown(objects.OwnerAccess()));
}
