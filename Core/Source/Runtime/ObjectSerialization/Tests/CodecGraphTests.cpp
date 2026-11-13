#include <ReflectionTestFixtures/ReflectionFixture.generated.h>
#include <Core/Memory/SystemAllocator.h>
#include <ObjectSerialization/ObjectGraph.h>
#include <Platform/Fatal.h>
#include <Serialization/Archive.h>
#include <Serialization/Record.h>
#include <TestSupport/Test.h>

// B3c 필드 codec 시험이다(Docs/Design/ReflectionFoundations.ko.md 6.1–6.4절): decode 정책(미선언
// enum의 대체 값, 모르는 필드, 높은 Record 버전, 누락 허용 플래그), 진단·소프트 참조 hook(한 번만,
// 실패 전파), Required의 저장 거부·복원 진단, 제외 플래그, 스키마와 정적 thunk의 어긋남 거부.
using namespace rocks;
using reflection_fixture::AssetFields;
using reflection_fixture::Base;
using reflection_fixture::Bias;
using reflection_fixture::Channels;
using reflection_fixture::EnumFields;
using reflection_fixture::Node;
using reflection_fixture::TextureGroup;
using reflection_fixture::WorldChild;
using reflection_fixture::WorldFixture;

namespace {

const FieldSchema& SchemaField(const TypeSchema& schema, StringView name) noexcept {
    for (const FieldSchema& field : schema.fields) {
        if (StringView(field.name.Data(), field.name.Size()) == name) {
            return field;
        }
    }

    platform::Fatal("fixture field missing");
}

constexpr AssetId IconAsset{Guid{0x1111222233334444ull, 0x5555666677778888ull}};
constexpr AssetId AnyAsset{Guid{0x9999aaaabbbbccccull, 0xddddeeeeffff0000ull}};
constexpr Guid UnknownFieldId{0x554e4b4e4f574eull, 9};

// 진단·소프트 참조를 기록한다. failReport가 None이 아니면 진단을 그 오류로 거부한다.
class RecordingHooks final : public FieldCodecHooks {
public:
    struct Diagnostic {
        FieldDiagnosticKind kind = FieldDiagnosticKind::UnknownField;
        TypeId objectType{};
        uint32 objectIndex = 0;
        Guid field{};
        usize pathSize = 0;
        Guid pathField{};
        TypeId valueType{};
        int64 value = 0;
    };

    struct Soft {
        AssetId asset{};
        char path[64] = {};
        usize pathBytes = 0;
        TypeId expectedType{};
        uint32 effectiveFlags = 0;
        Guid pathField{};
    };

    Result<void> Report(const FieldDiagnostic& diagnostic) noexcept override {
        if (failReport != ErrorCode::None) {
            return Result<void>::Failure(failReport);
        }

        if (diagnosticCount < 16) {
            Diagnostic& entry = diagnostics[diagnosticCount++];
            entry.kind = diagnostic.kind;
            entry.objectType = diagnostic.objectType;
            entry.objectIndex = diagnostic.objectIndex;
            entry.field = diagnostic.field;
            entry.pathSize = diagnostic.path.Size();
            entry.pathField = diagnostic.path.Size() != 0 ? diagnostic.path[0].fieldId : Guid{};
            entry.valueType = diagnostic.valueType;
            entry.value = diagnostic.value;
        }

        return Result<void>::Success();
    }

    Result<void> CollectSoftReference(const SoftReferenceInfo& info) noexcept override {
        if (failCollect != ErrorCode::None) {
            return Result<void>::Failure(failCollect);
        }

        if (softCount < 8) {
            Soft& entry = softs[softCount++];
            entry.asset = info.asset;
            entry.pathBytes = info.objectPath.ByteCount() < sizeof(entry.path)
                                  ? info.objectPath.ByteCount()
                                  : sizeof(entry.path);
            for (usize index = 0; index < entry.pathBytes; ++index) {
                entry.path[index] = info.objectPath.Data()[index];
            }

            entry.expectedType = info.expectedType;
            entry.effectiveFlags = info.effectiveFlags;
            entry.pathField = info.path.Size() != 0 ? info.path[0].fieldId : Guid{};
        }

        return Result<void>::Success();
    }

    usize Count(FieldDiagnosticKind kind) const noexcept {
        usize count = 0;
        for (usize index = 0; index < diagnosticCount; ++index) {
            count += diagnostics[index].kind == kind ? 1 : 0;
        }

        return count;
    }

    const Diagnostic* Find(FieldDiagnosticKind kind) const noexcept {
        for (usize index = 0; index < diagnosticCount; ++index) {
            if (diagnostics[index].kind == kind) {
                return &diagnostics[index];
            }
        }

        return nullptr;
    }

    Diagnostic diagnostics[16];
    usize diagnosticCount = 0;
    Soft softs[8];
    usize softCount = 0;
    ErrorCode failReport = ErrorCode::None;
    ErrorCode failCollect = ErrorCode::None;
};

// 레코드 하나짜리 graph format 1 바이트다.
Result<Array<uint8>> SingleGraph(Allocator& allocator, TypeId type, uint32 version,
                                 Span<const uint8> record) noexcept {
    const uint8 magic[] = {'R', 'O', 'C', 'K', 'S', 'G', 'R', 'F'};
    ArchiveWriter writer(allocator);
    Result<void> written = writer.WriteRaw(magic);
    const uint32 header[] = {1, 1, 1};
    for (uint32 value : header) {
        if (written) {
            written = writer.WriteU32(value);
        }
    }

    if (written) {
        written = writer.WriteU64(1);
    }

    if (written) {
        written = writer.WriteGuid(type);
    }

    if (written) {
        written = writer.WriteU32(version);
    }

    if (written) {
        written = writer.WriteBytes(record);
    }

    if (!written) {
        return Result<Array<uint8>>::Failure(written.Error());
    }

    return Result<Array<uint8>>::Success(writer.TakeBytes());
}

// EnumFields 레코드를 만든다(값마다 I64). extra면 모르는 필드 하나를 더한다.
Result<Array<uint8>> EnumRecord(Allocator& allocator, uint32 version, int64 group, int64 bias,
                                int64 channels, bool extra) noexcept {
    const TypeSchema& schema = TypeOf<EnumFields>();
    RecordBuilder record(allocator, TypeIdOf<EnumFields>(), version);
    const struct {
        StringView name;
        int64 value;
    } values[] = {{"group", group}, {"bias", bias}, {"channels", channels}};
    for (const auto& value : values) {
        ArchiveWriter payload(allocator);
        if (auto written = payload.WriteI64(value.value); !written) {
            return Result<Array<uint8>>::Failure(written.Error());
        }

        if (auto added =
                record.AddField(SchemaField(schema, value.name).id, WireType::I64, payload.View());
            !added) {
            return Result<Array<uint8>>::Failure(added.Error());
        }
    }

    if (extra) {
        ArchiveWriter payload(allocator);
        if (auto written = payload.WriteU32(7); !written) {
            return Result<Array<uint8>>::Failure(written.Error());
        }

        if (auto added = record.AddField(UnknownFieldId, WireType::U32, payload.View()); !added) {
            return Result<Array<uint8>>::Failure(added.Error());
        }
    }

    return record.Build();
}

bool RegisterEnumFixture(TypeRegistry& types) noexcept {
    return GeneratedType<TextureGroup>::Register(types).IsOk() &&
           GeneratedType<Bias>::Register(types).IsOk() &&
           GeneratedType<Channels>::Register(types).IsOk() &&
           GeneratedType<EnumFields>::Register(types).IsOk();
}

// 필드 하나(ID, wire, payload)짜리 Record를 만든다.
Result<Array<uint8>> OneFieldRecord(Allocator& allocator, TypeId type, Guid field, WireType wire,
                                    Span<const uint8> payload) noexcept {
    RecordBuilder record(allocator, type, 1);
    if (auto added = record.AddField(field, wire, payload); !added) {
        return Result<Array<uint8>>::Failure(added.Error());
    }

    return record.Build();
}

// 객체 둘짜리 graph format 1 바이트다(local ID 1과 2).
Result<Array<uint8>> TwoObjectGraph(Allocator& allocator, TypeId firstType,
                                    Span<const uint8> firstRecord, TypeId secondType,
                                    Span<const uint8> secondRecord) noexcept {
    const uint8 magic[] = {'R', 'O', 'C', 'K', 'S', 'G', 'R', 'F'};
    ArchiveWriter writer(allocator);
    Result<void> written = writer.WriteRaw(magic);
    const uint32 header[] = {1, 2, 1};
    for (uint32 value : header) {
        if (written) {
            written = writer.WriteU32(value);
        }
    }

    const struct {
        TypeId type;
        Span<const uint8> record;
    } objects[] = {{firstType, firstRecord}, {secondType, secondRecord}};
    for (usize index = 0; index < 2; ++index) {
        if (written) {
            written = writer.WriteU64(index + 1);
        }

        if (written) {
            written = writer.WriteGuid(objects[index].type);
        }

        if (written) {
            written = writer.WriteU32(1);
        }

        if (written) {
            written = writer.WriteBytes(objects[index].record);
        }
    }

    if (!written) {
        return Result<Array<uint8>>::Failure(written.Error());
    }

    return Result<Array<uint8>>::Success(writer.TakeBytes());
}

// 생성 함수표를 복사해 codec을 범용(FieldValue 경유) 구현으로 바꾼다(수동 함수표 경로).
template <class T, usize Count>
Result<TypeRegistrationToken> RegisterThroughFieldValue(TypeRegistry& types,
                                                        RuntimeFieldOps (&copied)[Count]) noexcept {
    RuntimeTypeOps ops = GeneratedType<T>::Ops();
    if (ops.fields.Size() != Count) {
        return Result<TypeRegistrationToken>::Failure(ErrorCode::InvalidArgument);
    }

    for (usize index = 0; index < Count; ++index) {
        copied[index] = ops.fields[index];
        copied[index].encode = &detail::EncodeThroughFieldValue;
        copied[index].decode = &detail::DecodeThroughFieldValue;
    }

    ops.fields = Span<const RuntimeFieldOps>(copied, Count);
    return types.Register(GeneratedType<T>::Schema(), Move(ops));
}

bool RegisterWorldFixture(TypeRegistry& types) noexcept {
    return GeneratedType<Base>::Register(types).IsOk() &&
           GeneratedType<Node>::Register(types).IsOk() &&
           GeneratedType<WorldFixture>::Register(types).IsOk() &&
           GeneratedType<WorldChild>::Register(types).IsOk();
}

} // namespace

TEST(Codec_UndeclaredEnumsReportAndReplace) {
    SystemAllocator allocator;
    TypeRegistry types(allocator);
    REQUIRE(RegisterEnumFixture(types));
    {
        ObjectRegistry target(allocator, types, {0x4344, 1});
        auto record = EnumRecord(allocator, 1, 5, 1, 16 | 1, false);
        REQUIRE(record);
        auto graph = SingleGraph(allocator, TypeIdOf<EnumFields>(), 1, record.Value().View());
        REQUIRE(graph);

        // 기본(Reject)은 거부, 진단 받을 곳이 없는 ReportAndDefault는 InvalidArgument다.
        ObjectGraphLoadOptions options;
        CHECK(LoadObjectGraph(allocator, target, types, target.OwnerAccess(), graph.Value().View(),
                              options)
                  .Error() == ErrorCode::InvalidArgument);
        options.policy.undeclaredEnum = UndeclaredEnumPolicy::ReportAndDefault;
        CHECK(LoadObjectGraph(allocator, target, types, target.OwnerAccess(), graph.Value().View(),
                              options)
                  .Error() == ErrorCode::InvalidArgument);

        // hook이 진단을 거부하면 복원이 실패한다.
        RecordingHooks failing;
        failing.failReport = ErrorCode::OutOfMemory;
        options.hooks = &failing;
        CHECK(LoadObjectGraph(allocator, target, types, target.OwnerAccess(), graph.Value().View(),
                              options)
                  .Error() == ErrorCode::OutOfMemory);
        CHECK(target.Stats().liveObjects == 0);

        RecordingHooks hooks;
        options.hooks = &hooks;
        {
            auto loaded = LoadObjectGraph(allocator, target, types, target.OwnerAccess(),
                                          graph.Value().View(), options);
            REQUIRE(loaded);
            auto borrowed =
                target.Resolve<EnumFields>(target.OwnerAccess(), loaded.Value().Handle());
            REQUIRE(borrowed);
            auto value = borrowed.Value().Get();
            REQUIRE(value);
            // 일반 enum은 선언된 0, Flags enum은 선언되지 않은 비트만 지운 값이다.
            CHECK(value.Value()->group == TextureGroup::World);
            CHECK(value.Value()->channels == Channels::Red);
            CHECK(value.Value()->bias == Bias::High);
        }

        // 진단은 1단계에서 한 번씩만 온다.
        REQUIRE(hooks.diagnosticCount == 2);
        CHECK(hooks.diagnostics[0].kind == FieldDiagnosticKind::UndeclaredEnumValue);
        CHECK(hooks.diagnostics[0].value == 5);
        CHECK(hooks.diagnostics[0].valueType == TypeIdOf<TextureGroup>());
        CHECK(hooks.diagnostics[0].objectType == TypeIdOf<EnumFields>());
        CHECK(hooks.diagnostics[0].objectIndex == 0);
        CHECK(hooks.diagnostics[0].pathSize == 1);
        CHECK(hooks.diagnostics[0].pathField == SchemaField(TypeOf<EnumFields>(), "group").id);
        CHECK(hooks.diagnostics[1].value == 17);
        CHECK(hooks.diagnostics[1].valueType == TypeIdOf<Channels>());
        REQUIRE(target.Shutdown(target.OwnerAccess()));
    }

    CHECK(types.Shutdown());
}

TEST(Codec_UnknownFieldsReportOrReject) {
    SystemAllocator allocator;
    TypeRegistry types(allocator);
    REQUIRE(RegisterEnumFixture(types));
    {
        ObjectRegistry target(allocator, types, {0x4344, 2});
        auto record = EnumRecord(allocator, 1, 7, 0, 4, true);
        REQUIRE(record);
        auto graph = SingleGraph(allocator, TypeIdOf<EnumFields>(), 1, record.Value().View());
        REQUIRE(graph);
        RecordingHooks hooks;
        ObjectGraphLoadOptions options;
        options.hooks = &hooks;
        {
            auto loaded = LoadObjectGraph(allocator, target, types, target.OwnerAccess(),
                                          graph.Value().View(), options);
            REQUIRE(loaded);
        }

        REQUIRE(hooks.diagnosticCount == 1);
        CHECK(hooks.diagnostics[0].kind == FieldDiagnosticKind::UnknownField);
        CHECK(hooks.diagnostics[0].field == UnknownFieldId);
        CHECK(hooks.diagnostics[0].pathSize == 0);

        // hook이 없어도 기본은 건너뛴다. Reject 정책이면 거부한다.
        options.hooks = nullptr;
        {
            auto loaded = LoadObjectGraph(allocator, target, types, target.OwnerAccess(),
                                          graph.Value().View(), options);
            REQUIRE(loaded);
        }

        options.policy.unknownField = UnknownFieldPolicy::Reject;
        CHECK(LoadObjectGraph(allocator, target, types, target.OwnerAccess(), graph.Value().View(),
                              options)
                  .Error() == ErrorCode::InvalidArgument);
        REQUIRE(target.Shutdown(target.OwnerAccess()));
    }

    CHECK(types.Shutdown());
}

// 저장 버전이 현재보다 높으면 migrate를 부르지 않는다. 기본은 Unsupported, 높은 버전 정책이면 아는
// 필드만 읽고 NewerVersion을 보고한다.
TEST(Codec_NewerRecordVersionPolicy) {
    SystemAllocator allocator;
    TypeRegistry types(allocator);
    REQUIRE(RegisterEnumFixture(types));
    {
        ObjectRegistry target(allocator, types, {0x4344, 3});
        auto record = EnumRecord(allocator, 2, 7, -2, 8, true);
        REQUIRE(record);
        auto graph = SingleGraph(allocator, TypeIdOf<EnumFields>(), 2, record.Value().View());
        REQUIRE(graph);
        ObjectGraphLoadOptions options;
        CHECK(LoadObjectGraph(allocator, target, types, target.OwnerAccess(), graph.Value().View(),
                              options)
                  .Error() == ErrorCode::Unsupported);
        options.policy.newerVersion = NewerVersionPolicy::LoadNewerSkippingUnknown;
        CHECK(LoadObjectGraph(allocator, target, types, target.OwnerAccess(), graph.Value().View(),
                              options)
                  .Error() == ErrorCode::InvalidArgument);
        RecordingHooks hooks;
        options.hooks = &hooks;
        {
            auto loaded = LoadObjectGraph(allocator, target, types, target.OwnerAccess(),
                                          graph.Value().View(), options);
            REQUIRE(loaded);
            auto borrowed =
                target.Resolve<EnumFields>(target.OwnerAccess(), loaded.Value().Handle());
            REQUIRE(borrowed);
            auto value = borrowed.Value().Get();
            REQUIRE(value);
            CHECK(value.Value()->group == TextureGroup::EightBitData);
            CHECK(value.Value()->bias == Bias::Low);
            CHECK(value.Value()->channels == Channels::Alpha);
        }

        const RecordingHooks::Diagnostic* newer = hooks.Find(FieldDiagnosticKind::NewerVersion);
        REQUIRE(newer);
        CHECK(newer->value == 2);
        CHECK(newer->pathSize == 0);
        CHECK(hooks.Count(FieldDiagnosticKind::UnknownField) == 1);
        REQUIRE(target.Shutdown(target.OwnerAccess()));
    }

    CHECK(types.Shutdown());
}

// Required 참조의 null은 복원에서 거부하지 않고 진단하며, rejectMissingRequired 저장은 거부한다.
TEST(Codec_RequiredReferencesReportOnLoadAndRejectOnPackageSave) {
    SystemAllocator allocator;
    TypeRegistry types(allocator);
    REQUIRE(RegisterWorldFixture(types));
    {
        ObjectRegistry source(allocator, types, {0x4344, 4});
        ObjectRegistry target(allocator, types, {0x4344, 5});
        {
            auto created =
                source.NewObject<WorldChild>(source.OwnerAccess(), TypeIdOf<WorldChild>());
            REQUIRE(created);
            const Guid targetField = SchemaField(TypeOf<WorldChild>(), "target").id;

            RecordingHooks saveHooks;
            ObjectGraphSaveOptions saveOptions;
            saveOptions.policy.rejectMissingRequired = true;
            saveOptions.hooks = &saveHooks;
            CHECK(SaveObjectGraph(allocator, source, source.OwnerAccess(), created.Value().Handle(),
                                  saveOptions)
                      .Error() == ErrorCode::InvalidArgument);
            REQUIRE(saveHooks.diagnosticCount == 1);
            CHECK(saveHooks.diagnostics[0].kind == FieldDiagnosticKind::RequiredMissing);
            CHECK(saveHooks.diagnostics[0].field == targetField);

            // 일반 저장은 받고, 복원은 RequiredMissing만 보고한다.
            auto saved =
                SaveObjectGraph(allocator, source, source.OwnerAccess(), created.Value().Handle());
            REQUIRE(saved);
            RecordingHooks loadHooks;
            ObjectGraphLoadOptions loadOptions;
            loadOptions.hooks = &loadHooks;
            {
                auto loaded = LoadObjectGraph(allocator, target, types, target.OwnerAccess(),
                                              saved.Value().View(), loadOptions);
                REQUIRE(loaded);
            }

            REQUIRE(loadHooks.diagnosticCount == 1);
            CHECK(loadHooks.diagnostics[0].kind == FieldDiagnosticKind::RequiredMissing);
            CHECK(loadHooks.diagnostics[0].field == targetField);

            // 대상이 있으면 패키지 저장도 받는다.
            auto node = source.NewObject<Node>(source.OwnerAccess(), TypeIdOf<Node>());
            REQUIRE(node);
            {
                auto borrowed =
                    source.Resolve<WorldChild>(source.OwnerAccess(), created.Value().Handle());
                REQUIRE(borrowed);
                REQUIRE(source.SetReference(source.OwnerAccess(), created.Value().Handle(),
                                            borrowed.Value().Get().Value()->target,
                                            node.Value().Handle()));
            }

            saveHooks.diagnosticCount = 0;
            REQUIRE(SaveObjectGraph(allocator, source, source.OwnerAccess(),
                                    created.Value().Handle(), saveOptions));
            CHECK(saveHooks.diagnosticCount == 0);
        }

        REQUIRE(source.Shutdown(source.OwnerAccess()));
        REQUIRE(target.Shutdown(target.OwnerAccess()));
    }

    CHECK(types.Shutdown());
}

// 제외 플래그(쿠킹의 EditorOnly)의 필드는 저장하지 않고, 누락 허용 플래그로만 그 누락을 받는다.
TEST(Codec_ExcludedFlagsAndAllowedMissingFlags) {
    SystemAllocator allocator;
    TypeRegistry types(allocator);
    REQUIRE(RegisterWorldFixture(types));
    {
        ObjectRegistry source(allocator, types, {0x4344, 6});
        ObjectRegistry target(allocator, types, {0x4344, 7});
        {
            auto created =
                source.NewObject<WorldChild>(source.OwnerAccess(), TypeIdOf<WorldChild>());
            REQUIRE(created);
            {
                auto borrowed =
                    source.Resolve<WorldChild>(source.OwnerAccess(), created.Value().Handle());
                REQUIRE(borrowed);
                borrowed.Value().Get().Value()->note = 41;
                borrowed.Value().Get().Value()->depth = 3;
            }

            ObjectGraphSaveOptions saveOptions;
            saveOptions.policy.excludedFlags = static_cast<uint32>(FieldFlags::EditorOnly);
            auto cooked = SaveObjectGraph(allocator, source, source.OwnerAccess(),
                                          created.Value().Handle(), saveOptions);
            REQUIRE(cooked);
            ObjectGraphLoadOptions loadOptions;
            CHECK(LoadObjectGraph(allocator, target, types, target.OwnerAccess(),
                                  cooked.Value().View(), loadOptions)
                      .Error() == ErrorCode::NotFound);
            CHECK(target.Stats().liveObjects == 0);
            loadOptions.policy.allowMissingFlags = static_cast<uint32>(FieldFlags::EditorOnly);
            {
                auto loaded = LoadObjectGraph(allocator, target, types, target.OwnerAccess(),
                                              cooked.Value().View(), loadOptions);
                REQUIRE(loaded);
                auto borrowed =
                    target.Resolve<WorldChild>(target.OwnerAccess(), loaded.Value().Handle());
                REQUIRE(borrowed);
                CHECK(borrowed.Value().Get().Value()->note == 0);
                CHECK(borrowed.Value().Get().Value()->depth == 3);
            }
        }

        REQUIRE(source.Shutdown(source.OwnerAccess()));
        REQUIRE(target.Shutdown(target.OwnerAccess()));
    }

    CHECK(types.Shutdown());
}

// non-null 소프트 참조는 저장과 복원에서 각각 한 번씩 수집한다(복원 2단계에서 다시 오지 않는다).
TEST(Codec_SoftReferencesCollectedOncePerDirection) {
    SystemAllocator allocator;
    TypeRegistry types(allocator);
    REQUIRE(GeneratedType<AssetFields>::Register(types));
    {
        ObjectRegistry source(allocator, types, {0x4344, 8});
        ObjectRegistry target(allocator, types, {0x4344, 9});
        {
            auto created =
                source.NewObject<AssetFields>(source.OwnerAccess(), TypeIdOf<AssetFields>());
            REQUIRE(created);
            {
                auto borrowed =
                    source.Resolve<AssetFields>(source.OwnerAccess(), created.Value().Handle());
                REQUIRE(borrowed);
                REQUIRE(borrowed.Value().Get().Value()->icon.TryAssign(IconAsset, "Body.Socket"));
                REQUIRE(borrowed.Value().Get().Value()->anyAsset.TryAssign(AnyAsset, ""));
            }

            RecordingHooks saveHooks;
            ObjectGraphSaveOptions saveOptions;
            saveOptions.hooks = &saveHooks;
            auto saved = SaveObjectGraph(allocator, source, source.OwnerAccess(),
                                         created.Value().Handle(), saveOptions);
            REQUIRE(saved);
            REQUIRE(saveHooks.softCount == 2);
            const Guid iconField = SchemaField(TypeOf<AssetFields>(), "icon").id;
            const RecordingHooks::Soft* icon = saveHooks.softs[0].pathField == iconField
                                                   ? &saveHooks.softs[0]
                                                   : &saveHooks.softs[1];
            CHECK(icon->asset == IconAsset);
            CHECK(StringView(icon->path, icon->pathBytes) == StringView("Body.Socket"));
            CHECK(icon->expectedType == TypeIdOf<Node>());
            CHECK(HasFlag(icon->effectiveFlags, FieldFlags::Required));

            RecordingHooks loadHooks;
            ObjectGraphLoadOptions loadOptions;
            loadOptions.hooks = &loadHooks;
            {
                auto loaded = LoadObjectGraph(allocator, target, types, target.OwnerAccess(),
                                              saved.Value().View(), loadOptions);
                REQUIRE(loaded);
            }

            CHECK(loadHooks.softCount == 2);
            CHECK(loadHooks.diagnosticCount == 0);
        }

        REQUIRE(source.Shutdown(source.OwnerAccess()));
        REQUIRE(target.Shutdown(target.OwnerAccess()));
    }

    CHECK(types.Shutdown());
}

// 생성 thunk의 멤버 형식과 다른 값 종류를 가진 스키마로 등록된 객체는 저장하지 않는다(같은 wire라도
// 조용히 오인코딩하지 않는다).
TEST(Codec_SchemaThunkMismatchIsRejected) {
    SystemAllocator allocator;
    TypeRegistry types(allocator);
    const TypeSchema& generated = GeneratedType<AssetFields>::Schema();
    FieldSchema fields[4];
    for (usize index = 0; index < 4; ++index) {
        fields[index] = generated.fields[index];
        if (StringView(fields[index].name.Data(), fields[index].name.Size()) ==
            StringView("source")) {
            fields[index].value.kind = ValueKind::Guid;
        }
    }

    TypeSchema schema = generated;
    schema.fields = Span<const FieldSchema>(fields, 4);
    auto registered = types.Register(schema, GeneratedType<AssetFields>::Ops());
    REQUIRE(registered);
    {
        ObjectRegistry objects(allocator, types, {0x4344, 10});
        {
            auto created =
                objects.NewObject<AssetFields>(objects.OwnerAccess(), TypeIdOf<AssetFields>());
            REQUIRE(created);
            CHECK(
                SaveObjectGraph(allocator, objects, objects.OwnerAccess(), created.Value().Handle())
                    .Error() == ErrorCode::TypeMismatch);
        }

        REQUIRE(objects.Shutdown(objects.OwnerAccess()));
    }

    REQUIRE(types.Unregister(registered.Value()));
    CHECK(types.Shutdown());
}

// 복원 쪽에서도 스키마 값 형식과 정적 thunk의 어긋남은 생성자 전에 TypeMismatch다.
TEST(Codec_DecodeRejectsSchemaThunkMismatchBeforeConstruction) {
    SystemAllocator allocator;
    Array<uint8> bytes(allocator);
    {
        TypeRegistry types(allocator);
        auto registered = GeneratedType<AssetFields>::Register(types);
        REQUIRE(registered);
        {
            ObjectRegistry source(allocator, types, {0x4344, 11});
            {
                auto created =
                    source.NewObject<AssetFields>(source.OwnerAccess(), TypeIdOf<AssetFields>());
                REQUIRE(created);
                auto saved = SaveObjectGraph(allocator, source, source.OwnerAccess(),
                                             created.Value().Handle());
                REQUIRE(saved);
                bytes = Move(saved.Value());
            }

            REQUIRE(source.Shutdown(source.OwnerAccess()));
        }

        REQUIRE(types.Unregister(registered.Value()));
        REQUIRE(types.Shutdown());
    }

    TypeRegistry altered(allocator);
    const TypeSchema& generated = GeneratedType<AssetFields>::Schema();
    FieldSchema fields[4];
    for (usize index = 0; index < 4; ++index) {
        fields[index] = generated.fields[index];
        if (StringView(fields[index].name.Data(), fields[index].name.Size()) ==
            StringView("source")) {
            fields[index].value.kind = ValueKind::Guid;
        }
    }

    TypeSchema schema = generated;
    schema.fields = Span<const FieldSchema>(fields, 4);
    auto registered = altered.Register(schema, GeneratedType<AssetFields>::Ops());
    REQUIRE(registered);
    {
        ObjectRegistry target(allocator, altered, {0x4344, 12});
        CHECK(LoadObjectGraph(allocator, target, altered, target.OwnerAccess(), bytes.View())
                  .Error() == ErrorCode::TypeMismatch);
        CHECK(target.Stats().liveObjects == 0);
        REQUIRE(target.Shutdown(target.OwnerAccess()));
    }

    REQUIRE(altered.Unregister(registered.Value()));
    CHECK(altered.Shutdown());
}

// 스키마 기대 타입이 Object여도 생성 thunk의 정적 대상 타입(ObjectRef<Node>)을 만족하지 않는 참조는
// 생성자 전에 거부한다.
TEST(Codec_StaticReferenceTargetIsCheckedBeforeConstruction) {
    SystemAllocator allocator;
    TypeRegistry types(allocator);
    REQUIRE(GeneratedType<Base>::Register(types));
    // Node와 같은 native의 별명 타입이지만 next의 기대 타입을 Object로 넓힌다.
    const TypeSchema& generated = GeneratedType<Node>::Schema();
    FieldSchema fields[16];
    REQUIRE(generated.fields.Size() <= 16);
    for (usize index = 0; index < generated.fields.Size(); ++index) {
        fields[index] = generated.fields[index];
        if (StringView(fields[index].name.Data(), fields[index].name.Size()) ==
            StringView("next")) {
            fields[index].value.type = TypeId{};
        }
    }

    TypeSchema schema = generated;
    schema.id = Guid{0x414c4941, 0x53};
    schema.fields = Span<const FieldSchema>(fields, generated.fields.Size());
    REQUIRE(types.Register(schema, GeneratedType<Node>::Ops()));
    {
        ObjectRegistry target(allocator, types, {0x4344, 13});
        ArchiveWriter payload(allocator);
        REQUIRE(payload.WriteU64(2));
        auto first = OneFieldRecord(allocator, schema.id, SchemaField(generated, "next").id,
                                    WireType::U64, payload.View());
        REQUIRE(first);
        RecordBuilder empty(allocator, TypeIdOf<Base>(), 1);
        auto second = empty.Build();
        REQUIRE(second);
        auto graph = TwoObjectGraph(allocator, schema.id, first.Value().View(), TypeIdOf<Base>(),
                                    second.Value().View());
        REQUIRE(graph);
        ObjectGraphLoadOptions options;
        options.policy.allowMissingFields = true;
        CHECK(LoadObjectGraph(allocator, target, types, target.OwnerAccess(), graph.Value().View(),
                              options)
                  .Error() == ErrorCode::TypeMismatch);
        CHECK(target.Stats().liveObjects == 0);
        REQUIRE(target.Shutdown(target.OwnerAccess()));
    }

    CHECK(types.Shutdown());
}

// 소프트 참조 수집 hook의 실패도 저장·복원을 실패시킨다.
TEST(Codec_SoftReferenceCollectorFailurePropagates) {
    SystemAllocator allocator;
    TypeRegistry types(allocator);
    REQUIRE(GeneratedType<AssetFields>::Register(types));
    {
        ObjectRegistry source(allocator, types, {0x4344, 14});
        ObjectRegistry target(allocator, types, {0x4344, 15});
        {
            auto created =
                source.NewObject<AssetFields>(source.OwnerAccess(), TypeIdOf<AssetFields>());
            REQUIRE(created);
            {
                auto borrowed =
                    source.Resolve<AssetFields>(source.OwnerAccess(), created.Value().Handle());
                REQUIRE(borrowed);
                REQUIRE(borrowed.Value().Get().Value()->icon.TryAssign(IconAsset, "Body.Socket"));
            }

            RecordingHooks failing;
            failing.failCollect = ErrorCode::OutOfMemory;
            ObjectGraphSaveOptions saveOptions;
            saveOptions.hooks = &failing;
            CHECK(SaveObjectGraph(allocator, source, source.OwnerAccess(), created.Value().Handle(),
                                  saveOptions)
                      .Error() == ErrorCode::OutOfMemory);
            auto saved =
                SaveObjectGraph(allocator, source, source.OwnerAccess(), created.Value().Handle());
            REQUIRE(saved);
            ObjectGraphLoadOptions loadOptions;
            loadOptions.hooks = &failing;
            CHECK(LoadObjectGraph(allocator, target, types, target.OwnerAccess(),
                                  saved.Value().View(), loadOptions)
                      .Error() == ErrorCode::OutOfMemory);
            CHECK(target.Stats().liveObjects == 0);
        }

        REQUIRE(source.Shutdown(source.OwnerAccess()));
        REQUIRE(target.Shutdown(target.OwnerAccess()));
    }

    CHECK(types.Shutdown());
}

// Required 소프트 참조: 패키지 저장은 null을 거부하고, 복원은 null·허용된 누락을 진단한다.
TEST(Codec_RequiredSoftReferencePolicies) {
    SystemAllocator allocator;
    TypeRegistry types(allocator);
    REQUIRE(GeneratedType<AssetFields>::Register(types));
    const Guid iconField = SchemaField(TypeOf<AssetFields>(), "icon").id;
    {
        ObjectRegistry source(allocator, types, {0x4344, 16});
        ObjectRegistry target(allocator, types, {0x4344, 17});
        {
            auto created =
                source.NewObject<AssetFields>(source.OwnerAccess(), TypeIdOf<AssetFields>());
            REQUIRE(created);
            RecordingHooks saveHooks;
            ObjectGraphSaveOptions saveOptions;
            saveOptions.policy.rejectMissingRequired = true;
            saveOptions.hooks = &saveHooks;
            CHECK(SaveObjectGraph(allocator, source, source.OwnerAccess(), created.Value().Handle(),
                                  saveOptions)
                      .Error() == ErrorCode::InvalidArgument);
            const RecordingHooks::Diagnostic* missing =
                saveHooks.Find(FieldDiagnosticKind::RequiredMissing);
            REQUIRE(missing);
            CHECK(missing->field == iconField);

            // 아이콘 필드가 없는 Record를 누락 허용으로 복원하면 RequiredMissing을 보고한다.
            ArchiveWriter payload(allocator);
            REQUIRE(payload.WriteGuid(Guid{1, 2}));
            auto record = OneFieldRecord(allocator, TypeIdOf<AssetFields>(),
                                         SchemaField(TypeOf<AssetFields>(), "source").id,
                                         WireType::Guid, payload.View());
            REQUIRE(record);
            auto graph = SingleGraph(allocator, TypeIdOf<AssetFields>(), 1, record.Value().View());
            REQUIRE(graph);
            RecordingHooks loadHooks;
            ObjectGraphLoadOptions loadOptions;
            loadOptions.policy.allowMissingFields = true;
            loadOptions.hooks = &loadHooks;
            {
                auto loaded = LoadObjectGraph(allocator, target, types, target.OwnerAccess(),
                                              graph.Value().View(), loadOptions);
                REQUIRE(loaded);
            }

            REQUIRE(loadHooks.Count(FieldDiagnosticKind::RequiredMissing) == 1);
            CHECK(loadHooks.Find(FieldDiagnosticKind::RequiredMissing)->field == iconField);
        }

        REQUIRE(source.Shutdown(source.OwnerAccess()));
        REQUIRE(target.Shutdown(target.OwnerAccess()));
    }

    CHECK(types.Shutdown());
}

// 수동 함수표용 범용 codec(read·writeStaged 경유)도 모든 단일 값 종류(정수 폭별·실수·문자열·Guid·
// 강한 참조·AssetId·digest·소프트 참조·enum)를 생성 codec과 같은 byte로 왕복한다.
TEST(Codec_ThroughFieldValueRoundTripsEveryValueKind) {
    SystemAllocator allocator;
    TypeRegistry generatedTypes(allocator);
    TypeRegistry manualTypes(allocator);
    REQUIRE(RegisterEnumFixture(generatedTypes));
    REQUIRE(GeneratedType<AssetFields>::Register(generatedTypes));
    REQUIRE(GeneratedType<Base>::Register(generatedTypes));
    REQUIRE(GeneratedType<Node>::Register(generatedTypes));
    REQUIRE(GeneratedType<TextureGroup>::Register(manualTypes));
    REQUIRE(GeneratedType<Bias>::Register(manualTypes));
    REQUIRE(GeneratedType<Channels>::Register(manualTypes));
    RuntimeFieldOps enumOps[3];
    RuntimeFieldOps assetOps[4];
    RuntimeFieldOps baseOps[2];
    // Node의 함수표는 상속한 Base 필드 둘을 포함한다.
    RuntimeFieldOps nodeOps[12];
    REQUIRE(RegisterThroughFieldValue<EnumFields>(manualTypes, enumOps));
    REQUIRE(RegisterThroughFieldValue<AssetFields>(manualTypes, assetOps));
    REQUIRE(RegisterThroughFieldValue<Base>(manualTypes, baseOps));
    REQUIRE(RegisterThroughFieldValue<Node>(manualTypes, nodeOps));
    {
        ObjectRegistry generated(allocator, generatedTypes, {0x4344, 18});
        ObjectRegistry manual(allocator, manualTypes, {0x4344, 19});
        {
            auto enums = manual.NewObject<EnumFields>(manual.OwnerAccess(), TypeIdOf<EnumFields>());
            auto assets =
                manual.NewObject<AssetFields>(manual.OwnerAccess(), TypeIdOf<AssetFields>());
            auto first = manual.NewObject<Node>(manual.OwnerAccess(), TypeIdOf<Node>());
            auto second = manual.NewObject<Node>(manual.OwnerAccess(), TypeIdOf<Node>());
            REQUIRE(enums && assets && first && second);
            {
                auto borrowed = manual.Resolve<Node>(manual.OwnerAccess(), first.Value().Handle());
                REQUIRE(borrowed);
                Node& node = *borrowed.Value().Get().Value();
                node.key = Guid{0x0102030405060708ull, 0x090a0b0c0d0e0f10ull};
                node.count = -2147483647 - 1;
                REQUIRE(node.label.TryAssign("가나다"));
                node.enabled = true;
                node.small = 0xff;
                node.medium = 0xffff;
                node.flags = 0xffffffffu;
                node.wide = 0xfedcba9876543210ull;
                node.signedWide = -1234567890123ll;
                node.weight = -0.5f;
                node.precision = 1.0 / 3.0;
            }

            // 강한 참조는 등록된 쓰기(레지스트리가 묶은 함수표)로 연결한다.
            auto nodeType = manualTypes.Acquire(TypeIdOf<Node>());
            REQUIRE(nodeType);
            const Guid nextId = SchemaField(TypeOf<Node>(), "next").id;
            FieldValue edge(allocator);
            edge.kind = ValueKind::ObjectReference;
            edge.reference = second.Value().Handle();
            bool linked = false;
            for (const RuntimeFieldOps& field : nodeType.Value().Ops().fields) {
                if (field.fieldId == nextId) {
                    REQUIRE(
                        field.write(manual, manual.OwnerAccess(), first.Value().Handle(), edge));
                    linked = true;
                }
            }

            REQUIRE(linked);

            {
                auto borrowed =
                    manual.Resolve<EnumFields>(manual.OwnerAccess(), enums.Value().Handle());
                REQUIRE(borrowed);
                borrowed.Value().Get().Value()->group = TextureGroup::Reserved13;
                borrowed.Value().Get().Value()->bias = Bias::Low;
                borrowed.Value().Get().Value()->channels = static_cast<Channels>(1 | 8);
            }

            {
                auto borrowed =
                    manual.Resolve<AssetFields>(manual.OwnerAccess(), assets.Value().Handle());
                REQUIRE(borrowed);
                borrowed.Value().Get().Value()->source = IconAsset;
                borrowed.Value().Get().Value()->hash.bytes[0] = 7;
                REQUIRE(borrowed.Value().Get().Value()->icon.TryAssign(IconAsset, "Body.Socket"));
            }

            // 범용 경로로 저장한 byte를 생성 경로로 복원해 다시 저장하면 같은 byte다.
            const ObjectHandle roots[] = {enums.Value().Handle(), assets.Value().Handle(),
                                          first.Value().Handle()};
            for (ObjectHandle root : roots) {
                auto saved = SaveObjectGraph(allocator, manual, manual.OwnerAccess(), root);
                REQUIRE(saved);
                auto loaded = LoadObjectGraph(allocator, generated, generatedTypes,
                                              generated.OwnerAccess(), saved.Value().View());
                REQUIRE(loaded);
                auto resaved = SaveObjectGraph(allocator, generated, generated.OwnerAccess(),
                                               loaded.Value().Handle());
                REQUIRE(resaved);
                bool same = resaved.Value().Size() == saved.Value().Size();
                for (usize index = 0; same && index < saved.Value().Size(); ++index) {
                    same = saved.Value()[index] == resaved.Value()[index];
                }

                CHECK(same);
                // 생성 경로로 저장한 byte를 범용 경로로 복원해도 된다.
                auto back = LoadObjectGraph(allocator, manual, manualTypes, manual.OwnerAccess(),
                                            resaved.Value().View());
                REQUIRE(back);
            }
        }

        REQUIRE(generated.Shutdown(generated.OwnerAccess()));
        REQUIRE(manual.Shutdown(manual.OwnerAccess()));
    }

    CHECK(generatedTypes.Shutdown());
    CHECK(manualTypes.Shutdown());
}

// 높은 버전 정책과 모르는 필드 Reject를 함께 주면 Reject가 우선한다.
TEST(Codec_UnknownFieldRejectWinsOverNewerVersionSkipping) {
    SystemAllocator allocator;
    TypeRegistry types(allocator);
    REQUIRE(RegisterEnumFixture(types));
    {
        ObjectRegistry target(allocator, types, {0x4344, 20});
        auto record = EnumRecord(allocator, 2, 7, 0, 4, true);
        REQUIRE(record);
        auto graph = SingleGraph(allocator, TypeIdOf<EnumFields>(), 2, record.Value().View());
        REQUIRE(graph);
        RecordingHooks hooks;
        ObjectGraphLoadOptions options;
        options.policy.newerVersion = NewerVersionPolicy::LoadNewerSkippingUnknown;
        options.policy.unknownField = UnknownFieldPolicy::Reject;
        options.hooks = &hooks;
        CHECK(LoadObjectGraph(allocator, target, types, target.OwnerAccess(), graph.Value().View(),
                              options)
                  .Error() == ErrorCode::InvalidArgument);
        CHECK(target.Stats().liveObjects == 0);
        REQUIRE(target.Shutdown(target.OwnerAccess()));
    }

    CHECK(types.Shutdown());
}

namespace {

// TextureGroup과 같은 ID·native로 등록하는 넓은 스키마다(값 0–199 — 정적 enum은 0·7·13뿐).
constexpr usize WideCount = 200;
char WideNames[WideCount][5];
EnumeratorSchema WideValues[WideCount];

EnumSchema WideTextureGroup() noexcept {
    for (usize index = 0; index < WideCount; ++index) {
        WideNames[index][0] = 'V';
        WideNames[index][1] = static_cast<char>('0' + index / 100);
        WideNames[index][2] = static_cast<char>('0' + index / 10 % 10);
        WideNames[index][3] = static_cast<char>('0' + index % 10);
        WideNames[index][4] = '\0';
        WideValues[index].name = Span<const char>(WideNames[index], 4);
        WideValues[index].value = static_cast<int64>(index);
    }

    return EnumSchema{GeneratedType<TextureGroup>::Id(),
                      Span<const char>("TextureGroup", 12),
                      ValueKind::UInt8,
                      0u,
                      Span<const EnumeratorSchema>(WideValues, WideCount),
                      {}};
}

bool RegisterWideEnumFixture(TypeRegistry& types) noexcept {
    return types.RegisterEnum(WideTextureGroup(), RuntimeEnumOps{NativeTypeKey<TextureGroup>(), {}})
               .IsOk() &&
           GeneratedType<Bias>::Register(types).IsOk() &&
           GeneratedType<Channels>::Register(types).IsOk() &&
           GeneratedType<EnumFields>::Register(types).IsOk();
}

// 복원이 성공하는 가장 작은 maxWork다. 그보다 작은 한도는 모두 CapacityExceeded이고 객체를 남기지
// 않아야 한다(아니면 0).
uint64 MinimalLoadWork(Allocator& allocator, ObjectRegistry& target, TypeRegistry& types,
                       Span<const uint8> bytes) noexcept {
    for (uint64 work = 0; work < 100000; ++work) {
        ObjectGraphLoadOptions options;
        options.limits.maxWork = work;
        auto loaded =
            LoadObjectGraph(allocator, target, types, target.OwnerAccess(), bytes, options);
        if (loaded) {
            loaded.Value().Reset();
            return target.Collect(target.OwnerAccess()) ? work : 0;
        }

        if (loaded.Error() != ErrorCode::CapacityExceeded || target.Stats().liveObjects != 0) {
            return 0;
        }
    }

    return 0;
}

// 저장이 성공하는 가장 작은 maxWork다(그보다 작은 한도는 모두 CapacityExceeded, 아니면 0).
uint64 MinimalSaveWork(Allocator& allocator, ObjectRegistry& source, ObjectHandle root) noexcept {
    for (uint64 work = 0; work < 100000; ++work) {
        ObjectGraphSaveOptions options;
        options.limits.maxWork = work;
        auto saved = SaveObjectGraph(allocator, source, source.OwnerAccess(), root, options);
        if (saved) {
            return work;
        }

        if (saved.Error() != ErrorCode::CapacityExceeded) {
            return 0;
        }
    }

    return 0;
}

} // namespace

// 등록 enum이 정적 enum보다 넓어도(같은 ID·native) 정적 enum에 없는 값은 객체를 만들기 전에
// 거부하고(2단계의 staged 쓰기가 실패하지 않게), 등록 enum의 열거자 수에 비례하는 검증 작업량을
// 저장·복원 예산에 청구한다(복원은 2단계 몫까지 두 배).
TEST(Codec_WideRegisteredEnumIsCheckedAndCharged) {
    SystemAllocator allocator;
    TypeRegistry narrowTypes(allocator);
    TypeRegistry wideTypes(allocator);
    REQUIRE(RegisterEnumFixture(narrowTypes));
    REQUIRE(RegisterWideEnumFixture(wideTypes));
    {
        ObjectRegistry narrow(allocator, narrowTypes, {0x4344, 21});
        ObjectRegistry wide(allocator, wideTypes, {0x4344, 22});

        // 넓은 스키마에만 있는 값(20)은 1단계에서 InvalidArgument다.
        auto record = EnumRecord(allocator, 1, 20, 0, 4, false);
        REQUIRE(record);
        auto graph = SingleGraph(allocator, TypeIdOf<EnumFields>(), 1, record.Value().View());
        REQUIRE(graph);
        CHECK(LoadObjectGraph(allocator, wide, wideTypes, wide.OwnerAccess(), graph.Value().View())
                  .Error() == ErrorCode::InvalidArgument);
        CHECK(wide.Stats().liveObjects == 0);

        // 선언된 값(7)이면 둘 다 복원되고, 넓은 쪽이 열거자 차이의 두 배 이상을 더 쓴다.
        auto valid = EnumRecord(allocator, 1, 7, 0, 4, false);
        REQUIRE(valid);
        auto validGraph = SingleGraph(allocator, TypeIdOf<EnumFields>(), 1, valid.Value().View());
        REQUIRE(validGraph);
        const uint64 narrowLoad =
            MinimalLoadWork(allocator, narrow, narrowTypes, validGraph.Value().View());
        const uint64 wideLoad =
            MinimalLoadWork(allocator, wide, wideTypes, validGraph.Value().View());
        REQUIRE(narrowLoad != 0);
        REQUIRE(wideLoad != 0);
        CHECK(wideLoad >= narrowLoad + 2 * (WideCount - 3));

        // 저장도 열거자 차이만큼 더 쓴다.
        {
            auto narrowRoot =
                narrow.NewObject<EnumFields>(narrow.OwnerAccess(), TypeIdOf<EnumFields>());
            auto wideRoot = wide.NewObject<EnumFields>(wide.OwnerAccess(), TypeIdOf<EnumFields>());
            REQUIRE(narrowRoot && wideRoot);
            const uint64 narrowSave =
                MinimalSaveWork(allocator, narrow, narrowRoot.Value().Handle());
            const uint64 wideSave = MinimalSaveWork(allocator, wide, wideRoot.Value().Handle());
            REQUIRE(narrowSave != 0);
            REQUIRE(wideSave != 0);
            CHECK(wideSave >= narrowSave + (WideCount - 3));
        }

        REQUIRE(narrow.Shutdown(narrow.OwnerAccess()));
        REQUIRE(wide.Shutdown(wide.OwnerAccess()));
    }

    CHECK(narrowTypes.Shutdown());
    CHECK(wideTypes.Shutdown());
}
