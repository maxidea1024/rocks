#include <MeshTools/StaticMeshCook.h>

#include <AssetCooker/CookBulkSource.h>
#include <AssetNugget/NuggetReflected.h>
#include <AssetNugget/NuggetScc.h>
#include <Base/NumericLimits.h>
#include <Core/Encoding/ByteReader.h>
#include <Core/Encoding/ByteWriter.h>
#include <Core/Text/NamePool.h>
#include <GraphicsTypes/IndexFormat.h>
#include <Mesh/MeshBuildSettings.generated.h>
#include <Mesh/StaticMesh/StaticMesh.generated.h>
#include <Meta/GeneratedType.h>
#include <Object/ObjectRegistry.h>
#include <Platform/Memory.h>

#include "MeshCookCommon.h"

// 정적 메시 쿠킹 처리기다(Docs/Design/MeshAnimationAssets 5.5.2절 — MA2b2).
namespace rocks {
namespace {

using mesh_cook::CookFail;
using mesh_cook::PendingBulkId;
using reflection_text::AssignmentTarget;

// 머리의 고정부(버전·LOD 수·bounds·경고 수)다(나머지는 MeshCookCommon.h 단계가 본다).
constexpr usize HeaderFixedBytes = 4 + 4 + mesh_cook::HeaderBoundsBytes + 4;
// 쿠킹 LOD 하나의 입력이다(원본 LOD 번호, baked를 쓰는지, 유효 빌드 설정).
struct LodInput {
    uint32 sourceLod = 0;
    bool baked = false;
    MeshBuildSettings build;
    float screenSize = 0.0f;
};

const MeshSourceData& InputData(const StaticMesh& mesh, const LodInput& input) noexcept {
    const StaticMeshSourceLod& lod = mesh.sourceLods[input.sourceLod];
    return input.baked ? lod.bakedSource : lod.source;
}

// 지역 키다(5.5.2절 10 — 머리, TypeId, 버전들, 색 형식·장치, 쿠킹 LOD마다 원본 정체성·유효 설정·
// 슬롯 이름·섹션 정보). mesh는 읽기 Borrow 안의 객체다.
Result<Sha256Digest> LocalKey(const CookContext& context, NamePool& names, const StaticMesh& mesh,
                              Span<const LodInput> inputs, Span<const StringView> slots,
                              MeshColorFormat color, bool device) noexcept {
    auto lease = context.registry->Types().AcquireValueType(GeneratedType<MeshBuildSettings>::Id(),
                                                            TypeKind::Struct);
    if (!lease) {
        return Result<Sha256Digest>::Failure(lease.Error());
    }

    NuggetKeyWriter writer;
    WriteNuggetKeyHeader(writer, StaticMeshNuggetProcessorId, context.nuggets->ProjectGeneration());
    writer.WriteGuid(TypeIdOf<StaticMesh>());
    writer.WriteU32(StaticMeshBuildVersion);
    writer.WriteU32(MeshMikkTSpacePatchVersion);
    writer.WriteU32(MeshBuildThresholdVersion);
    writer.WriteU32(static_cast<uint32>(color));
    writer.WriteBool(device);
    writer.WriteList(slots.Size());
    for (StringView slot : slots) {
        writer.WriteString(slot);
    }

    writer.WriteList(inputs.Size());
    for (const LodInput& input : inputs) {
        const MeshSourceData& data = InputData(mesh, input);
        writer.WriteBool(input.baked);
        if (auto written = mesh_cook::WriteSourceKey(writer, names, data); !written) {
            return Result<Sha256Digest>::Failure(written.Error());
        }

        MeshBuildSettings keyed = input.build;
        const AssignmentTarget target = AssignmentTarget::Value(
            *context.registry, context.access, lease.Value(), &keyed, *context.allocator);
        if (auto written = WriteReflected(writer, target); !written) {
            return Result<Sha256Digest>::Failure(written.Error());
        }

        mesh_cook::WriteSectionKey(writer, mesh.sectionInfo.View(), input.sourceLod,
                                   data.polygonGroupCount);
    }

    return writer.Finish();
}

// 쿠킹 LOD 하나의 결과다(빌드·패킹 또는 적중 값 — bytes는 data 덩어리).
struct CookedLod {
    explicit CookedLod(Allocator& allocator) noexcept : lod(allocator), bytes(allocator) {
    }

    StaticMeshRenderLod lod;
    Array<uint8> bytes;
};

// 쿠킹 결과 전체다.
struct CookedMesh {
    explicit CookedMesh(Allocator& allocator) noexcept
        : allocator(&allocator), lods(allocator), warnings(allocator) {
    }

    Allocator* allocator;
    Array<CookedLod> lods;
    Box<double> box{};
    double sphereRadius = 0.0;
    Array<MeshCookWarning> warnings;
};

// 메시 객체 renderData와 같은 모양의 값(머리 쓰기·검증용 — data는 크기만)을 만든다.
Result<void> ShapeOf(const CookedMesh& cooked, StaticMeshRenderData& out) noexcept {
    out.lods.Clear();
    if (auto reserved = out.lods.TryReserve(cooked.lods.Size()); !reserved) {
        return reserved;
    }

    for (const CookedLod& source : cooked.lods) {
        StaticMeshRenderLod lod(*cooked.allocator);
        lod.vertexCount = source.lod.vertexCount;
        lod.data.rawBytes = source.lod.data.rawBytes;
        lod.data.bulkId = source.lod.data.bulkId;
        lod.positions = source.lod.positions;
        lod.tangentBasis = source.lod.tangentBasis;
        lod.colors = source.lod.colors;
        lod.indexFormat = source.lod.indexFormat;
        lod.indices = source.lod.indices;
        if (auto copied = lod.texCoords.TryAppend(source.lod.texCoords.View()); !copied) {
            return copied;
        }

        if (auto copied = lod.sections.TryAppend(source.lod.sections.View()); !copied) {
            return copied;
        }

        (void)out.lods.TryPushBack(Move(lod));
    }

    out.box = cooked.box;
    out.sphereRadius = cooked.sphereRadius;
    return Result<void>::Success();
}

// 장치 없는 프로파일: stream·data를 비운다(섹션·정점 수·bounds는 남는다 — 5.5.2절 3).
void StripStreams(StaticMeshRenderLod& lod) noexcept {
    lod.data = BulkDataRef{};
    lod.positions = MeshRenderStream{};
    lod.tangentBasis = MeshRenderStream{};
    lod.colors = MeshRenderStream{};
    lod.indices = MeshRenderStream{};
    lod.indexFormat = 0;
    lod.texCoords.Clear();
}

// 적중 값을 받아들일 수 있으면 cooked에 채운다(5.5.2절 10 — 아니면 false). 오류는 할당 실패뿐이다.
Result<bool> AcceptHit(const NuggetCacheGetResult& got, usize lodCount, bool device,
                       Allocator& allocator, CookedMesh& cooked) noexcept {
    if (got.values.IsEmpty() || got.values[0].id != NuggetSccValueId(0, 0)) {
        return Result<bool>::Success(false);
    }

    StaticMeshRenderData shape(allocator);
    Array<MeshCookWarning> warnings(allocator);
    if (auto read = ReadStaticMeshNuggetHeader(got.values[0].bytes.View(), shape, warnings);
        !read) {
        return read.Error() == ErrorCode::OutOfMemory ? Result<bool>::Failure(read.Error())
                                                      : Result<bool>::Success(false);
    }

    const usize parts = device ? lodCount + 1 : 1;
    if (shape.lods.Size() != lodCount || got.values.Size() != parts) {
        return Result<bool>::Success(false);
    }

    for (usize index = 0; device && index < lodCount; ++index) {
        StaticMeshRenderLod& lod = shape.lods[index];
        const NuggetValue& value = got.values[index + 1];
        if (value.id != NuggetSccValueId(0, static_cast<uint32>(index + 1)) ||
            value.bytes.Size() != lod.data.rawBytes) {
            return Result<bool>::Success(false);
        }

        // 검증이 data를 "있음"으로 보게 임시 id를 둔다(채우기가 실제 ref로 바꾼다).
        lod.data.bulkId = PendingBulkId;
    }

    if (!ValidateStaticMeshRenderData(shape)) {
        return Result<bool>::Success(false);
    }

    for (usize index = 0; device && index < lodCount; ++index) {
        if (!ValidateStaticMeshIndices(shape.lods[index], static_cast<uint32>(index),
                                       got.values[index + 1].bytes.View())) {
            return Result<bool>::Success(false);
        }
    }

    cooked.lods.Clear();
    if (auto reserved = cooked.lods.TryReserve(lodCount); !reserved) {
        return Result<bool>::Failure(reserved.Error());
    }

    for (usize index = 0; index < lodCount; ++index) {
        CookedLod lod(allocator);
        lod.lod = Move(shape.lods[index]);
        if (device) {
            if (auto copied = lod.bytes.TryAppend(got.values[index + 1].bytes.View()); !copied) {
                return Result<bool>::Failure(copied.Error());
            }
        }

        (void)cooked.lods.TryPushBack(Move(lod));
    }

    cooked.box = shape.box;
    cooked.sphereRadius = shape.sphereRadius;
    cooked.warnings = Move(warnings);
    return Result<bool>::Success(true);
}

// 두 결과가 같은지다(verify — 머리 byte와 LOD byte).
bool SameCooked(Span<const uint8> leftHeader, const CookedMesh& left, Span<const uint8> rightHeader,
                const CookedMesh& right) noexcept {
    if (leftHeader.Size() != rightHeader.Size() || left.lods.Size() != right.lods.Size() ||
        !platform::MemoryEquals(leftHeader.Data(), rightHeader.Data(), leftHeader.Size())) {
        return false;
    }

    for (usize index = 0; index < left.lods.Size(); ++index) {
        const Span<const uint8> a = left.lods[index].bytes.View();
        const Span<const uint8> b = right.lods[index].bytes.View();
        if (a.Size() != b.Size() || !platform::MemoryEquals(a.Data(), b.Data(), a.Size())) {
            return false;
        }
    }

    return true;
}

// 에디터 객체의 원래 값이다(쓰기 뒤 되돌리기).
struct SavedFields {
    int32 minLod = 0;
    int32 numStreamedLods = 0;
    int32 lodForCollision = 0;
};

// 에디터 객체의 renderData·플랫폼 값을 되돌린다(할당하지 않는다). 에디터 패키지의 renderData는 늘
// 비어 있으므로(쿠킹 데이터 — 임포트가 비운다) 빈 값으로 되돌린다(텍스처 platformData와 같다).
Result<void> Restore(ObjectRegistry& registry, const ObjectAccessContext& access,
                     ObjectHandle handle, const SavedFields& saved) noexcept {
    auto borrowed = registry.ResolveForWrite<StaticMesh>(access, handle);
    if (!borrowed) {
        return Result<void>::Failure(borrowed.Error());
    }

    auto value = borrowed.Value().Get();
    if (!value) {
        return Result<void>::Failure(value.Error());
    }

    StaticMesh& mesh = *value.Value();
    StaticMeshRenderData empty(mesh.renderData.lods.GetAllocator());
    mesh.renderData = Move(empty);
    mesh.minLod.defaultValue = saved.minLod;
    mesh.numStreamedLods.defaultValue = saved.numStreamedLods;
    mesh.lodForCollision = saved.lodForCollision;
    return Result<void>::Success();
}

} // namespace

Result<void> WriteStaticMeshNuggetHeader(const StaticMeshRenderData& renderData,
                                         Span<const MeshCookWarning> warnings,
                                         Array<uint8>& out) noexcept {
    out.Clear();
    if (renderData.lods.Size() > StaticMeshMaxLods ||
        warnings.Size() > mesh_cook::HeaderMaxWarnings) {
        return Result<void>::Failure(ErrorCode::InvalidArgument);
    }

    ByteWriter writer(out);
    writer.WriteU32LE(StaticMeshNuggetHeaderVersion);
    writer.WriteU32LE(static_cast<uint32>(renderData.lods.Size()));
    mesh_cook::PutHeaderBounds(writer, renderData.box, renderData.sphereRadius);
    mesh_cook::PutHeaderWarnings(writer, warnings);
    for (const StaticMeshRenderLod& lod : renderData.lods) {
        if (!mesh_cook::FitsHeaderLod(lod)) {
            out.Clear();
            return Result<void>::Failure(ErrorCode::InvalidArgument);
        }

        mesh_cook::PutHeaderLodStreams(writer, lod);
        mesh_cook::PutHeaderLodIndices(writer, lod);
    }

    return writer.Status();
}

Result<void> ReadStaticMeshNuggetHeader(Span<const uint8> bytes, StaticMeshRenderData& out,
                                        Array<MeshCookWarning>& warnings) noexcept {
    out.lods.Clear();
    warnings.Clear();
    ByteReader reader(bytes);
    if (bytes.Size() < HeaderFixedBytes || reader.ReadU32LE() != StaticMeshNuggetHeaderVersion) {
        return Result<void>::Failure(ErrorCode::CorruptData);
    }

    const uint32 lodCount = reader.ReadU32LE();
    mesh_cook::ReadHeaderBounds(reader, out.box, out.sphereRadius);
    if (lodCount > StaticMeshMaxLods) {
        return Result<void>::Failure(ErrorCode::CorruptData);
    }

    if (auto read = mesh_cook::ReadHeaderWarnings(reader, lodCount, warnings); !read) {
        return read;
    }

    if (auto reserved = out.lods.TryReserve(lodCount); !reserved) {
        return reserved;
    }

    for (uint32 index = 0; index < lodCount; ++index) {
        StaticMeshRenderLod lod(out.lods.GetAllocator());
        if (auto read = mesh_cook::ReadHeaderLodStreams(reader, lod); !read) {
            return read;
        }

        if (auto read = mesh_cook::ReadHeaderLodIndices(reader, lod); !read) {
            return read;
        }

        (void)out.lods.TryPushBack(Move(lod));
    }

    return reader.Failed() || reader.Remaining() != 0
               ? Result<void>::Failure(ErrorCode::CorruptData)
               : Result<void>::Success();
}

MeshColorFormat StaticMeshColorFormat(const TargetProfile& profile) noexcept {
    for (const String& backend : profile.graphicsBackends) {
        if (backend.View() == StringView("OpenGL")) {
            return MeshColorFormat::UByte4Unorm;
        }
    }

    return MeshColorFormat::Bgra8Unorm;
}

bool StaticMeshProfileHasDevice(const TargetProfile& profile) noexcept {
    return !profile.graphicsBackends.IsEmpty();
}

void StaticMeshAutoScreenSizes(Span<float> out) noexcept {
    float power = 1.0f;
    for (usize index = 0; index < out.Size(); ++index) {
        if (index == 0) {
            out[index] = StaticMeshLod0ScreenSize;
            continue;
        }

        power *= StaticMeshAutoLodPowerBase;
        out[index] = power > out[index - 1] ? out[index - 1] / 2.0f : power;
    }
}

Result<void> StaticMeshCookProcessor::Cook(CookContext& context) const noexcept {
    Allocator& allocator = *context.allocator;
    ObjectRegistry& registry = *context.registry;
    const ObjectAccessContext& access = context.access;
    const AssetId assetId = context.entry->id;

    // 1. 대표 에셋과 타입.
    auto asset = registry.FindChild(access, context.package, "");
    if (!asset) {
        return CookFail(context, AssetDiagnosticCode::None, asset.Error());
    }

    auto type = registry.AcquireType(access, asset.Value());
    if (!type) {
        return CookFail(context, AssetDiagnosticCode::None, type.Error());
    }

    if (type.Value().Id() != TypeIdOf<StaticMesh>()) {
        return CookFail(context, AssetDiagnosticCode::TypeMismatch, ErrorCode::TypeMismatch);
    }

    NamePool* names = registry.Names();
    if (names == nullptr || context.bulkSource == nullptr) {
        return CookFail(context, AssetDiagnosticCode::None, ErrorCode::InvalidArgument);
    }

    auto found = names->TryFind(context.profile->id.View());
    const Name profileName = found ? found.Value() : Name{};
    const MeshColorFormat color = StaticMeshColorFormat(*context.profile);
    const bool device = StaticMeshProfileHasDevice(*context.profile);

    // 2. LOD 입력·키·조회·(미적중이면) 원본 로드 — 읽기 Borrow 안.
    Array<LodInput> inputs(allocator);
    Array<MeshCookWarning> selection(allocator);
    Array<NameLease> slotLeases(allocator);
    Array<StringView> slots(allocator);
    Array<MeshSource> sources(allocator);
    Array<Array<MeshBuildSectionInfo>> buildSections(allocator);
    Array<Array<MeshSectionInfo>> packSections(allocator);
    SavedFields saved;
    bool autoScreen = true;
    int32 minLod = 0;
    int32 numStreamed = 0;
    int32 lodForCollision = 0;
    NuggetCache* nuggets = context.nuggets;
    Sha256Digest sccKey{};
    NuggetCacheGetResult got(allocator);
    CookedMesh hitMesh(allocator);
    bool hit = false;
    {
        auto borrowed = registry.Resolve<StaticMesh>(access, asset.Value());
        if (!borrowed) {
            return CookFail(context, AssetDiagnosticCode::None, borrowed.Error());
        }

        auto value = borrowed.Value().Get();
        if (!value) {
            return CookFail(context, AssetDiagnosticCode::None, value.Error());
        }

        const StaticMesh& mesh = *value.Value();
        if (mesh.sourceLods.Size() > StaticMeshMaxLods) {
            return CookFail(context, AssetDiagnosticCode::InvalidSetting,
                            ErrorCode::InvalidArgument, mesh.sourceLods.Size());
        }

        saved.minLod = mesh.minLod.defaultValue;
        saved.numStreamedLods = mesh.numStreamedLods.defaultValue;
        saved.lodForCollision = mesh.lodForCollision;
        autoScreen = mesh.autoComputeLodScreenSize;
        minLod = mesh.minLod.Resolve(profileName);
        numStreamed = mesh.numStreamedLods.Resolve(profileName);
        lodForCollision = mesh.lodForCollision;
        for (uint32 index = 0; index < mesh.sourceLods.Size(); ++index) {
            const StaticMeshSourceLod& lod = mesh.sourceLods[index];
            LodInput input;
            input.sourceLod = index;
            input.build = lod.build;
            input.screenSize = lod.screenSize.Resolve(profileName);
            const bool generated = IsStaticMeshLodGenerated(lod);
            if (generated && lod.hasBakedSource) {
                // 교환 경로의 baked는 크기·법선·tangent가 이미 들어 있다(5.5.2절 5).
                input.baked = true;
                input.build.buildScale = Vec3d(1.0, 1.0, 1.0);
                input.build.recomputeNormals = false;
                input.build.recomputeTangents = false;
            } else if (!lod.hasSource || generated) {
                // 뺀 LOD·축소 없이 빌드한 LOD의 경고다(쿠킹 LOD 번호 — 뺀 LOD는 다음 쿠킹 LOD).
                const MeshCookWarning warning{AssetDiagnosticCode::UnsupportedSetting,
                                              static_cast<uint32>(inputs.Size())};
                if (!selection.TryPushBack(warning)) {
                    return CookFail(context, AssetDiagnosticCode::None, ErrorCode::OutOfMemory);
                }

                if (!lod.hasSource) {
                    continue;
                }
            }

            if (auto pushed = inputs.TryPushBack(input); !pushed) {
                return CookFail(context, AssetDiagnosticCode::None, pushed.Error());
            }
        }

        if (inputs.IsEmpty()) {
            return CookFail(context, AssetDiagnosticCode::InvalidSource,
                            ErrorCode::InvalidArgument);
        }

        // 마지막 LOD를 뺀 경고는 마지막 쿠킹 LOD에 둔다.
        for (MeshCookWarning& warning : selection) {
            if (warning.lod >= inputs.Size()) {
                warning.lod = static_cast<uint32>(inputs.Size() - 1);
            }
        }

        if (auto acquired =
                mesh_cook::AcquireSlotNames(*names, mesh.materialSlots, slotLeases, slots);
            !acquired) {
            return CookFail(context, AssetDiagnosticCode::None, acquired.Error());
        }

        // 섹션 정보(빌드 입력 — 표에 없으면 슬롯 대응, 패킹 플래그 — 표에 없으면 기본값).
        for (const LodInput& input : inputs) {
            Array<MeshBuildSectionInfo> build(allocator);
            Array<MeshSectionInfo> pack(allocator);
            if (auto chosen = mesh_cook::ChooseSections(mesh.sectionInfo.View(), input.sourceLod,
                                                        InputData(mesh, input).polygonGroupCount,
                                                        build, pack);
                !chosen) {
                return CookFail(context, AssetDiagnosticCode::None, chosen.Error());
            }

            if (!buildSections.TryPushBack(Move(build)) || !packSections.TryPushBack(Move(pack))) {
                return CookFail(context, AssetDiagnosticCode::None, ErrorCode::OutOfMemory);
            }
        }

        if (nuggets != nullptr) {
            auto localKey =
                LocalKey(context, *names, mesh, inputs.View(), slots.View(), color, device);
            if (!localKey) {
                return CookFail(context, AssetDiagnosticCode::None, localKey.Error());
            }

            const NuggetSccMember member{assetId, localKey.Value()};
            auto key = NuggetSccKey(allocator, Span<const NuggetSccMember>(&member, 1), {});
            if (!key) {
                return CookFail(context, AssetDiagnosticCode::None, key.Error());
            }

            sccKey = key.Value();
            lastKey_ = sccKey;
            got = nuggets->GetSccMember(NuggetBucketMesh, sccKey, assetId);
            if (got.failed && nuggets->IsRequired()) {
                return CookFail(context, AssetDiagnosticCode::None,
                                got.error != ErrorCode::None ? got.error : ErrorCode::IoError);
            }

            if (got.status == NuggetStatus::Hit) {
                auto accepted = AcceptHit(got, inputs.Size(), device, allocator, hitMesh);
                if (!accepted) {
                    return CookFail(context, AssetDiagnosticCode::None, accepted.Error());
                }

                hit = accepted.Value();
                if (!hit) {
                    auto rejected = nuggets->Reject(NuggetBucketMesh, sccKey, got.digest);
                    if (!rejected && nuggets->IsRequired()) {
                        return CookFail(context, AssetDiagnosticCode::None, rejected.Error());
                    }
                }
            }
        }

        // 미적중이거나 verify면 원본을 읽는다(적중이면 원본 bulk를 읽지 않는다).
        if (!hit || context.verifyNuggets) {
            if (auto reserved = sources.TryReserve(inputs.Size()); !reserved) {
                return CookFail(context, AssetDiagnosticCode::None, reserved.Error());
            }

            for (const LodInput& input : inputs) {
                MeshSource source(allocator);
                MeshDiagnostic failure;
                if (auto loaded =
                        source.Load(InputData(mesh, input), *context.bulkSource, *names, &failure);
                    !loaded) {
                    return CookFail(context, AssetDiagnosticCode::InvalidSource, loaded.Error(),
                                    input.sourceLod);
                }

                (void)sources.TryPushBack(Move(source));
            }
        }
    }

    // 3. 미적중이거나 verify면 빌드·패킹한다.
    CookedMesh builtMesh(allocator);
    Array<uint8> builtHeader(allocator);
    const bool rebuild = !hit || context.verifyNuggets;
    if (rebuild) {
        if (auto reserved = builtMesh.lods.TryReserve(inputs.Size()); !reserved) {
            return CookFail(context, AssetDiagnosticCode::None, reserved.Error());
        }

        for (usize index = 0; index < inputs.Size(); ++index) {
            StaticMeshBuildInput build;
            build.source = &sources[index];
            build.settings = inputs[index].build;
            build.slotNames = slots.View();
            build.sectionInfo = buildSections[index].View();
            build.scratch = &allocator;
            StaticMeshBuildResult built(allocator);
            MeshDiagnostic failure;
            ++builds_;
            if (auto done = BuildStaticMeshLod(build, built, &failure); !done) {
                return CookFail(context, failure.code, done.Error(), index);
            }

            for (const MeshDiagnostic& warning : built.warnings) {
                if (auto pushed = builtMesh.warnings.TryPushBack(
                        MeshCookWarning{warning.code, static_cast<uint32>(index)});
                    !pushed) {
                    return CookFail(context, AssetDiagnosticCode::None, pushed.Error());
                }
            }

            CookedLod lod(allocator);
            if (auto packed =
                    PackStaticMeshLod(built, inputs[index].build, packSections[index].View(), color,
                                      lod.bytes, lod.lod);
                !packed) {
                return CookFail(context, AssetDiagnosticCode::None, packed.Error());
            }

            lod.lod.data.rawBytes = lod.bytes.Size();
            lod.lod.data.bulkId = PendingBulkId;
            if (!device) {
                StripStreams(lod.lod);
                lod.bytes.Clear();
            }

            if (index == 0) {
                builtMesh.box = Box<double>(built.bounds.boxMin, built.bounds.boxMax);
                builtMesh.sphereRadius = built.bounds.sphereRadius;
            }

            (void)builtMesh.lods.TryPushBack(Move(lod));
        }

        StaticMeshRenderData shape(allocator);
        if (auto shaped = ShapeOf(builtMesh, shape); !shaped) {
            return CookFail(context, AssetDiagnosticCode::None, shaped.Error());
        }

        if (auto written =
                WriteStaticMeshNuggetHeader(shape, builtMesh.warnings.View(), builtHeader);
            !written) {
            return CookFail(context, AssetDiagnosticCode::None, written.Error());
        }

        if (hit &&
            !SameCooked(got.values[0].bytes.View(), hitMesh, builtHeader.View(), builtMesh)) {
            return CookFail(context, AssetDiagnosticCode::NuggetMismatch,
                            ErrorCode::InvalidArgument);
        }
    }

    CookedMesh& cooked = rebuild ? builtMesh : hitMesh;

    // 4. 경고(쿠킹 LOD 순서 — LOD마다 입력 선택 경고, 그다음 빌드 경고. 빌드 경고는 적중이면 머리의
    // 것이다).
    if (auto warned = mesh_cook::ReportWarnings(context, static_cast<uint32>(cooked.lods.Size()),
                                                selection.View(), cooked.warnings.View());
        !warned) {
        return warned;
    }

    // 5. 화면 크기(너겟 밖 — 5.5.2절 8).
    float screenSizes[StaticMeshMaxLods] = {};
    const Span<float> screens(screenSizes, cooked.lods.Size());
    if (autoScreen) {
        StaticMeshAutoScreenSizes(screens);
    } else {
        for (usize index = 0; index < cooked.lods.Size(); ++index) {
            screens[index] = inputs[index].screenSize;
        }
    }

    // 6. renderData·플랫폼 값을 채우고 쓰고, 성공·실패 모두 되돌린다.
    CookChainedBulkSource bulk(allocator, context.bulkSource);
    Result<void> filled = Result<void>::Success();
    {
        auto borrowed = registry.ResolveForWrite<StaticMesh>(access, asset.Value());
        if (!borrowed) {
            return CookFail(context, AssetDiagnosticCode::None, borrowed.Error());
        }

        auto value = borrowed.Value().Get();
        if (!value) {
            return CookFail(context, AssetDiagnosticCode::None, value.Error());
        }

        StaticMesh& mesh = *value.Value();
        mesh.minLod.defaultValue = mesh_cook::MapLod(inputs.View(), minLod);
        mesh.numStreamedLods.defaultValue = numStreamed;
        mesh.lodForCollision = mesh_cook::MapLod(inputs.View(), lodForCollision);
        StaticMeshRenderData& data = mesh.renderData;
        data.lods.Clear();
        data.box = cooked.box;
        data.sphereRadius = cooked.sphereRadius;
        filled = data.lods.TryReserve(cooked.lods.Size());
        for (usize index = 0; filled && index < cooked.lods.Size(); ++index) {
            CookedLod& source = cooked.lods[index];
            StaticMeshRenderLod lod = Move(source.lod);
            lod.screenSize = screens[index];
            if (device) {
                auto added = bulk.Add(source.bytes.View());
                if (!added) {
                    filled = Result<void>::Failure(added.Error());
                    break;
                }

                lod.data = added.Value();
            } else {
                lod.data = BulkDataRef{};
            }

            (void)data.lods.TryPushBack(Move(lod));
        }
    }

    Result<void> written = Result<void>::Success();
    if (filled) {
        CookContext write = context;
        write.bulkSource = &bulk;
        written = WriteCookedPackage(write, StaticMeshStreamingBulkAlignment);
    } else {
        written = CookFail(context, AssetDiagnosticCode::None, filled.Error());
    }

    Result<void> restored = Restore(registry, access, asset.Value(), saved);
    if (!written) {
        return written;
    }

    if (!restored) {
        return CookFail(context, AssetDiagnosticCode::None, restored.Error());
    }

    // 7. 미적중이면 Put한다(값 byte를 옮긴다 — 쓰기가 끝난 뒤). Put 실패는 required일 때만 오류다.
    if (nuggets == nullptr || hit) {
        return Result<void>::Success();
    }

    Array<NuggetValue> values(allocator);
    if (auto reserved = values.TryReserve(builtMesh.lods.Size() + 1); !reserved) {
        return CookFail(context, AssetDiagnosticCode::None, reserved.Error());
    }

    NuggetValue head(allocator);
    head.id = NuggetSccValueId(0, 0);
    head.bytes = Move(builtHeader);
    (void)values.TryPushBack(Move(head));
    for (usize index = 0; device && index < builtMesh.lods.Size(); ++index) {
        NuggetValue part(allocator);
        part.id = NuggetSccValueId(0, static_cast<uint32>(index + 1));
        part.bytes = Move(builtMesh.lods[index].bytes);
        (void)values.TryPushBack(Move(part));
    }

    return mesh_cook::PutNuggets(context, sccKey, assetId, values.View());
}

Result<void> RegisterStaticMeshCookProcessors(void* state, CookProcessorTable& table) noexcept {
    if (state == nullptr) {
        return Result<void>::Failure(ErrorCode::InvalidArgument);
    }

    return table.Register(TypeIdOf<StaticMesh>(),
                          *static_cast<const StaticMeshCookProcessor*>(state));
}

} // namespace rocks
