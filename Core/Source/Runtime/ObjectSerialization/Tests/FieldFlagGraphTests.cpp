#include <CoreTypeBindings/CoreValueTypes.generated.h>
#include <ReflectionTestFixtures/FieldFlagFixture.generated.h>
#include <ReflectionTestFixtures/ReflectionTestFixtures.module.generated.h>
#include "MapTestSupport.h"

// 리플렉션 B9h1 필드 플래그 시험이다(Docs/Design/ReflectionExtensions.ko.md 9.1절, fixture
// ReflectionTestFixtures/FieldFlagFixture.h): 생성 스키마의 플래그 비트와 Transform 미러 멤버
// SaveGame, Required + 복제 제외 플래그 등록 거부, 복제 정책(에셋·PIE)별 저장 집합, includeFlags
// 저장·로드 대칭(필드·struct 멤버·원소 멤버마다 자기 플래그, Atomic struct는 통째), Required 실효
// 값 검사의 포함 필터 밖 필드 제외.
using namespace rocks;
using namespace rocks::test::map;
using reflection_fixture::FlagAtom;
using reflection_fixture::FlagContainers;
using reflection_fixture::FlagHolder;
using reflection_fixture::FlagSaveAtom;
using reflection_fixture::FlagSaveList;
using reflection_fixture::FlagSaveSlot;
using reflection_fixture::FlagSlot;

namespace {

constexpr uint32 SaveGameFlag = static_cast<uint32>(FieldFlags::SaveGame);

// 모듈 집계 등록 묶음이다.
struct FlagFixture {
    explicit FlagFixture(Allocator& allocator) noexcept
        : types(allocator), registration(types, allocator) {
        REQUIRE(generated::ReflectionTestFixtures::RegisterModuleTypes(registration));
    }

    ~FlagFixture() noexcept {
        CHECK(registration.Unregister());
        CHECK(types.Shutdown());
    }

    TypeRegistry types;
    ModuleRegistration registration;
};

// 진단 종류별 수를 센다.
class FlagDiagnostics final : public FieldCodecHooks {
public:
    Result<void> Report(const FieldDiagnostic& diagnostic) noexcept override {
        missing += diagnostic.kind == FieldDiagnosticKind::RequiredMissing ? 1 : 0;
        return Result<void>::Success();
    }

    usize missing = 0;
};

// 모든 필드를 기본값과 다른 값으로 채운다(source는 null — Required 실효 값 검사용).
void Fill(ObjectRegistry& objects, ObjectHandle handle) noexcept {
    With<FlagHolder>(objects, handle, [](FlagHolder& value) noexcept {
        value.duplicateOnly = 1;
        value.instance = 2;
        value.textOnly = 3;
        value.score = 4;
        value.plain = 5;
        value.slot.kept = 6;
        value.slot.dropped = 7;
        REQUIRE(value.slots.TryPushBack(FlagSlot{8, 9}));
        value.atom.a = 10;
        value.atom.b = 11;
        value.pose.translation = Vec3d{1.0, 2.0, 3.0};
        value.pose.scale = Vec3d{2.0, 2.0, 2.0};
    });
}

Result<Array<uint8>> Save(Allocator& allocator, ObjectRegistry& objects, ObjectHandle root,
                          FieldEncodePolicy policy) noexcept {
    ObjectGraphSaveOptions options;
    options.policy = policy;
    options.writeNodeTable = true;
    return SaveObjectGraph(allocator, objects, objects.OwnerAccess(), root, options);
}

// bytes를 policy로 새 Registry에 복원해 check로 본다(복원 실패면 그 코드).
template <class Check>
ErrorCode LoadAndCheck(Allocator& allocator, TypeRegistry& types, Span<const uint8> bytes,
                       FieldDecodePolicy policy, FieldCodecHooks* hooks, Check&& check) noexcept {
    ObjectRegistry target(allocator, types, {0x464c47, 9});
    ErrorCode result = ErrorCode::None;
    {
        ObjectGraphLoadOptions options;
        options.policy = policy;
        options.hooks = hooks;
        auto loaded =
            LoadObjectGraph(allocator, target, types, target.OwnerAccess(), bytes, options);
        if (loaded) {
            check(target, loaded.Value().Handle());
        } else {
            result = loaded.Error();
        }
    }

    CHECK(target.Collect(target.OwnerAccess()));
    CHECK(target.Shutdown(target.OwnerAccess()));
    return result;
}

} // namespace

// 생성 스키마가 지정자마다 비트를 싣고(DuplicateTransient 16·NonPIEDuplicateTransient 32·SaveGame
// 64·TextExportTransient 128) Transform 미러(비Atomic)의 멤버는 SaveGame이다. 등록은 Required와
// 복제 제외 플래그의 조합을 InvalidArgument로 거부하고 각 플래그만은 받는다.
TEST(FieldFlagGraph_SchemaCarriesFlagsAndRegistryRejectsRequiredDuplicate) {
    const TypeSchema& schema = TypeOf<FlagHolder>();
    CHECK(SchemaField(schema, "duplicateOnly").flags ==
          static_cast<uint32>(FieldFlags::DuplicateTransient));
    CHECK(SchemaField(schema, "instance").flags ==
          static_cast<uint32>(FieldFlags::NonPIEDuplicateTransient));
    CHECK(SchemaField(schema, "textOnly").flags ==
          static_cast<uint32>(FieldFlags::TextExportTransient));
    CHECK(SchemaField(schema, "score").flags == SaveGameFlag);
    CHECK(SchemaField(schema, "plain").flags == 0);
    const TypeSchema& transform = GeneratedType<Transform>::Schema();
    REQUIRE(transform.fields.Size() == 3);
    for (const FieldSchema& member : transform.fields) {
        CHECK(member.flags == SaveGameFlag);
    }

    SystemAllocator allocator;
    TypeRegistry types(allocator);
    const uint32 duplicateFlags[] = {static_cast<uint32>(FieldFlags::DuplicateTransient),
                                     static_cast<uint32>(FieldFlags::NonPIEDuplicateTransient)};
    for (uint32 flag : duplicateFlags) {
        const bool requiredCases[] = {false, true};
        for (bool required : requiredCases) {
            FieldSchema field;
            field.id = Guid{0x464c46, 1};
            field.name = Span<const char>("source", 6);
            field.flags = flag | (required ? static_cast<uint32>(FieldFlags::Required) : 0u);
            field.value = ValueType{ValueKind::SoftObjectReference, {}};
            RuntimeFieldOps ops;
            ops.fieldId = field.id;
            ops.read = [](ObjectRegistry&, const ObjectAccessContext&, ObjectHandle,
                          FieldValue&) noexcept { return Result<void>::Success(); };
            ops.write = [](ObjectRegistry&, const ObjectAccessContext&, ObjectHandle,
                           const FieldValue&) noexcept { return Result<void>::Success(); };
            ops.writeStaged = [](ConstructionContext&, ObjectHandle, const FieldValue&) noexcept {
                return Result<void>::Success();
            };
            auto typeOps = MakeRuntimeTypeOps<FlagSaveAtom>();
            typeOps.fields = Span<const RuntimeFieldOps>(&ops, 1);
            auto token = types.Register(
                {TypeId{0x464c43, 1}, 1, Span<const FieldSchema>(&field, 1), {"Manual", 6}},
                Move(typeOps));
            CHECK(token.IsOk() == !required);
            if (token) {
                CHECK(types.Unregister(token.Value()));
            } else {
                CHECK(token.Error() == ErrorCode::InvalidArgument);
            }
        }
    }

    CHECK(types.Shutdown());
}

// 복제 정책은 호출자의 excludedFlags로 정한다: 에셋 복제(DuplicateTransient |
// NonPIEDuplicateTransient)는 두 필드를 빼고, PIE 복제(DuplicateTransient)는 duplicateOnly만 뺀다.
// 빠진 필드는 복제본에서 기준 값(생성자 값)이고 나머지는 그대로다. TextExportTransient는 그래프
// 저장과 무관하다(텍스트 codec이 쓴다).
TEST(FieldFlagGraph_DuplicatePoliciesChooseSavedFields) {
    SystemAllocator allocator;
    FlagFixture fixture(allocator);
    ObjectRegistry objects(allocator, fixture.types, {0x464c44, 1});
    {
        RootRef<FlagHolder> holder = New<FlagHolder>(objects);
        Fill(objects, holder.Handle());
        const uint32 asset = static_cast<uint32>(FieldFlags::DuplicateTransient) |
                             static_cast<uint32>(FieldFlags::NonPIEDuplicateTransient);
        const uint32 pie = static_cast<uint32>(FieldFlags::DuplicateTransient);
        const uint32 policies[] = {asset, pie};
        for (uint32 excluded : policies) {
            auto bytes =
                Save(allocator, objects, holder.Handle(), FieldEncodePolicy{false, excluded});
            REQUIRE(bytes);
            CHECK(LoadAndCheck(allocator, fixture.types, bytes.Value().View(), {}, nullptr,
                               [&](ObjectRegistry& target, ObjectHandle handle) noexcept {
                                   With<FlagHolder>(
                                       target, handle, [&](FlagHolder& value) noexcept {
                                           CHECK(value.duplicateOnly == 0);
                                           CHECK(value.instance == (excluded == pie ? 2 : 0));
                                           CHECK(value.textOnly == 3 && value.score == 4 &&
                                                 value.plain == 5 && value.slot.dropped == 7);
                                       });
                               }) == ErrorCode::None);
        }
    }

    REQUIRE(objects.Collect(objects.OwnerAccess()));
    REQUIRE(objects.Shutdown(objects.OwnerAccess()));
}

// includeFlags = SaveGame 저장은 필드·struct 멤버·원소 멤버마다 자기 플래그를 본다: SaveGame 필드
// 안의 SaveGame이 아닌 멤버(slot.dropped·slots[i].dropped)는 쓰지 않고, Atomic struct(atom)는
// 통째로, Transform(미러 멤버 SaveGame)은 모든 성분을 쓴다. 그 파일은 같은 포함 필터로도 복원된다.
// 전부 저장한 파일을 포함 필터로 읽으면 SaveGame이 아닌 최상위 필드·struct 멤버·원소 멤버 때문에
// InvalidArgument이고, Atomic struct의 멤버는 보지 않아 성공한다.
TEST(FieldFlagGraph_IncludeFlagsSaveAndLoadPerMember) {
    SystemAllocator allocator;
    FlagFixture fixture(allocator);
    ObjectRegistry objects(allocator, fixture.types, {0x464c49, 1});
    {
        RootRef<FlagHolder> holder = New<FlagHolder>(objects);
        Fill(objects, holder.Handle());
        FieldEncodePolicy saveGame;
        saveGame.includeFlags = SaveGameFlag;
        auto bytes = Save(allocator, objects, holder.Handle(), saveGame);
        REQUIRE(bytes);
        FieldDecodePolicy includeLoad;
        includeLoad.includeFlags = SaveGameFlag;
        const FieldDecodePolicy loads[] = {FieldDecodePolicy{}, includeLoad};
        for (const FieldDecodePolicy& policy : loads) {
            CHECK(LoadAndCheck(
                      allocator, fixture.types, bytes.Value().View(), policy, nullptr,
                      [](ObjectRegistry& target, ObjectHandle handle) noexcept {
                          With<FlagHolder>(target, handle, [](FlagHolder& value) noexcept {
                              CHECK(value.duplicateOnly == 0 && value.instance == 0 &&
                                    value.textOnly == 0 && value.plain == 0);
                              CHECK(value.score == 4);
                              CHECK(value.slot.kept == 6 && value.slot.dropped == 0);
                              REQUIRE(value.slots.Size() == 1);
                              CHECK(value.slots[0].kept == 8 && value.slots[0].dropped == 0);
                              CHECK(value.atom.a == 10 && value.atom.b == 11);
                              CHECK(value.pose.translation.y == 2.0 && value.pose.scale.z == 2.0f);
                          });
                      }) == ErrorCode::None);
        }

        auto full = Save(allocator, objects, holder.Handle(), {});
        REQUIRE(full);
        CHECK(LoadAndCheck(allocator, fixture.types, full.Value().View(), includeLoad, nullptr,
                           [](ObjectRegistry&, ObjectHandle) noexcept {}) ==
              ErrorCode::InvalidArgument);
    }

    // 최상위가 모두 SaveGame인 클래스의 전부 저장 파일: struct 멤버·원소 멤버의 dropped가 포함 필터
    // 밖이라 거부되고, Atomic struct의 멤버는 통째라 받는다.
    {
        RootRef<FlagSaveSlot> slot = New<FlagSaveSlot>(objects);
        RootRef<FlagSaveList> list = New<FlagSaveList>(objects);
        RootRef<FlagSaveAtom> atom = New<FlagSaveAtom>(objects);
        With<FlagSaveList>(objects, list.Handle(), [](FlagSaveList& value) noexcept {
            REQUIRE(value.slots.TryPushBack(FlagSlot{1, 2}));
        });

        FieldDecodePolicy includeLoad;
        includeLoad.includeFlags = SaveGameFlag;
        const ObjectHandle roots[] = {slot.Handle(), list.Handle(), atom.Handle()};
        const ErrorCode expected[] = {ErrorCode::InvalidArgument, ErrorCode::InvalidArgument,
                                      ErrorCode::None};
        for (usize index = 0; index < 3; ++index) {
            auto full = Save(allocator, objects, roots[index], {});
            REQUIRE(full);
            CHECK(LoadAndCheck(allocator, fixture.types, full.Value().View(), includeLoad, nullptr,
                               [](ObjectRegistry&, ObjectHandle) noexcept {}) == expected[index]);
            FieldEncodePolicy saveGame;
            saveGame.includeFlags = SaveGameFlag;
            auto included = Save(allocator, objects, roots[index], saveGame);
            REQUIRE(included);
            CHECK(LoadAndCheck(allocator, fixture.types, included.Value().View(), includeLoad,
                               nullptr,
                               [](ObjectRegistry&, ObjectHandle) noexcept {}) == ErrorCode::None);
        }
    }

    REQUIRE(objects.Collect(objects.OwnerAccess()));
    REQUIRE(objects.Shutdown(objects.OwnerAccess()));
}

// 복원의 Required 실효 값 검사(2c)는 포함 필터 밖의 필드를 뺀다: SaveGame 저장 파일을 포함 필터로
// 읽으면 null인 Required 필드 source(SaveGame 아님)를 보고하지 않고, 필터 없이 읽으면 보고한다.
TEST(FieldFlagGraph_RequiredCheckSkipsFieldsOutsideInclude) {
    SystemAllocator allocator;
    FlagFixture fixture(allocator);
    ObjectRegistry objects(allocator, fixture.types, {0x464c52, 1});
    {
        RootRef<FlagHolder> holder = New<FlagHolder>(objects);
        FieldEncodePolicy saveGame;
        saveGame.includeFlags = SaveGameFlag;
        auto bytes = Save(allocator, objects, holder.Handle(), saveGame);
        REQUIRE(bytes);
        FieldDecodePolicy includeLoad;
        includeLoad.includeFlags = SaveGameFlag;
        FlagDiagnostics filtered;
        CHECK(LoadAndCheck(allocator, fixture.types, bytes.Value().View(), includeLoad, &filtered,
                           [](ObjectRegistry&, ObjectHandle) noexcept {}) == ErrorCode::None);
        CHECK(filtered.missing == 0);
        FlagDiagnostics unfiltered;
        CHECK(LoadAndCheck(allocator, fixture.types, bytes.Value().View(), {}, &unfiltered,
                           [](ObjectRegistry&, ObjectHandle) noexcept {}) == ErrorCode::None);
        CHECK(unfiltered.missing == 1);
    }

    REQUIRE(objects.Collect(objects.OwnerAccess()));
    REQUIRE(objects.Shutdown(objects.OwnerAccess()));
}

// 컨테이너 안 struct의 멤버도 포함 필터를 자기 플래그로 본다(Map 값·Optional 내부·정적 배열 원소 —
// EncodeStructRecord·DecodeStructRecord의 같은 길): SaveGame 저장은 각 자리의 dropped를 쓰지 않고
// kept만 쓰며, 전부 저장한 파일을 포함 필터로 읽으면 InvalidArgument다.
TEST(FieldFlagGraph_IncludeFlagsReachContainerStructMembers) {
    SystemAllocator allocator;
    FlagFixture fixture(allocator);
    ObjectRegistry objects(allocator, fixture.types, {0x464c43, 1});
    {
        RootRef<FlagContainers> holder = New<FlagContainers>(objects);
        With<FlagContainers>(objects, holder.Handle(), [](FlagContainers& value) noexcept {
            Put(value.byId, int32{1}, FlagSlot{2, 3});
            value.maybe.Emplace(FlagSlot{4, 5});
            value.fixed[1] = FlagSlot{6, 7};
        });

        FieldEncodePolicy saveGame;
        saveGame.includeFlags = SaveGameFlag;
        auto bytes = Save(allocator, objects, holder.Handle(), saveGame);
        REQUIRE(bytes);
        FieldDecodePolicy includeLoad;
        includeLoad.includeFlags = SaveGameFlag;
        CHECK(LoadAndCheck(
                  allocator, fixture.types, bytes.Value().View(), includeLoad, nullptr,
                  [](ObjectRegistry& target, ObjectHandle handle) noexcept {
                      With<FlagContainers>(target, handle, [](FlagContainers& value) noexcept {
                          const FlagSlot* slot = value.byId.Find(int32{1});
                          REQUIRE(slot);
                          CHECK(slot->kept == 2 && slot->dropped == 0);
                          REQUIRE(value.maybe.HasValue());
                          CHECK(value.maybe->kept == 4 && value.maybe->dropped == 0);
                          CHECK(value.fixed[1].kept == 6 && value.fixed[1].dropped == 0);
                      });
                  }) == ErrorCode::None);
        auto full = Save(allocator, objects, holder.Handle(), {});
        REQUIRE(full);
        CHECK(LoadAndCheck(allocator, fixture.types, full.Value().View(), includeLoad, nullptr,
                           [](ObjectRegistry&, ObjectHandle) noexcept {}) ==
              ErrorCode::InvalidArgument);
    }

    REQUIRE(objects.Collect(objects.OwnerAccess()));
    REQUIRE(objects.Shutdown(objects.OwnerAccess()));
}
