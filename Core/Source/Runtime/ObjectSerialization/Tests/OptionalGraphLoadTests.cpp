#include "OptionalTestSupport.h"

// 리플렉션 B9e Optional PROPERTY의 복원 세부
// 시험이다(Docs/Design/ReflectionExtensions.ko.md 1.2·6절): 최상위 Optional<struct> 내용의 1단계
// 선청구(2단계 몫), 옛 버전 내부 Record의 struct 마이그레이션, 관찰 struct 안의 Optional(관찰 임시
// 값·Optional<Name> 담은 struct 거부), Optional<struct> 안 Required의 저장·복원 2c 검사, Optional
// 내부 종류의 나머지(AssetId·double·uint64·int64) 왕복.
using namespace rocks;
using namespace rocks::test::optional;

namespace {

// 진단 종류별 수를 센다.
struct Diagnostics final : FieldCodecHooks {
    Result<void> Report(const FieldDiagnostic& diagnostic) noexcept override {
        if (diagnostic.kind == FieldDiagnosticKind::RequiredMissing) {
            ++requiredMissing;
            requiredPathSteps = static_cast<uint32>(diagnostic.path.Size());
        }

        return Result<void>::Success();
    }

    uint32 requiredMissing = 0;
    uint32 requiredPathSteps = 0;
};

// 관찰할 struct 하나를 요청하고 받은 값을 기록한다.
struct Observer final : FieldCodecHooks {
    explicit Observer(TypeId type) noexcept : id(type) {
    }

    Span<const TypeId> ObservedStructs() noexcept override {
        return Span<const TypeId>(&id, 1);
    }

    Result<void> ObserveStruct(const FieldCodecFrame&, TypeId type, const void*,
                               const void* value) noexcept override {
        ++observed;
        if (type == TypeIdOf<OptionalPoint>()) {
            const auto& point = *static_cast<const OptionalPoint*>(value);
            if (point.label.View() == "spot") {
                spotWeight = point.weight.HasValue() ? *point.weight : 0;
            }
        } else if (type == TypeIdOf<OptionalSlot>()) {
            const auto& slot = *static_cast<const OptionalSlot*>(value);
            slotPointX = slot.point.HasValue() ? slot.point->x : -1;
            slotNoted = slot.note.HasValue() && slot.note->View() == "n";
        }

        return Result<void>::Success();
    }

    TypeId id;
    uint32 observed = 0;
    uint32 spotWeight = 0;
    int32 slotPointX = 0;
    bool slotNoted = false;
};

// 옛 버전 1 OptionalVersioned Record를 버전 2로 올린다: value를 10배하고 extra를 1로 둔다. user는
// 받은 원본 Record byte 수를 기록한다.
Result<Array<uint8>> MigrateVersioned(Allocator& allocator, TypeId type, uint32 sourceVersion,
                                      uint32 targetVersion, Span<const uint8> source,
                                      void* user) noexcept {
    if (type != TypeIdOf<OptionalVersioned>() || sourceVersion != 1 || targetVersion != 2) {
        return Result<Array<uint8>>::Failure(ErrorCode::Unsupported);
    }

    *static_cast<usize*>(user) = source.Size();
    auto reader = RecordReader::Open(allocator, source, type, 1, 1);
    if (!reader) {
        return Result<Array<uint8>>::Failure(reader.Error());
    }

    int64 value = 0;
    const Guid valueId = SchemaField(TypeOf<OptionalVersioned>(), "value").id;
    for (usize index = 0; index < reader.Value().FieldCount(); ++index) {
        auto field = reader.Value().Next();
        if (!field) {
            return Result<Array<uint8>>::Failure(field.Error());
        }

        if (field.Value().id == valueId) {
            auto archive = reader.Value().OpenValue(field.Value());
            if (!archive) {
                return Result<Array<uint8>>::Failure(archive.Error());
            }

            auto read = archive.Value().ReadI64();
            if (!read) {
                return Result<Array<uint8>>::Failure(read.Error());
            }

            value = read.Value();
        }
    }

    ArchiveWriter scaled(allocator);
    ArchiveWriter extra(allocator);
    if (!scaled.WriteI64(value * 10) || !extra.WriteI64(1)) {
        return Result<Array<uint8>>::Failure(ErrorCode::OutOfMemory);
    }

    const FieldBytes fields[] = {{valueId, static_cast<uint32>(WireType::I64), scaled.View()},
                                 {SchemaField(TypeOf<OptionalVersioned>(), "extra").id,
                                  static_cast<uint32>(WireType::I64), extra.View()}};
    return Result<Array<uint8>>::Success(RecordOf(allocator, type, 2, fields));
}

} // namespace

// 최상위 Optional<struct>의 내용은 2단계가 단독 reader로 넘기고(내용 byte) 단독 Record로 다시
// 열므로(payload byte) 1단계가 그 두 배를 미리 청구한다(1단계 청구가 2단계 몫을 덮는다). struct
// 필드 안의 Optional<struct>는 그 struct 필드가 payload를 한 번 미리 청구하고 Optional 자신은
// 청구하지 않는다. 그래서 내부 label이 Δ byte 길어질 때 1단계 최소 작업량의 증가는 최상위가 struct
// 안보다 정확히 Δ 크다(나머지 몫 — 객체 Record·문자열 읽기 — 은 두 자리가 같다).
TEST(OptionalGraph_TopLevelStructContentIsPrechargedTwice) {
    SystemAllocator allocator;
    TypeRegistry types(allocator);
    REQUIRE(RegisterFixture(types));
    {
        ObjectRegistry objects(allocator, types, {0x4f5047, 20});
        constexpr usize LabelCapacity = 512;
        const auto minimum = [&](bool topLevel, usize labelBytes) noexcept -> int64 {
            char label[LabelCapacity] = {};
            for (usize index = 0; index < labelBytes && index < LabelCapacity; ++index) {
                label[index] = 'a';
            }

            Array<uint8> bytes(allocator);
            {
                RootRef<OptionalHolder> holder = New(objects);
                With(objects, holder.Handle(), [&](OptionalHolder& value) noexcept {
                    OptionalPoint& point =
                        topLevel ? value.point.Emplace(objects.ObjectAllocator())
                                 : value.slot.point.Emplace(objects.ObjectAllocator());
                    REQUIRE(point.label.TryAssign(StringView(label, labelBytes)));
                });
                auto saved =
                    SaveObjectGraph(allocator, objects, objects.OwnerAccess(), holder.Handle());
                CHECK(saved);
                if (!saved) {
                    return 0;
                }

                bytes = Move(saved.Value());
            }

            return static_cast<int64>(MinimumValidateWork(allocator, objects, types, bytes.View()));
        };

        constexpr int64 Grown = 256;
        const int64 topLevel = minimum(true, Grown + 8) - minimum(true, 8);
        const int64 nested = minimum(false, Grown + 8) - minimum(false, 8);
        CHECK(topLevel - nested == Grown);
        REQUIRE(objects.Collect(objects.OwnerAccess()));
        REQUIRE(objects.Shutdown(objects.OwnerAccess()));
    }

    CHECK(types.Shutdown());
}

// 옛 버전(1) 내부 Record의 Optional<OptionalVersioned>는 struct 마이그레이션으로 현재 버전(2)이
// 된다: 콜백은 Optional 내부 Record byte 전부(Record 내용 출처)를 원본으로 받고, 결과의 멤버가
// 적용된다.
TEST(OptionalGraph_InnerRecordMigrationUsesTheContentBytes) {
    SystemAllocator allocator;
    TypeRegistry types(allocator);
    REQUIRE(RegisterFixture(types));
    {
        ObjectRegistry objects(allocator, types, {0x4f5047, 21});
        ArchiveWriter value(allocator);
        REQUIRE(value.WriteI64(5));
        const FieldBytes members[] = {{SchemaField(TypeOf<OptionalVersioned>(), "value").id,
                                       static_cast<uint32>(WireType::I64), value.View()}};
        auto old = RecordOf(allocator, TypeIdOf<OptionalVersioned>(), 1, members);
        auto payload =
            OptionalPayload(allocator, true, WireType::Record, [&](ArchiveWriter& out) noexcept {
                return out.WriteRaw(old.View()).IsOk();
            });
        const FieldBytes fields[] = {
            {HolderField("versioned"), static_cast<uint32>(WireType::Optional), payload.View()}};
        auto graph = HolderGraph(allocator, fields);
        usize received = 0;
        ObjectGraphLoadOptions options;
        options.migrateStruct = &MigrateVersioned;
        options.migrationUser = &received;
        auto loaded = LoadObjectGraph(allocator, objects, types, objects.OwnerAccess(),
                                      graph.View(), options);
        REQUIRE(loaded);
        CHECK(received == old.Size());
        With(objects, loaded.Value().Handle(), [](OptionalHolder& holder) noexcept {
            REQUIRE(holder.versioned.HasValue());
            CHECK(holder.versioned->value == 50 && holder.versioned->extra == 1);
        });

        // 마이그레이션이 없으면 옛 버전은 Unsupported다.
        CHECK(!LoadObjectGraph(allocator, objects, types, objects.OwnerAccess(), graph.View()));
        loaded.Value().Reset();
        REQUIRE(objects.Shutdown(objects.OwnerAccess()));
    }

    CHECK(types.Shutdown());
}

// 관찰 struct 안의 Optional: 최상위 Optional<OptionalPoint>의 관찰 임시 값은 Optional<uint32>
// 멤버를 담고, 관찰한 OptionalSlot은 Optional<OptionalPoint>·Optional<String> 멤버를 관찰 임시
// 값(ObservationScratch) 안에 만든다. Optional<Name>을 담는 struct(OptionalTag — Name 담김 형질이
// Optional 내부로 전파된다)는 관찰할 수 없다(Validate InvalidArgument).
TEST(OptionalGraph_ObservedStructsSeeOptionalMembers) {
    SystemAllocator allocator;
    TypeRegistry types(allocator);
    REQUIRE(RegisterFixture(types));
    {
        ObjectRegistry objects(allocator, types, {0x4f5047, 22});
        Array<uint8> bytes(allocator);
        {
            RootRef<OptionalHolder> holder = New(objects);
            With(objects, holder.Handle(), [&](OptionalHolder& value) noexcept {
                OptionalPoint& point = value.point.Emplace(objects.ObjectAllocator());
                REQUIRE(point.label.TryAssign("spot"));
                point.weight.Emplace(9u);
                value.slot.point.Emplace(objects.ObjectAllocator()).x = 11;
                value.slot.note.Emplace(String(objects.ObjectAllocator()));
                REQUIRE(value.slot.note->TryAssign("n"));
            });
            auto saved =
                SaveObjectGraph(allocator, objects, objects.OwnerAccess(), holder.Handle());
            REQUIRE(saved);
            bytes = Move(saved.Value());
        }

        const auto validate = [&](FieldCodecHooks& hooks) noexcept {
            ObjectGraphLoadOptions options;
            options.hooks = &hooks;
            ObjectGraphStage stage(allocator, objects, types, objects.OwnerAccess(), options);
            ObjectGraphInput input;
            input.bytes = bytes.View();
            return ErrorOf(stage.Validate(input));
        };

        Observer points(TypeIdOf<OptionalPoint>());
        CHECK(validate(points) == ErrorCode::None);
        CHECK(points.observed >= 2 && points.spotWeight == 9);
        Observer slots(TypeIdOf<OptionalSlot>());
        CHECK(validate(slots) == ErrorCode::None);
        CHECK(slots.observed == 1 && slots.slotPointX == 11 && slots.slotNoted);

        auto tag = types.AcquireValueType(TypeIdOf<OptionalTag>(), TypeKind::Struct);
        REQUIRE(tag);
        CHECK(tag.Value().Struct().ContainsName());
        Observer tags(TypeIdOf<OptionalTag>());
        CHECK(validate(tags) == ErrorCode::InvalidArgument);
        CHECK(tags.observed == 0);
        REQUIRE(objects.Collect(objects.OwnerAccess()));
        REQUIRE(objects.Shutdown(objects.OwnerAccess()));
    }

    CHECK(types.Shutdown());
}

// Optional<struct> 안의 Required는 set일 때만 검사한다: 저장 rejectMissingRequired는 unset이면
// 통과하고 set이면서 null이면 RequiredMissing(경로 [link, asset]) 뒤 InvalidArgument, non-null이면
// 통과한다. 복원 2c(진단 hook이 있을 때 적용 뒤 실효 값 검사)도 set·null이면 RequiredMissing 하나를
// 내고(로드는 성공) unset이면 내지 않는다.
TEST(OptionalGraph_RequiredInsideOptionalStructFollowsSetState) {
    SystemAllocator allocator;
    TypeRegistry types(allocator);
    REQUIRE(RegisterFixture(types));
    {
        ObjectRegistry objects(allocator, types, {0x4f5047, 23});
        const auto access = objects.OwnerAccess();
        RootRef<OptionalHolder> holder = New(objects);
        const auto strictSave = [&](uint32& missing) noexcept {
            Diagnostics diagnostics;
            ObjectGraphSaveOptions strict;
            strict.policy.rejectMissingRequired = true;
            strict.hooks = &diagnostics;
            auto saved = SaveObjectGraph(allocator, objects, access, holder.Handle(), strict);
            missing = diagnostics.requiredMissing;
            return saved ? ErrorCode::None : saved.Error();
        };

        const auto loadMissing = [&]() noexcept -> uint32 {
            auto saved = SaveObjectGraph(allocator, objects, access, holder.Handle());
            if (!saved) {
                CHECK(false);
                return NumericLimits<uint32>::Max();
            }

            Diagnostics diagnostics;
            ObjectGraphLoadOptions options;
            options.hooks = &diagnostics;
            auto loaded =
                LoadObjectGraph(allocator, objects, types, access, saved.Value().View(), options);
            CHECK(loaded);
            CHECK(diagnostics.requiredMissing == 0 || diagnostics.requiredPathSteps == 2);
            return diagnostics.requiredMissing;
        };

        uint32 missing = 0;
        CHECK(strictSave(missing) == ErrorCode::None && missing == 0);
        CHECK(loadMissing() == 0);
        With(objects, holder.Handle(), [&](OptionalHolder& value) noexcept {
            value.link.Emplace(objects.ObjectAllocator());
        });
        CHECK(strictSave(missing) == ErrorCode::InvalidArgument && missing == 1);
        CHECK(loadMissing() == 1);
        With(objects, holder.Handle(), [](OptionalHolder& value) noexcept {
            REQUIRE(value.link->asset.TryAssign(AssetId{Guid{0x4f50, 0x4c}}, {}));
        });
        CHECK(strictSave(missing) == ErrorCode::None && missing == 0);
        CHECK(loadMissing() == 0);
        holder.Reset();
        REQUIRE(objects.Collect(access));
        REQUIRE(objects.Shutdown(access));
    }

    CHECK(types.Shutdown());
}

// Optional 내부 종류의 나머지(AssetId·double·uint64·int64)도 set·unset이 왕복한다(경계 값 포함).
TEST(OptionalGraph_RemainingInnerKindsRoundTrip) {
    SystemAllocator allocator;
    TypeRegistry types(allocator);
    REQUIRE(RegisterFixture(types));
    {
        ObjectRegistry objects(allocator, types, {0x4f5047, 24});
        RootRef<OptionalHolder> holder = New(objects);
        With(objects, holder.Handle(), [](OptionalHolder& value) noexcept {
            value.asset.Emplace(AssetId{Guid{0x4f50, 0x41}});
            value.precise.Emplace(-0.25);
            value.big.Emplace(NumericLimits<uint64>::Max());
        });
        auto saved = SaveObjectGraph(allocator, objects, objects.OwnerAccess(), holder.Handle());
        REQUIRE(saved);
        auto loaded =
            LoadObjectGraph(allocator, objects, types, objects.OwnerAccess(), saved.Value().View());
        REQUIRE(loaded);
        With(objects, loaded.Value().Handle(), [](OptionalHolder& value) noexcept {
            CHECK(value.asset.HasValue() && value.asset->value == (Guid{0x4f50, 0x41}));
            CHECK(value.precise.HasValue() && *value.precise == -0.25);
            CHECK(value.big.HasValue() && *value.big == NumericLimits<uint64>::Max());
            CHECK(!value.wide.HasValue());
        });
        holder.Reset();
        loaded.Value().Reset();
        REQUIRE(objects.Shutdown(objects.OwnerAccess()));
    }

    CHECK(types.Shutdown());
}
