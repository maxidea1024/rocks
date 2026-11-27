#include <MeshTools/SkeletalMeshNuggetHeader.h>

#include <Animation/BoneHierarchy.h>
#include <Core/Encoding/ByteReader.h>
#include <Core/Encoding/ByteWriter.h>
#include <Mesh/SkeletalMesh/SkeletalMesh.h>

#include "MeshCookCommon.h"

// 스킨 메시 너겟 머리 쓰기·읽기다(Docs/Design/MeshAnimationAssets 6.7.2절 7). 정적 머리와 같은
// 항목은 MeshCookCommon.h의 단계다.
namespace rocks {
namespace {

using mesh_cook::HeaderStreamBytes;
using mesh_cook::PutHeaderStream;
using mesh_cook::ReadHeaderStream;

// 고정부(버전·LOD 수)다(bounds·경고는 공용 단계가 본다).
constexpr usize SkinHeaderFixedBytes = 4 + 4 + mesh_cook::HeaderBoundsBytes + 4;
// LOD 하나의 스킨 고정부(영향 칸 수·영향 stream 둘·bone map 수·필요 본 수·프로필 수·morph 수)다.
constexpr usize HeaderSkinBytes = 4 + HeaderStreamBytes * 2 + 4 + 4 + 4 + 4;
// 프로필·morph 항목의 고정부(이름 byte 수 + stream — 프로필은 stream 둘, morph는 항목 수 + 하나)다.
constexpr usize HeaderProfileBytes = 4 + HeaderStreamBytes * 2;
constexpr usize HeaderMorphBytes = 4 + 4 + HeaderStreamBytes;
// 머리가 담을 수 있는 프로필·morph target 수 상한이다(넘으면 손상으로 본다).
constexpr uint32 HeaderMaxNamedStreams = 65535;

Result<void> PutName(ByteWriter& writer, NamePool& names, Name name) noexcept {
    auto lease = names.Acquire(name);
    if (!lease) {
        return Result<void>::Failure(lease.Error());
    }

    const StringView text = lease.Value().View();
    writer.WriteU32LE(static_cast<uint32>(text.ByteCount()));
    writer.WriteBytes(
        Span<const uint8>(reinterpret_cast<const uint8*>(text.Data()), text.ByteCount()));
    return Result<void>::Success();
}

// 이름 하나를 읽어 intern한다(pool 오류는 할당 실패만 그대로, 나머지는 손상이다).
Result<Name> ReadName(ByteReader& reader, NamePool& names) noexcept {
    const uint32 size = reader.ReadU32LE();
    if (reader.Failed() || reader.Remaining() < size) {
        return Result<Name>::Failure(ErrorCode::CorruptData);
    }

    const Span<const uint8> bytes = reader.Take(size);
    auto name = names.TryIntern(StringView(reinterpret_cast<const char*>(bytes.Data()), size));
    if (!name) {
        // 이름 byte 위반(잘못된 UTF-8 등)은 손상이고 pool 상태 오류는 그대로다.
        return Result<Name>::Failure(
            name.Error() == ErrorCode::InvalidArgument ? ErrorCode::CorruptData : name.Error());
    }

    return name;
}

void PutBoneList(ByteWriter& writer, Span<const uint16> bones) noexcept {
    writer.WriteU32LE(static_cast<uint32>(bones.Size()));
    for (const uint16 bone : bones) {
        writer.WriteU16LE(bone);
    }
}

Result<void> ReadBoneList(ByteReader& reader, uint32 count, Array<uint16>& out) noexcept {
    if (count > MeshMaxBones || reader.Remaining() < usize{count} * sizeof(uint16)) {
        return Result<void>::Failure(ErrorCode::CorruptData);
    }

    if (auto resized = out.TryResize(count); !resized) {
        return resized;
    }

    for (uint16& bone : out) {
        bone = reader.ReadU16LE();
    }

    return Result<void>::Success();
}

} // namespace

Result<void> WriteSkeletalMeshNuggetHeader(const SkeletalMeshRenderData& renderData,
                                           Span<const MeshCookWarning> warnings, NamePool& names,
                                           Array<uint8>& out) noexcept {
    out.Clear();
    if (renderData.lods.Size() > SkeletalMeshMaxLods ||
        warnings.Size() > mesh_cook::HeaderMaxWarnings) {
        return Result<void>::Failure(ErrorCode::InvalidArgument);
    }

    ByteWriter writer(out);
    writer.WriteU32LE(SkeletalMeshNuggetHeaderVersion);
    writer.WriteU32LE(static_cast<uint32>(renderData.lods.Size()));
    mesh_cook::PutHeaderBounds(writer, renderData.box, renderData.sphereRadius);
    mesh_cook::PutHeaderWarnings(writer, warnings);
    for (const SkinnedRenderLod& lod : renderData.lods) {
        if (!mesh_cook::FitsHeaderLod(lod) || lod.boneMap.Size() > MeshMaxBones ||
            lod.requiredBones.Size() > MeshMaxBones ||
            lod.skinWeightProfiles.Size() > HeaderMaxNamedStreams ||
            lod.morphTargets.Size() > HeaderMaxNamedStreams) {
            out.Clear();
            return Result<void>::Failure(ErrorCode::InvalidArgument);
        }

        mesh_cook::PutHeaderLodStreams(writer, lod);
        writer.WriteU32LE(lod.influencesPerVertex);
        PutHeaderStream(writer, lod.boneIndices);
        PutHeaderStream(writer, lod.boneWeights);
        mesh_cook::PutHeaderLodIndices(writer, lod);
        PutBoneList(writer, lod.boneMap.View());
        PutBoneList(writer, lod.requiredBones.View());
        writer.WriteU32LE(static_cast<uint32>(lod.skinWeightProfiles.Size()));
        for (const SkinWeightProfileRenderLod& profile : lod.skinWeightProfiles) {
            if (auto put = PutName(writer, names, profile.name); !put) {
                out.Clear();
                return put;
            }

            PutHeaderStream(writer, profile.boneIndices);
            PutHeaderStream(writer, profile.boneWeights);
        }

        writer.WriteU32LE(static_cast<uint32>(lod.morphTargets.Size()));
        for (const SkinnedMorphTarget& target : lod.morphTargets) {
            if (auto put = PutName(writer, names, target.name); !put) {
                out.Clear();
                return put;
            }

            writer.WriteU32LE(target.deltaCount);
            PutHeaderStream(writer, target.deltas);
        }
    }

    if (auto status = writer.Status(); !status) {
        out.Clear();
        return status;
    }

    return Result<void>::Success();
}

Result<void> ReadSkeletalMeshNuggetHeader(Span<const uint8> bytes, NamePool& names,
                                          SkeletalMeshRenderData& out,
                                          Array<MeshCookWarning>& warnings) noexcept {
    out.lods.Clear();
    warnings.Clear();
    ByteReader reader(bytes);
    if (bytes.Size() < SkinHeaderFixedBytes ||
        reader.ReadU32LE() != SkeletalMeshNuggetHeaderVersion) {
        return Result<void>::Failure(ErrorCode::CorruptData);
    }

    const uint32 lodCount = reader.ReadU32LE();
    mesh_cook::ReadHeaderBounds(reader, out.box, out.sphereRadius);
    if (lodCount > SkeletalMeshMaxLods) {
        return Result<void>::Failure(ErrorCode::CorruptData);
    }

    if (auto read = mesh_cook::ReadHeaderWarnings(reader, lodCount, warnings); !read) {
        return read;
    }

    if (auto reserved = out.lods.TryReserve(lodCount); !reserved) {
        return reserved;
    }

    for (uint32 index = 0; index < lodCount; ++index) {
        SkinnedRenderLod lod(out.lods.GetAllocator());
        if (auto read = mesh_cook::ReadHeaderLodStreams(reader, lod); !read) {
            return read;
        }

        if (reader.Remaining() < HeaderSkinBytes) {
            return Result<void>::Failure(ErrorCode::CorruptData);
        }

        lod.influencesPerVertex = reader.ReadU32LE();
        ReadHeaderStream(reader, lod.boneIndices);
        ReadHeaderStream(reader, lod.boneWeights);
        if (auto read = mesh_cook::ReadHeaderLodIndices(reader, lod); !read) {
            return read;
        }

        if (auto read = ReadBoneList(reader, reader.ReadU32LE(), lod.boneMap); !read) {
            return read;
        }

        if (auto read = ReadBoneList(reader, reader.ReadU32LE(), lod.requiredBones); !read) {
            return read;
        }

        const uint32 profiles = reader.ReadU32LE();
        if (profiles > HeaderMaxNamedStreams ||
            reader.Remaining() < usize{profiles} * HeaderProfileBytes) {
            return Result<void>::Failure(ErrorCode::CorruptData);
        }

        if (auto reserved = lod.skinWeightProfiles.TryReserve(profiles); !reserved) {
            return reserved;
        }

        for (uint32 profile = 0; profile < profiles; ++profile) {
            SkinWeightProfileRenderLod value;
            auto name = ReadName(reader, names);
            if (!name) {
                return Result<void>::Failure(name.Error());
            }

            value.name = name.Value();
            ReadHeaderStream(reader, value.boneIndices);
            ReadHeaderStream(reader, value.boneWeights);
            (void)lod.skinWeightProfiles.TryPushBack(value);
        }

        const uint32 morphs = reader.ReadU32LE();
        if (morphs > HeaderMaxNamedStreams ||
            reader.Remaining() < usize{morphs} * HeaderMorphBytes) {
            return Result<void>::Failure(ErrorCode::CorruptData);
        }

        if (auto reserved = lod.morphTargets.TryReserve(morphs); !reserved) {
            return reserved;
        }

        for (uint32 morph = 0; morph < morphs; ++morph) {
            SkinnedMorphTarget value;
            auto name = ReadName(reader, names);
            if (!name) {
                return Result<void>::Failure(name.Error());
            }

            value.name = name.Value();
            value.deltaCount = reader.ReadU32LE();
            ReadHeaderStream(reader, value.deltas);
            (void)lod.morphTargets.TryPushBack(value);
        }

        if (reader.Failed()) {
            return Result<void>::Failure(ErrorCode::CorruptData);
        }

        (void)out.lods.TryPushBack(Move(lod));
    }

    return reader.Failed() || reader.Remaining() != 0
               ? Result<void>::Failure(ErrorCode::CorruptData)
               : Result<void>::Success();
}

} // namespace rocks
