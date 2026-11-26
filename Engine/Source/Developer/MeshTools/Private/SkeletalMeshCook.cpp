#include <MeshTools/SkeletalMeshCook.h>

#include <AssetCooker/CookBulkSource.h>
#include <AssetNugget/NuggetReflected.h>
#include <AssetNugget/NuggetScc.h>
#include <Core/Encoding/ByteWriter.h>
#include <Core/Text/NamePool.h>
#include <Mesh/MeshBuildSettings.generated.h>
#include <Mesh/SkeletalMesh/SkeletalMesh.generated.h>
#include <Mesh/SkeletalMesh/SkeletalMeshValidation.h>
#include <MeshSource/MeshSource.h>
#include <MeshTools/StaticMeshBuilder.h>
#include <MeshTools/StaticMeshCook.h>
#include <Meta/GeneratedType.h>
#include <Object/ObjectRegistry.h>
#include <Platform/Memory.h>

#include "MeshCookCommon.h"

// 스킨 메시 쿠킹 처리기다(Docs/Design/MeshAnimationAssets 6.7.2절 — MA5b2). 흐름은 정적 처리기
// (StaticMeshCook.cpp)와 같고 같은 단계는 MeshCookCommon.h다.
namespace rocks {
// 유니티 빌드에서 정적 처리기(StaticMeshCook.cpp)의 같은 이름 도우미와 겹치지 않게 파일 전용
// namespace에 둔다.
namespace skeletal_mesh_cook {

using mesh_cook::CookFail;
using mesh_cook::CookWarn;
using mesh_cook::PendingBulkId;
using reflection_text::AssignmentTarget;

// 너겟 값 part 번호다(머리, inverse-bind, LOD 0부터의 덩어리).
constexpr uint32 PartHeader = 0;
constexpr uint32 PartInverseBind = 1;
constexpr uint32 PartFirstLod = 2;

// 쿠킹 LOD 하나의 입력이다(원본 LOD 번호, baked를 쓰는지, 유효 빌드 설정, 너겟 밖 값, 머티리얼 맵).
struct LodInput {
    explicit LodInput(Allocator& allocator) noexcept : materialMap(allocator) {
    }

    uint32 sourceLod = 0;
    bool baked = false;
    SkinnedMeshBuildSettings build;
    float screenSize = 0.0f;
    float lodHysteresis = 0.0f;
    Array<int32> materialMap;
};

const MeshSourceData& InputData(const SkeletalMesh& mesh, const LodInput& input) noexcept {
    const SkeletalMeshSourceLod& lod = mesh.sourceLods[input.sourceLod];
    return input.baked ? lod.bakedSource : lod.source;
}

// 빌드가 쓰는 본 계층 사본이다(이름·부모·기준 pose — 읽기 Borrow 밖에서 빌드한다).
Result<void> CopyBones(const BoneHierarchy& source, BoneHierarchy& out) noexcept {
    out.bones.Clear();
    out.referencePose.Clear();
    if (auto reserved = out.bones.TryReserve(source.bones.Size()); !reserved) {
        return reserved;
    }

    for (const BoneInfo& bone : source.bones) {
        BoneInfo copy(out.bones.GetAllocator());
        copy.name = bone.name;
        copy.parentIndex = bone.parentIndex;
        (void)out.bones.TryPushBack(Move(copy));
    }

    return out.referencePose.TryAppend(source.referencePose.View());
}

// 지역 키다(6.7.2절 7 — 정적 키 항목 + 스킨 빌드 버전, 메시 본, LOD마다 머티리얼 맵). 스켈레톤은
// 넣지 않는다(6.7.2절 3).
Result<Sha256Digest> LocalKey(const CookContext& context, NamePool& names, const SkeletalMesh& mesh,
                              Span<const LodInput> inputs, Span<const StringView> slots,
                              MeshColorFormat color, bool device) noexcept {
    auto lease = context.registry->Types().AcquireValueType(
        GeneratedType<SkinnedMeshBuildSettings>::Id(), TypeKind::Struct);
    if (!lease) {
        return Result<Sha256Digest>::Failure(lease.Error());
    }

    NuggetKeyWriter writer;
    WriteNuggetKeyHeader(writer, SkeletalMeshNuggetProcessorId,
                         context.nuggets->ProjectGeneration());
    writer.WriteGuid(TypeIdOf<SkeletalMesh>());
    // 정적 단계(빌드 5.2절·공용 패킹)를 함께 쓰므로 정적 버전도 넣는다.
    writer.WriteU32(StaticMeshBuildVersion);
    writer.WriteU32(SkinnedMeshBuildVersion);
    writer.WriteU32(MeshMikkTSpacePatchVersion);
    writer.WriteU32(MeshBuildThresholdVersion);
    writer.WriteU32(static_cast<uint32>(color));
    writer.WriteBool(device);
    writer.WriteList(slots.Size());
    for (StringView slot : slots) {
        writer.WriteString(slot);
    }

    // 메시 본(이름 원문·부모·기준 pose 비트).
    writer.WriteList(mesh.bones.bones.Size());
    for (usize index = 0; index < mesh.bones.bones.Size(); ++index) {
        const BoneInfo& bone = mesh.bones.bones[index];
        auto name = names.Acquire(bone.name);
        if (!name) {
            return Result<Sha256Digest>::Failure(name.Error());
        }

        writer.WriteString(name.Value().View());
        writer.WriteI64(bone.parentIndex);
        const Transform& pose = mesh.bones.referencePose[index];
        const double components[] = {pose.rotation.x,    pose.rotation.y,    pose.rotation.z,
                                     pose.rotation.w,    pose.translation.x, pose.translation.y,
                                     pose.translation.z, pose.scale.x,       pose.scale.y,
                                     pose.scale.z};
        for (const double component : components) {
            writer.WriteU64(platform::BitCast<uint64>(component));
        }
    }

    writer.WriteList(inputs.Size());
    for (const LodInput& input : inputs) {
        const MeshSourceData& data = InputData(mesh, input);
        writer.WriteBool(input.baked);
        if (auto written = mesh_cook::WriteSourceKey(writer, names, data); !written) {
            return Result<Sha256Digest>::Failure(written.Error());
        }

        SkinnedMeshBuildSettings keyed = input.build;
        const AssignmentTarget target = AssignmentTarget::Value(
            *context.registry, context.access, lease.Value(), &keyed, *context.allocator);
        if (auto written = WriteReflected(writer, target); !written) {
            return Result<Sha256Digest>::Failure(written.Error());
        }

        mesh_cook::WriteSectionKey(writer, mesh.sectionInfo.View(), input.sourceLod,
                                   data.polygonGroupCount);
        writer.WriteList(input.materialMap.Size());
        for (const int32 slot : input.materialMap) {
            writer.WriteI64(slot);
        }
    }

    return writer.Finish();
}

// 쿠킹 LOD 하나의 결과다(빌드·패킹 또는 적중 값 — bytes는 data 덩어리).
struct CookedLod {
    explicit CookedLod(Allocator& allocator) noexcept : lod(allocator), bytes(allocator) {
    }

    SkinnedRenderLod lod;
    Array<uint8> bytes;
};

// 쿠킹 결과 전체다(inverseBind는 본마다 float 16개 byte).
struct CookedMesh {
    explicit CookedMesh(Allocator& allocator) noexcept
        : allocator(&allocator), lods(allocator), warnings(allocator), inverseBind(allocator) {
    }

    Allocator* allocator;
    Array<CookedLod> lods;
    Box<double> box{};
    double sphereRadius = 0.0;
    Array<MeshCookWarning> warnings;
    Array<uint8> inverseBind;
};

// lod의 값을 out에 복사한다(배열 포함 — screenSize·lodHysteresis는 너겟 밖이라 복사하지 않는다).
Result<void> CopyLodShape(const SkinnedRenderLod& source, SkinnedRenderLod& out) noexcept {
    out.vertexCount = source.vertexCount;
    out.data.rawBytes = source.data.rawBytes;
    out.data.bulkId = source.data.bulkId;
    out.positions = source.positions;
    out.tangentBasis = source.tangentBasis;
    out.colors = source.colors;
    out.influencesPerVertex = source.influencesPerVertex;
    out.boneIndices = source.boneIndices;
    out.boneWeights = source.boneWeights;
    out.indexFormat = source.indexFormat;
    out.indices = source.indices;
    if (auto copied = out.texCoords.TryAppend(source.texCoords.View()); !copied) {
        return copied;
    }

    if (auto copied = out.sections.TryAppend(source.sections.View()); !copied) {
        return copied;
    }

    if (auto copied = out.boneMap.TryAppend(source.boneMap.View()); !copied) {
        return copied;
    }

    if (auto copied = out.requiredBones.TryAppend(source.requiredBones.View()); !copied) {
        return copied;
    }

    if (auto copied = out.morphTargets.TryAppend(source.morphTargets.View()); !copied) {
        return copied;
    }

    return out.skinWeightProfiles.TryAppend(source.skinWeightProfiles.View());
}

// 메시 객체 renderData와 같은 모양의 값(머리 쓰기용 — data는 크기만)을 만든다.
Result<void> ShapeOf(const CookedMesh& cooked, SkeletalMeshRenderData& out) noexcept {
    out.lods.Clear();
    if (auto reserved = out.lods.TryReserve(cooked.lods.Size()); !reserved) {
        return reserved;
    }

    for (const CookedLod& source : cooked.lods) {
        SkinnedRenderLod lod(*cooked.allocator);
        if (auto copied = CopyLodShape(source.lod, lod); !copied) {
            return copied;
        }

        (void)out.lods.TryPushBack(Move(lod));
    }

    out.box = cooked.box;
    out.sphereRadius = cooked.sphereRadius;
    return Result<void>::Success();
}

// 장치 없는 프로파일: stream·data를 비운다(섹션·정점 수·bounds·bone map·필요 본·이름은 남는다 —
// 6.7.1절 7).
void StripStreams(SkinnedRenderLod& lod) noexcept {
    lod.data = BulkDataRef{};
    lod.positions = MeshRenderStream{};
    lod.tangentBasis = MeshRenderStream{};
    lod.colors = MeshRenderStream{};
    lod.indices = MeshRenderStream{};
    lod.indexFormat = 0;
    lod.texCoords.Clear();
    lod.influencesPerVertex = 0;
    lod.boneIndices = MeshRenderStream{};
    lod.boneWeights = MeshRenderStream{};
    for (SkinWeightProfileRenderLod& profile : lod.skinWeightProfiles) {
        profile.boneIndices = MeshRenderStream{};
        profile.boneWeights = MeshRenderStream{};
    }

    for (SkinnedMorphTarget& target : lod.morphTargets) {
        target.deltaCount = 0;
        target.deltas = MeshRenderStream{};
    }
}

// inverse-bind 행렬을 float 16개(행 우선, little-endian)로 쓴다(6.7.1절 6).
Result<void> InverseBindBytes(Span<const Mat4d> matrices, Array<uint8>& out) noexcept {
    out.Clear();
    ByteWriter writer(out);
    for (const Mat4d& matrix : matrices) {
        for (const double value : matrix.m) {
            writer.WriteF32LE(static_cast<float>(value));
        }
    }

    return writer.Status();
}

// 원본 속성 이름에서 접두를 뗀 Name이다(프로필·morph 이름 — 6.7.2절 7).
Result<Name> SuffixName(NamePool& names, const MeshSource& source, uint32 attribute,
                        StringView prefix) noexcept {
    const StringView name = source.AttributeInfo(attribute).name;
    return names.TryIntern(
        StringView(name.Data() + prefix.ByteCount(), name.ByteCount() - prefix.ByteCount()));
}

Result<void> BuildNames(NamePool& names, const MeshSource& source,
                        const SkinnedMeshBuildResult& built, Array<Name>& profiles,
                        Array<Name>& morphs) noexcept {
    for (const SkinWeightProfileBuild& profile : built.profiles) {
        auto name = SuffixName(names, source, profile.sourceAttribute,
                               MeshAttributeName::SkinWeightsPrefix);
        if (!name) {
            return Result<void>::Failure(name.Error());
        }

        if (auto pushed = profiles.TryPushBack(name.Value()); !pushed) {
            return pushed;
        }
    }

    for (const MorphTargetBuild& target : built.morphTargets) {
        auto name = target.positionAttribute != MeshNoAttribute
                        ? SuffixName(names, source, target.positionAttribute,
                                     MeshAttributeName::MorphPrefix)
                        : SuffixName(names, source, target.normalAttribute,
                                     MeshAttributeName::MorphNormalPrefix);
        if (!name) {
            return Result<void>::Failure(name.Error());
        }

        if (auto pushed = morphs.TryPushBack(name.Value()); !pushed) {
            return pushed;
        }
    }

    return Result<void>::Success();
}

// 적중 값을 받아들일 수 있으면 cooked에 채운다(6.7.2절 7 — 아니면 false). 오류는 할당 실패뿐이다.
Result<bool> AcceptHit(const NuggetCacheGetResult& got, usize lodCount, usize boneCount,
                       bool device, NamePool& names, Allocator& allocator,
                       CookedMesh& cooked) noexcept {
    const usize parts = PartFirstLod + (device ? lodCount : 0);
    if (got.values.Size() != parts || got.values[0].id != NuggetSccValueId(0, PartHeader) ||
        got.values[1].id != NuggetSccValueId(0, PartInverseBind) ||
        got.values[1].bytes.Size() != boneCount * SkinnedInverseBindMatrixBytes) {
        return Result<bool>::Success(false);
    }

    SkeletalMeshRenderData shape(allocator);
    Array<MeshCookWarning> warnings(allocator);
    if (auto read =
            ReadSkeletalMeshNuggetHeader(got.values[0].bytes.View(), names, shape, warnings);
        !read) {
        return read.Error() == ErrorCode::OutOfMemory ? Result<bool>::Failure(read.Error())
                                                      : Result<bool>::Success(false);
    }

    if (shape.lods.Size() != lodCount) {
        return Result<bool>::Success(false);
    }

    for (usize index = 0; device && index < lodCount; ++index) {
        SkinnedRenderLod& lod = shape.lods[index];
        const NuggetValue& value = got.values[PartFirstLod + index];
        if (value.id != NuggetSccValueId(0, static_cast<uint32>(PartFirstLod + index)) ||
            value.bytes.Size() != lod.data.rawBytes) {
            return Result<bool>::Success(false);
        }

        // 검증이 data를 "있음"으로 보게 임시 id를 둔다(채우기가 실제 ref로 바꾼다).
        lod.data.bulkId = PendingBulkId;
    }

    if (!ValidateSkeletalMeshRenderData(shape, boneCount)) {
        return Result<bool>::Success(false);
    }

    for (usize index = 0; index < lodCount; ++index) {
        const Span<const uint8> bytes =
            device ? got.values[PartFirstLod + index].bytes.View() : Span<const uint8>{};
        if (!ValidateSkinnedRenderLodBulk(shape.lods[index], static_cast<uint32>(index), bytes)) {
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
            if (auto copied = lod.bytes.TryAppend(got.values[PartFirstLod + index].bytes.View());
                !copied) {
                return Result<bool>::Failure(copied.Error());
            }
        }

        (void)cooked.lods.TryPushBack(Move(lod));
    }

    if (auto copied = cooked.inverseBind.TryAppend(got.values[1].bytes.View()); !copied) {
        return Result<bool>::Failure(copied.Error());
    }

    cooked.box = shape.box;
    cooked.sphereRadius = shape.sphereRadius;
    cooked.warnings = Move(warnings);
    return Result<bool>::Success(true);
}

bool SameBytes(Span<const uint8> a, Span<const uint8> b) noexcept {
    return a.Size() == b.Size() && platform::MemoryEquals(a.Data(), b.Data(), a.Size());
}

// 두 결과가 같은지다(verify — 머리·inverse-bind·LOD byte).
bool SameCooked(Span<const uint8> leftHeader, const CookedMesh& left, Span<const uint8> rightHeader,
                const CookedMesh& right) noexcept {
    if (!SameBytes(leftHeader, rightHeader) || left.lods.Size() != right.lods.Size() ||
        !SameBytes(left.inverseBind.View(), right.inverseBind.View())) {
        return false;
    }

    for (usize index = 0; index < left.lods.Size(); ++index) {
        if (!SameBytes(left.lods[index].bytes.View(), right.lods[index].bytes.View())) {
            return false;
        }
    }

    return true;
}

// 에디터 객체의 renderData·플랫폼 값을 되돌린다(할당하지 않는다 — 에디터 패키지의 renderData는 늘
// 비어 있다).
Result<void> Restore(ObjectRegistry& registry, const ObjectAccessContext& access,
                     ObjectHandle handle, int32 savedMinLod) noexcept {
    auto borrowed = registry.ResolveForWrite<SkeletalMesh>(access, handle);
    if (!borrowed) {
        return Result<void>::Failure(borrowed.Error());
    }

    auto value = borrowed.Value().Get();
    if (!value) {
        return Result<void>::Failure(value.Error());
    }

    SkeletalMesh& mesh = *value.Value();
    SkeletalMeshRenderData empty(mesh.renderData.lods.GetAllocator());
    mesh.renderData = Move(empty);
    mesh.minLod.defaultValue = savedMinLod;
    return Result<void>::Success();
}

} // namespace skeletal_mesh_cook

Result<void> SkeletalMeshCookProcessor::Cook(CookContext& context) const noexcept {
    using namespace skeletal_mesh_cook;
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

    if (type.Value().Id() != TypeIdOf<SkeletalMesh>()) {
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

    // 2. 본 계층·LOD 입력·키·조회·(미적중이면) 원본 로드 — 읽기 Borrow 안.
    Array<LodInput> inputs(allocator);
    Array<MeshCookWarning> selection(allocator);
    Array<NameLease> slotLeases(allocator);
    Array<StringView> slots(allocator);
    Array<MeshSource> sources(allocator);
    Array<Array<MeshBuildSectionInfo>> buildSections(allocator);
    Array<Array<MeshSectionInfo>> packSections(allocator);
    BoneHierarchy bones(allocator);
    int32 savedMinLod = 0;
    int32 minLod = 0;
    NuggetCache* nuggets = context.nuggets;
    Sha256Digest sccKey{};
    NuggetCacheGetResult got(allocator);
    CookedMesh hitMesh(allocator);
    bool hit = false;
    {
        auto borrowed = registry.Resolve<SkeletalMesh>(access, asset.Value());
        if (!borrowed) {
            return CookFail(context, AssetDiagnosticCode::None, borrowed.Error());
        }

        auto value = borrowed.Value().Get();
        if (!value) {
            return CookFail(context, AssetDiagnosticCode::None, value.Error());
        }

        const SkeletalMesh& mesh = *value.Value();
        if (mesh.sourceLods.Size() > SkeletalMeshMaxLods) {
            return CookFail(context, AssetDiagnosticCode::InvalidSetting,
                            ErrorCode::InvalidArgument, mesh.sourceLods.Size());
        }

        SkeletonDiagnostic skeleton;
        if (auto valid = ValidateBoneHierarchy(mesh.bones, allocator, &skeleton); !valid) {
            return CookFail(context,
                            valid.Error() == ErrorCode::OutOfMemory
                                ? AssetDiagnosticCode::None
                                : AssetDiagnosticCode::InvalidSkeletonData,
                            valid.Error(), skeleton.index);
        }

        if (auto copied = CopyBones(mesh.bones, bones); !copied) {
            return CookFail(context, AssetDiagnosticCode::None, copied.Error());
        }

        savedMinLod = mesh.minLod.defaultValue;
        minLod = mesh.minLod.Resolve(profileName);
        for (uint32 index = 0; index < mesh.sourceLods.Size(); ++index) {
            const SkeletalMeshSourceLod& lod = mesh.sourceLods[index];
            LodInput input(allocator);
            input.sourceLod = index;
            input.build = lod.build;
            input.screenSize = lod.screenSize.Resolve(profileName);
            input.lodHysteresis = lod.lodHysteresis;
            if (auto copied = input.materialMap.TryAppend(lod.lodMaterialMap.View()); !copied) {
                return CookFail(context, AssetDiagnosticCode::None, copied.Error());
            }

            const bool generated = IsSkeletalMeshLodGenerated(lod, static_cast<int32>(index));
            if (generated && lod.hasBakedSource) {
                // 교환 경로의 baked는 법선·tangent가 이미 들어 있다(5.5.2절 5와 같다).
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

            if (auto pushed = inputs.TryPushBack(Move(input)); !pushed) {
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
                auto accepted = AcceptHit(got, inputs.Size(), bones.bones.Size(), device, *names,
                                          allocator, hitMesh);
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
            SkinnedMeshBuildInput build;
            build.source = &sources[index];
            build.bones = &bones;
            build.settings = inputs[index].build;
            build.slotNames = slots.View();
            build.sectionInfo = buildSections[index].View();
            build.scratch = &allocator;
            SkinnedMeshBuildResult built(allocator);
            MeshDiagnostic failure;
            ++builds_;
            if (auto done = BuildSkinnedMeshLod(build, built, &failure); !done) {
                return CookFail(context, failure.code, done.Error(), index);
            }

            for (const MeshDiagnostic& warning : built.mesh.warnings) {
                if (auto pushed = builtMesh.warnings.TryPushBack(
                        MeshCookWarning{warning.code, static_cast<uint32>(index)});
                    !pushed) {
                    return CookFail(context, AssetDiagnosticCode::None, pushed.Error());
                }
            }

            Array<Name> profileNames(allocator);
            Array<Name> morphNames(allocator);
            if (auto named = BuildNames(*names, sources[index], built, profileNames, morphNames);
                !named) {
                return CookFail(context, AssetDiagnosticCode::None, named.Error());
            }

            CookedLod lod(allocator);
            if (auto packed = PackSkinnedMeshLod(
                    built, inputs[index].build, packSections[index].View(),
                    inputs[index].materialMap.View(), static_cast<uint32>(slots.Size()), color,
                    profileNames.View(), morphNames.View(), lod.bytes, lod.lod);
                !packed) {
                return CookFail(context, AssetDiagnosticCode::None, packed.Error(), index);
            }

            lod.lod.data.rawBytes = lod.bytes.Size();
            lod.lod.data.bulkId = PendingBulkId;
            if (!device) {
                StripStreams(lod.lod);
                lod.bytes.Clear();
            }

            if (index == 0) {
                builtMesh.box = Box<double>(built.mesh.bounds.boxMin, built.mesh.bounds.boxMax);
                builtMesh.sphereRadius = built.mesh.bounds.sphereRadius;
                // inverse-bind는 본 계층만의 함수라 LOD마다 같다(6.5절 12).
                if (auto bound =
                        InverseBindBytes(built.inverseBindMatrices.View(), builtMesh.inverseBind);
                    !bound) {
                    return CookFail(context, AssetDiagnosticCode::None, bound.Error());
                }
            }

            (void)builtMesh.lods.TryPushBack(Move(lod));
        }

        SkeletalMeshRenderData shape(allocator);
        if (auto shaped = ShapeOf(builtMesh, shape); !shaped) {
            return CookFail(context, AssetDiagnosticCode::None, shaped.Error());
        }

        if (auto written = WriteSkeletalMeshNuggetHeader(shape, builtMesh.warnings.View(), *names,
                                                         builtHeader);
            !written) {
            // 머리 상한(프로필·morph·본 목록 수) 위반은 설정 문제다(할당 실패는 진단 없음).
            return CookFail(context,
                            written.Error() == ErrorCode::InvalidArgument
                                ? AssetDiagnosticCode::InvalidSetting
                                : AssetDiagnosticCode::None,
                            written.Error());
        }

        if (hit &&
            !SameCooked(got.values[0].bytes.View(), hitMesh, builtHeader.View(), builtMesh)) {
            return CookFail(context, AssetDiagnosticCode::NuggetMismatch,
                            ErrorCode::InvalidArgument);
        }
    }

    CookedMesh& cooked = rebuild ? builtMesh : hitMesh;

    // 4. 경고(쿠킹 LOD 순서 — LOD마다 입력 선택 경고, 그다음 빌드 경고).
    if (auto warned = mesh_cook::ReportWarnings(context, static_cast<uint32>(cooked.lods.Size()),
                                                selection.View(), cooked.warnings.View());
        !warned) {
        return warned;
    }

    // 5. renderData·inverse-bind·플랫폼 값을 채우고 쓰고, 성공·실패 모두 되돌린다. 화면 크기는 LOD
    // 정보 값이다(자동 계산 없음 — 6.7.2절 2).
    CookChainedBulkSource bulk(allocator, context.bulkSource);
    Result<void> filled = Result<void>::Success();
    {
        auto borrowed = registry.ResolveForWrite<SkeletalMesh>(access, asset.Value());
        if (!borrowed) {
            return CookFail(context, AssetDiagnosticCode::None, borrowed.Error());
        }

        auto value = borrowed.Value().Get();
        if (!value) {
            return CookFail(context, AssetDiagnosticCode::None, value.Error());
        }

        SkeletalMesh& mesh = *value.Value();
        mesh.minLod.defaultValue = mesh_cook::MapLod(inputs.View(), minLod);
        SkeletalMeshRenderData& data = mesh.renderData;
        data.lods.Clear();
        data.box = cooked.box;
        data.sphereRadius = cooked.sphereRadius;
        filled = data.lods.TryReserve(cooked.lods.Size());
        for (usize index = 0; filled && index < cooked.lods.Size(); ++index) {
            CookedLod& source = cooked.lods[index];
            SkinnedRenderLod lod = Move(source.lod);
            lod.screenSize = inputs[index].screenSize;
            lod.lodHysteresis = inputs[index].lodHysteresis;
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

        if (filled) {
            auto added = bulk.Add(cooked.inverseBind.View());
            if (added) {
                data.inverseBindMatrices = added.Value();
            } else {
                filled = Result<void>::Failure(added.Error());
            }
        }
    }

    Result<void> written = Result<void>::Success();
    if (filled) {
        CookContext write = context;
        write.bulkSource = &bulk;
        written = WriteCookedPackage(write, SkeletalMeshStreamingBulkAlignment);
    } else {
        written = CookFail(context, AssetDiagnosticCode::None, filled.Error());
    }

    Result<void> restored = Restore(registry, access, asset.Value(), savedMinLod);
    if (!written) {
        return written;
    }

    if (!restored) {
        return CookFail(context, AssetDiagnosticCode::None, restored.Error());
    }

    // 6. 미적중이면 Put한다(값 byte를 옮긴다 — 쓰기가 끝난 뒤). Put 실패는 required일 때만 오류다.
    if (nuggets == nullptr || hit) {
        return Result<void>::Success();
    }

    Array<NuggetValue> values(allocator);
    if (auto reserved = values.TryReserve(PartFirstLod + builtMesh.lods.Size()); !reserved) {
        return CookFail(context, AssetDiagnosticCode::None, reserved.Error());
    }

    NuggetValue head(allocator);
    head.id = NuggetSccValueId(0, PartHeader);
    head.bytes = Move(builtHeader);
    (void)values.TryPushBack(Move(head));
    NuggetValue binds(allocator);
    binds.id = NuggetSccValueId(0, PartInverseBind);
    binds.bytes = Move(builtMesh.inverseBind);
    (void)values.TryPushBack(Move(binds));
    for (usize index = 0; device && index < builtMesh.lods.Size(); ++index) {
        NuggetValue part(allocator);
        part.id = NuggetSccValueId(0, static_cast<uint32>(PartFirstLod + index));
        part.bytes = Move(builtMesh.lods[index].bytes);
        (void)values.TryPushBack(Move(part));
    }

    return mesh_cook::PutNuggets(context, sccKey, assetId, values.View());
}

Result<void> RegisterSkeletalMeshCookProcessors(void* state, CookProcessorTable& table) noexcept {
    if (state == nullptr) {
        return Result<void>::Failure(ErrorCode::InvalidArgument);
    }

    return table.Register(TypeIdOf<SkeletalMesh>(),
                          *static_cast<const SkeletalMeshCookProcessor*>(state));
}

} // namespace rocks
