#include <ReflectionTestFixtures/ClassFlagFixture.generated.h>
#include <ReflectionTestFixtures/ReflectionTestFixtures.module.generated.h>
#include "MapTestSupport.h"

// 리플렉션 B9h2 클래스 플래그 시험이다(Docs/Design/ReflectionExtensions.ko.md 9.2절, fixture
// ReflectionTestFixtures/ClassFlagFixture.h): 생성 스키마의 Transient·Deprecated 상속과 Within,
// 등록의 상속 검사, 저장 정책 Exclude(저장 불가 객체를 가리키는 강한·weak 참조 null + 진단 3·4,
// Required 위치, 집합·root 거부)와 Keep(보통 노드), 로드 진단(TransientType·DeprecatedType),
// ClassRef 쓰기의 Deprecated 거부와 decode 진단·재저장, Within 생성 규칙(Registry 생성·그래프 1단계
// 노드 Outer·기준 Outer).
using namespace rocks;
namespace class_flag_graph_tests_file {

using namespace rocks::test::map;
using reflection_fixture::ClassFlagHolder;
using reflection_fixture::DeprecatedThing;
using reflection_fixture::EditorOnlyThing;
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
            external5 += diagnostic.value == 5 ? 1 : 0;
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
    usize external5 = 0;
    usize transient = 0;
    usize deprecated = 0;
    usize missing = 0;
    TypeId lastDeprecated{};
};

// 모든 외부 참조를 Import로 받고 물어본 수를 센다(Admit 경로).
class ImportEverything final : public FieldCodecHooks {
public:
    Result<ExternalAdmission>
    AdmitExternalReference(const ExternalReferenceInfo&) noexcept override {
        ++asked;
        return Result<ExternalAdmission>::Success(ExternalAdmission::Import);
    }

    Result<void> Report(const FieldDiagnostic& diagnostic) noexcept override {
        external3 +=
            diagnostic.kind == FieldDiagnosticKind::ExternalReference && diagnostic.value == 3;
        return Result<void>::Success();
    }

    usize asked = 0;
    usize external3 = 0;
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
    // 파생이 Within을 다시 정하면 base Within의 파생이어야 한다(좁히기만 — 등록된 Within 클래스로
    // 판정).
    REQUIRE(GeneratedType<WithinOuter>::Register(types));
    REQUIRE(GeneratedType<WithinOuterChild>::Register(types));
    inner.withinType = TypeIdOf<KindBase>();
    CHECK(types.Register(inner, GeneratedType<WithinInnerChild>::Ops()).Error() ==
          ErrorCode::InvalidArgument);
    inner.withinType = TypeIdOf<WithinOuterChild>();
    auto narrowed = types.Register(inner, GeneratedType<WithinInnerChild>::Ops());
    REQUIRE(narrowed);
    CHECK(types.Unregister(narrowed.Value()));
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
            value.weak = WeakObjectRef<Object>(deprecated.Handle());
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

// 저장 불가 마스크(에셋 A4e2b — 쿠킹 쓰기가 EditorOnly를 더한다): 기본 마스크는 EditorOnly 객체를
// 보통 대상으로 저장하고, EditorOnly를 더하면 그 참조를 null + 진단 5로 쓰며 그것을 root·집합에
// 두면 InvalidArgument다. Transient 대상은 마스크와 무관하게 진단 3이다.
TEST(ClassFlagGraph_UnsaveableMaskAddsEditorOnly) {
    SystemAllocator allocator;
    ClassFlagFixture fixture(allocator);
    ObjectRegistry objects(allocator, fixture.types, {0x434647, 9});
    {
        RootRef<ClassFlagHolder> holder = New<ClassFlagHolder>(objects);
        RootRef<EditorOnlyThing> editor = New<EditorOnlyThing>(objects);
        RootRef<TransientChild> transient = New<TransientChild>(objects);
        With<ClassFlagHolder>(objects, holder.Handle(), [&](ClassFlagHolder& value) noexcept {
            const auto access = objects.OwnerAccess();
            CHECK(objects.SetReference(access, holder.Handle(), value.strong, editor.Handle()));
            CHECK(objects.SetReference(access, holder.Handle(), value.needed, transient.Handle()));
        });
        CHECK(HasFlag(GeneratedType<EditorOnlyThing>::Schema().flags, TypeFlags::EditorOnly));

        ObjectGraphSaveOptions options;
        ClassFlagDiagnostics plain;
        options.hooks = &plain;
        REQUIRE(
            SaveObjectGraph(allocator, objects, objects.OwnerAccess(), holder.Handle(), options));
        CHECK(plain.external5 == 0 && plain.external3 == 1);

        ClassFlagDiagnostics masked;
        options.hooks = &masked;
        options.unsaveableTypeFlags =
            UnsaveableTypeFlags | static_cast<uint32>(TypeFlags::EditorOnly);
        auto bytes =
            SaveObjectGraph(allocator, objects, objects.OwnerAccess(), holder.Handle(), options);
        REQUIRE(bytes);
        CHECK(masked.external5 == 1 && masked.external3 == 1);
        {
            ObjectRegistry target(allocator, fixture.types, {0x434647, 10});
            {
                auto loaded = LoadObjectGraph(allocator, target, fixture.types,
                                              target.OwnerAccess(), bytes.Value().View());
                REQUIRE(loaded);
                With<ClassFlagHolder>(
                    target, loaded.Value().Handle(),
                    [](ClassFlagHolder& value) noexcept { CHECK(value.strong.Handle().IsNull()); });
            }

            REQUIRE(target.Collect(target.OwnerAccess()));
            REQUIRE(target.Shutdown(target.OwnerAccess()));
        }

        options.hooks = nullptr;
        auto root =
            SaveObjectGraph(allocator, objects, objects.OwnerAccess(), editor.Handle(), options);
        CHECK(!root && root.Error() == ErrorCode::InvalidArgument);
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

        // 같은 값의 재기록은 받는다(asset-info → asset-set 왕복·undo — 등록된 write와 원소 경로
        // 쓰기). 다른 값에서 Deprecated로 바꾸거나 새 원소로 넣으면 거부한다.
        CHECK(write(TypeIdOf<DeprecatedThing>()) == ErrorCode::None);
        With<ClassFlagHolder>(objects, holder.Handle(), [](ClassFlagHolder& value) noexcept {
            REQUIRE(value.kinds.TryPushBack(ClassRef<KindBase>(TypeIdOf<DeprecatedThing>())));
        });
        const auto writeElement = [&](TypeId type) noexcept {
            FieldValue value(allocator);
            value.kind = ValueKind::ClassReference;
            value.guid = type;
            return ErrorOf(PathWrite(objects, objects.OwnerAccess(), holder.Handle(),
                                     PathText(lease.Value(), "kinds[0]").View(), value));
        };

        CHECK(writeElement(TypeIdOf<DeprecatedThing>()) == ErrorCode::None);
        CHECK(writeElement(TypeIdOf<TransientThing>()) == ErrorCode::None);
        CHECK(writeElement(TypeIdOf<DeprecatedThing>()) == ErrorCode::InvalidArgument);
        FieldValue inserted(allocator);
        inserted.kind = ValueKind::ClassReference;
        inserted.guid = TypeIdOf<DeprecatedThing>();
        CHECK(ErrorOf(PathInsertElement(objects, objects.OwnerAccess(), holder.Handle(),
                                        PathText(lease.Value(), "kinds[0]").View(), &inserted)) ==
              ErrorCode::InvalidArgument);
        With<ClassFlagHolder>(objects, holder.Handle(), [](ClassFlagHolder& value) noexcept {
            REQUIRE(value.kinds.Size() == 1);
            CHECK(value.kinds[0].Id() == TypeIdOf<TransientThing>());
            CHECK(value.kind.Id() == TypeIdOf<DeprecatedThing>());
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

// 저장 불가 조상(Outer 사슬): 저장 가능한 클래스의 객체라도 Outer가 저장 불가 클래스면 그 참조는
// null + 진단(3·4)이고 Include가 대상과 조상을 넣지 않는다(UE SavePackage가 저장 불가 Outer 아래
// 객체를 빼는 것과 같다 — 부분 트리 저장 불가 판정). Admit은 hook을 묻지 않고 import하지 않는다.
TEST(ClassFlagGraph_UnsaveableAncestorsAreExcluded) {
    SystemAllocator allocator;
    ClassFlagFixture fixture(allocator);
    ObjectRegistry objects(allocator, fixture.types, {0x434641, 1});
    {
        const auto access = objects.OwnerAccess();
        RootRef<ClassFlagHolder> holder = New<ClassFlagHolder>(objects);
        RootRef<TransientThing> transient = New<TransientThing>(objects);
        RootRef<DeprecatedThing> deprecated = New<DeprecatedThing>(objects);
        auto underTransient =
            objects.NewObject<KindBase>(access, TypeIdOf<KindBase>(), transient.Handle(), "x");
        auto underDeprecated =
            objects.NewObject<KindBase>(access, TypeIdOf<KindBase>(), deprecated.Handle(), "y");
        REQUIRE(underTransient && underDeprecated);
        With<ClassFlagHolder>(objects, holder.Handle(), [&](ClassFlagHolder& value) noexcept {
            CHECK(objects.SetReference(access, holder.Handle(), value.strong,
                                       underTransient.Value().Handle()));
            value.weak = WeakObjectRef<Object>(underDeprecated.Value().Handle());
        });

        ClassFlagDiagnostics included;
        auto bytes =
            Save(allocator, objects, holder.Handle(), UnsaveablePolicy::Exclude, &included);
        REQUIRE(bytes);
        CHECK(included.external3 == 1 && included.external4 == 1);
        {
            ObjectRegistry target(allocator, fixture.types, {0x434641, 2});
            {
                auto loaded = LoadObjectGraph(allocator, target, fixture.types,
                                              target.OwnerAccess(), bytes.Value().View());
                REQUIRE(loaded);
                With<ClassFlagHolder>(target, loaded.Value().Handle(),
                                      [](ClassFlagHolder& value) noexcept {
                                          CHECK(value.strong.Handle().IsNull());
                                          CHECK(value.weak.Handle().IsNull());
                                      });
                REQUIRE(target.Collect(target.OwnerAccess()));
                CHECK(target.Stats().liveObjects == 1);
            }

            REQUIRE(target.Collect(target.OwnerAccess()));
            REQUIRE(target.Shutdown(target.OwnerAccess()));
        }

        // Admit: 저장 불가 사슬의 대상은 hook에 묻지 않고 null로 쓴다.
        ImportEverything admit;
        ObjectGraphSaveOptions options;
        options.externals = GraphExternalPolicy::Admit;
        options.hooks = &admit;
        const ObjectHandle set[] = {holder.Handle()};
        auto admitted = SaveObjectGraphSet(allocator, objects, access,
                                           Span<const ObjectHandle>(set, 1), options);
        REQUIRE(admitted);
        CHECK(admit.asked == 0 && admit.external3 == 1);

        // Keep은 조상까지 보통 대상이다(Include가 Outer 사슬을 더한다).
        ClassFlagDiagnostics kept;
        CHECK(Save(allocator, objects, holder.Handle(), UnsaveablePolicy::Keep, &kept).IsOk());
        CHECK(kept.external3 == 0 && kept.external4 == 0);
    }

    REQUIRE(objects.Collect(objects.OwnerAccess()));
    REQUIRE(objects.Shutdown(objects.OwnerAccess()));
}

// 클래스를 나중에 Transient로 바꾼 패키지: 그 클래스가 저장 가능할 때 만든 파일을 Transient로
// 등록한 Registry가 읽으면 노드는 만들고 TransientType 진단을 내며, Exclude로 다시 저장하면 그
// 객체와 참조가 빠진다(패키지를 열고 고친다).
TEST(ClassFlagGraph_LaterTransientPackageResavesWithoutObject) {
    SystemAllocator allocator;
    ClassFlagFixture fixture(allocator);
    TypeRegistry changed(allocator);
    REQUIRE(GeneratedType<ClassFlagHolder>::Register(changed));
    TypeSchema transientOuter = GeneratedType<WithinOuter>::Schema();
    transientOuter.flags |= static_cast<uint32>(TypeFlags::Transient);
    REQUIRE(changed.Register(transientOuter, GeneratedType<WithinOuter>::Ops()));
    {
        ObjectRegistry objects(allocator, fixture.types, {0x43464c, 1});
        ObjectRegistry reopened(allocator, changed, {0x43464c, 2});
        {
            RootRef<ClassFlagHolder> holder = New<ClassFlagHolder>(objects);
            RootRef<WithinOuter> outer = New<WithinOuter>(objects);
            With<ClassFlagHolder>(objects, holder.Handle(), [&](ClassFlagHolder& value) noexcept {
                CHECK(objects.SetReference(objects.OwnerAccess(), holder.Handle(), value.strong,
                                           outer.Handle()));
            });

            auto bytes = Save(allocator, objects, holder.Handle(), UnsaveablePolicy::Exclude);
            REQUIRE(bytes);
            ClassFlagDiagnostics loads;
            ObjectGraphLoadOptions options;
            options.hooks = &loads;
            auto loaded = LoadObjectGraph(allocator, reopened, changed, reopened.OwnerAccess(),
                                          bytes.Value().View(), options);
            REQUIRE(loaded);
            CHECK(loads.transient == 1);
            ClassFlagDiagnostics resaved;
            auto again = Save(allocator, reopened, loaded.Value().Handle(),
                              UnsaveablePolicy::Exclude, &resaved);
            REQUIRE(again);
            CHECK(resaved.external3 == 1);
            ObjectRegistry checked(allocator, fixture.types, {0x43464c, 3});
            {
                auto fixed = LoadObjectGraph(allocator, checked, fixture.types,
                                             checked.OwnerAccess(), again.Value().View());
                REQUIRE(fixed);
                With<ClassFlagHolder>(
                    checked, fixed.Value().Handle(),
                    [](ClassFlagHolder& value) noexcept { CHECK(value.strong.Handle().IsNull()); });
            }

            REQUIRE(checked.Collect(checked.OwnerAccess()));
            REQUIRE(checked.Shutdown(checked.OwnerAccess()));
        }

        REQUIRE(objects.Collect(objects.OwnerAccess()));
        REQUIRE(objects.Shutdown(objects.OwnerAccess()));
        REQUIRE(reopened.Collect(reopened.OwnerAccess()));
        REQUIRE(reopened.Shutdown(reopened.OwnerAccess()));
    }

    CHECK(changed.Shutdown());
}

// 단계형 복원 1단계(Validate — 어느 스레드에서나, Registry에 닿지 않는다)는 기준 Outer 요약의
// 타입을 읽지 않는다: Within 밖 타입(KindBase)을 가리키는 요약으로도 기준 Outer 아래 WithinInner
// 노드를 받는다(판정은 CreateObjects·실제 생성 — 위 WithinOuterRule의 기준 Outer 사례).
TEST(ClassFlagGraph_StageValidateDoesNotReadBaseOuterType) {
    SystemAllocator allocator;
    ClassFlagFixture fixture(allocator);
    ObjectRegistry objects(allocator, fixture.types, {0x434656, 1});
    {
        auto wrong = fixture.types.Acquire(TypeIdOf<KindBase>());
        REQUIRE(wrong);
        Array<uint8> record = RecordOf(allocator, TypeIdOf<WithinInner>(), 1, {});
        const test::graph::Node node{TypeIdOf<WithinInner>(), 0, "based", record.View()};
        test::graph::Header header;
        header.flags = test::graph::NodeTableFlag | test::graph::BaseOuterFlag;
        auto graph = test::graph::Build(allocator, Span<const test::graph::Node>(&node, 1), header);
        REQUIRE(graph);
        ObjectGraphStage stage(allocator, objects, fixture.types, objects.OwnerAccess(), {});
        ObjectGraphInput input;
        input.bytes = graph.Value().View();
        input.hasBase = true;
        input.base = OuterSummary::ForNewOutermost(false, &wrong.Value());
        CHECK(stage.Validate(input).IsOk());
    }

    REQUIRE(objects.Shutdown(objects.OwnerAccess()));
}
} // namespace class_flag_graph_tests_file
