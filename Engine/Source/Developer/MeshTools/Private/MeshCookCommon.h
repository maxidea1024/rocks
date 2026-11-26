#pragma once

#include <AssetCooker/CookProcessor.h>
#include <AssetNugget/NuggetCache.h>
#include <AssetNugget/NuggetKey.h>
#include <Base/NumericLimits.h>
#include <Core/Containers/Array.h>
#include <Core/Containers/Span.h>
#include <Core/Encoding/ByteReader.h>
#include <Core/Encoding/ByteWriter.h>
#include <Core/Math/Shapes.h>
#include <Core/Text/NamePool.h>
#include <Mesh/Common/MeshMaterialSlot.h>
#include <Mesh/Common/MeshRenderSection.h>
#include <Mesh/Common/MeshRenderStream.h>
#include <Mesh/Common/MeshSectionInfoEntry.h>
#include <Mesh/StaticMesh/StaticMeshValidation.h>
#include <MeshSource/MeshSourceData.h>
#include <MeshSource/MeshSourceTypes.h>
#include <MeshTools/MeshCookWarning.h>
#include <MeshTools/StaticMeshBuilder.h>
#include <Object/StructRefArray.h>
#include <Platform/Memory.h>

// 정적·스킨 메시 쿠킹 처리기가 함께 쓰는 도우미다(Docs/Design/MeshAnimationAssets 5.5.2절·6.7.2절
// 7). 이름 있는 namespace라 유니티 빌드에서 파일마다 겹치지 않는다.
namespace rocks::mesh_cook {

// 머리 stream 항목 byte(형식 번호·시작·크기)다.
inline constexpr usize HeaderStreamBytes = 4 + 8 + 8;
// 머리 bounds(상자 6 double·반지름)·경고 항목·섹션 항목 byte다.
inline constexpr usize HeaderBoundsBytes = 8 * 7;
inline constexpr usize HeaderWarningBytes = 4 + 4;
inline constexpr usize HeaderSectionBytes = 4 * 5 + 5;
// LOD 하나의 앞 고정부(정점 수·data byte·위치·tangent 기저·UV 수·색)와 뒤 고정부(index 형식·
// index·섹션 수)다.
inline constexpr usize HeaderLodStreamBytes = 4 + 8 + HeaderStreamBytes * 2 + 4 + HeaderStreamBytes;
inline constexpr usize HeaderLodIndexBytes = 4 + HeaderStreamBytes + 4;
// 머리가 담을 수 있는 경고·섹션 수 상한이다(넘으면 손상으로 본다).
inline constexpr uint32 HeaderMaxWarnings = 65535;
inline constexpr uint32 HeaderMaxSections = MeshMaxPolygonGroups;

// 검증이 data를 "있음"으로 보게 두는 임시 bulk id다(채우기가 실제 ref로 바꾼다).
inline constexpr uint32 PendingBulkId = 1;

inline void PutHeaderStream(ByteWriter& writer, const MeshRenderStream& stream) noexcept {
    writer.WriteU32LE(stream.vertexFormat);
    writer.WriteU64LE(stream.offset);
    writer.WriteU64LE(stream.byteSize);
}

inline void ReadHeaderStream(ByteReader& reader, MeshRenderStream& stream) noexcept {
    stream.vertexFormat = reader.ReadU32LE();
    stream.offset = reader.ReadU64LE();
    stream.byteSize = reader.ReadU64LE();
}

inline void PutF64(ByteWriter& writer, double value) noexcept {
    writer.WriteU64LE(platform::BitCast<uint64>(value));
}

inline void PutHeaderBounds(ByteWriter& writer, const Box<double>& box, double radius) noexcept {
    PutF64(writer, box.min.x);
    PutF64(writer, box.min.y);
    PutF64(writer, box.min.z);
    PutF64(writer, box.max.x);
    PutF64(writer, box.max.y);
    PutF64(writer, box.max.z);
    PutF64(writer, radius);
}

inline void ReadHeaderBounds(ByteReader& reader, Box<double>& box, double& radius) noexcept {
    // 성분은 차례로 읽는다(함수 인자 평가 순서는 정해져 있지 않다).
    double bounds[7] = {};
    for (double& component : bounds) {
        component = reader.ReadF64LE();
    }

    box.min = Vec3d(bounds[0], bounds[1], bounds[2]);
    box.max = Vec3d(bounds[3], bounds[4], bounds[5]);
    radius = bounds[6];
}

// 경고 수와 (code, lod)를 쓴다(수는 호출자가 HeaderMaxWarnings 이하로 확인했다).
inline void PutHeaderWarnings(ByteWriter& writer, Span<const MeshCookWarning> warnings) noexcept {
    writer.WriteU32LE(static_cast<uint32>(warnings.Size()));
    for (const MeshCookWarning& warning : warnings) {
        writer.WriteU32LE(static_cast<uint32>(warning.code));
        writer.WriteU32LE(warning.lod);
    }
}

// 경고 수와 항목을 읽는다(수 상한·남은 byte·알려진 진단 코드·lod < lodCount). 위반 InvalidArgument.
inline Result<void> ReadHeaderWarnings(ByteReader& reader, uint32 lodCount,
                                       Array<MeshCookWarning>& warnings) noexcept {
    const uint32 count = reader.ReadU32LE();
    if (reader.Failed() || count > HeaderMaxWarnings ||
        reader.Remaining() < usize{count} * HeaderWarningBytes) {
        return Result<void>::Failure(ErrorCode::InvalidArgument);
    }

    if (auto reserved = warnings.TryReserve(count); !reserved) {
        return reserved;
    }

    for (uint32 index = 0; index < count; ++index) {
        const uint32 code = reader.ReadU32LE();
        const uint32 lod = reader.ReadU32LE();
        const auto value = static_cast<AssetDiagnosticCode>(code);
        if (code > NumericLimits<uint16>::Max() || value == AssetDiagnosticCode::None ||
            StringView(AssetDiagnosticName(value)) == StringView("Unknown") || lod >= lodCount) {
            return Result<void>::Failure(ErrorCode::InvalidArgument);
        }

        (void)warnings.TryPushBack(MeshCookWarning{value, lod});
    }

    return Result<void>::Success();
}

// LOD의 머리 목록 수가 상한 안인지다(UV stream·섹션).
template <class Lod>
bool FitsHeaderLod(const Lod& lod) noexcept {
    return lod.texCoords.Size() <= MeshMaxTexCoordStreams &&
           lod.sections.Size() <= HeaderMaxSections;
}

// LOD 앞부분(정점 수·data byte·위치·tangent 기저·UV 수와 각 UV·색)을 쓴다.
template <class Lod>
void PutHeaderLodStreams(ByteWriter& writer, const Lod& lod) noexcept {
    writer.WriteU32LE(lod.vertexCount);
    writer.WriteU64LE(lod.data.rawBytes);
    PutHeaderStream(writer, lod.positions);
    PutHeaderStream(writer, lod.tangentBasis);
    writer.WriteU32LE(static_cast<uint32>(lod.texCoords.Size()));
    for (const MeshRenderStream& stream : lod.texCoords) {
        PutHeaderStream(writer, stream);
    }

    PutHeaderStream(writer, lod.colors);
}

template <class Lod>
Result<void> ReadHeaderLodStreams(ByteReader& reader, Lod& lod) noexcept {
    if (reader.Remaining() < HeaderLodStreamBytes) {
        return Result<void>::Failure(ErrorCode::InvalidArgument);
    }

    lod.vertexCount = reader.ReadU32LE();
    lod.data.rawBytes = reader.ReadU64LE();
    ReadHeaderStream(reader, lod.positions);
    ReadHeaderStream(reader, lod.tangentBasis);
    const uint32 texCoords = reader.ReadU32LE();
    if (texCoords > MeshMaxTexCoordStreams) {
        return Result<void>::Failure(ErrorCode::InvalidArgument);
    }

    for (uint32 stream = 0; stream < texCoords; ++stream) {
        MeshRenderStream value;
        ReadHeaderStream(reader, value);
        if (auto pushed = lod.texCoords.TryPushBack(value); !pushed) {
            return pushed;
        }
    }

    ReadHeaderStream(reader, lod.colors);
    return Result<void>::Success();
}

// LOD 뒷부분(index 형식·index·섹션 수와 섹션 다섯 정수·다섯 bool)을 쓴다.
template <class Lod>
void PutHeaderLodIndices(ByteWriter& writer, const Lod& lod) noexcept {
    writer.WriteU32LE(lod.indexFormat);
    PutHeaderStream(writer, lod.indices);
    writer.WriteU32LE(static_cast<uint32>(lod.sections.Size()));
    for (const MeshRenderSection& section : lod.sections) {
        writer.WriteU32LE(section.materialIndex);
        writer.WriteU32LE(section.firstIndex);
        writer.WriteU32LE(section.triangleCount);
        writer.WriteU32LE(section.minVertex);
        writer.WriteU32LE(section.maxVertex);
        writer.WriteU8(section.enableCollision ? 1 : 0);
        writer.WriteU8(section.castShadow ? 1 : 0);
        writer.WriteU8(section.visibleInRayTracing ? 1 : 0);
        writer.WriteU8(section.affectDistanceFieldLighting ? 1 : 0);
        writer.WriteU8(section.forceOpaque ? 1 : 0);
    }
}

template <class Lod>
Result<void> ReadHeaderLodIndices(ByteReader& reader, Lod& lod) noexcept {
    if (reader.Remaining() < HeaderLodIndexBytes) {
        return Result<void>::Failure(ErrorCode::InvalidArgument);
    }

    lod.indexFormat = reader.ReadU32LE();
    ReadHeaderStream(reader, lod.indices);
    const uint32 sections = reader.ReadU32LE();
    if (sections > HeaderMaxSections || reader.Remaining() < usize{sections} * HeaderSectionBytes) {
        return Result<void>::Failure(ErrorCode::InvalidArgument);
    }

    if (auto reserved = lod.sections.TryReserve(sections); !reserved) {
        return reserved;
    }

    for (uint32 section = 0; section < sections; ++section) {
        MeshRenderSection value;
        value.materialIndex = reader.ReadU32LE();
        value.firstIndex = reader.ReadU32LE();
        value.triangleCount = reader.ReadU32LE();
        value.minVertex = reader.ReadU32LE();
        value.maxVertex = reader.ReadU32LE();
        uint8 flags[5] = {};
        for (uint8& flag : flags) {
            flag = reader.ReadU8();
            if (flag > 1) {
                return Result<void>::Failure(ErrorCode::InvalidArgument);
            }
        }

        value.enableCollision = flags[0] != 0;
        value.castShadow = flags[1] != 0;
        value.visibleInRayTracing = flags[2] != 0;
        value.affectDistanceFieldLighting = flags[3] != 0;
        value.forceOpaque = flags[4] != 0;
        (void)lod.sections.TryPushBack(value);
    }

    return Result<void>::Success();
}

// 실패 진단을 채우고 error로 실패한다.
inline Result<void> CookFail(const CookContext& context, AssetDiagnosticCode code, ErrorCode error,
                             uint64 detail = 0) noexcept {
    if (context.failure != nullptr) {
        *context.failure = AssetDiagnostic{};
        context.failure->code = code;
        context.failure->error = error;
        context.failure->asset = context.entry != nullptr ? context.entry->id : AssetId{};
        context.failure->detail = detail;
    }

    return Result<void>::Failure(error);
}

// 처리기 경고를 문맥에 둔다(없으면 버린다). setting은 쿠킹 LOD 번호다.
inline Result<void> CookWarn(const CookContext& context, AssetDiagnosticCode code,
                             uint32 lod) noexcept {
    if (context.warnings == nullptr) {
        return Result<void>::Success();
    }

    const AssetId asset = context.entry != nullptr ? context.entry->id : AssetId{};
    return context.warnings->TryPushBack(
        CookDiagnostic{code, true, asset, {}, lod, ErrorCode::None});
}

// 경고를 쿠킹 LOD 순서로 문맥에 둔다(LOD마다 입력 선택 경고, 그다음 빌드 경고 — 빌드 경고는
// 적중이면 머리의 것이다).
inline Result<void> ReportWarnings(const CookContext& context, uint32 lodCount,
                                   Span<const MeshCookWarning> selection,
                                   Span<const MeshCookWarning> build) noexcept {
    for (uint32 lod = 0; lod < lodCount; ++lod) {
        for (const MeshCookWarning& warning : selection) {
            if (warning.lod == lod) {
                if (auto warned = CookWarn(context, warning.code, lod); !warned) {
                    return warned;
                }
            }
        }

        for (const MeshCookWarning& warning : build) {
            if (warning.lod == lod) {
                if (auto warned = CookWarn(context, warning.code, lod); !warned) {
                    return warned;
                }
            }
        }
    }

    return Result<void>::Success();
}

// 미적중 값을 Put한다(bucket mesh, 구성원 하나). 충돌은 NuggetConflict 경고, Put 실패는 required일
// 때만 오류다.
inline Result<void> PutNuggets(const CookContext& context, const Sha256Digest& key, AssetId asset,
                               Span<NuggetValue> values) noexcept {
    NuggetCachePut put;
    put.bucket = NuggetBucketMesh;
    put.key = key;
    put.values = values;
    const AssetId members[1] = {asset};
    const NuggetCachePutResult stored = context.nuggets->PutScc(Span<const AssetId>(members), put);
    if (stored.status == NuggetStatus::Conflict) {
        if (auto warned = CookWarn(context, AssetDiagnosticCode::NuggetConflict, 0); !warned) {
            return warned;
        }
    }

    if (stored.failed && context.nuggets->IsRequired()) {
        return CookFail(context, AssetDiagnosticCode::None,
                        stored.error != ErrorCode::None ? stored.error : ErrorCode::IoError);
    }

    return Result<void>::Success();
}

// 머티리얼 슬롯의 임포트 이름 원문이다(lease가 view를 살려 둔다).
inline Result<void> AcquireSlotNames(NamePool& names,
                                     const StructRefArray<MeshMaterialSlot>& materialSlots,
                                     Array<NameLease>& leases, Array<StringView>& views) noexcept {
    for (usize index = 0; index < materialSlots.Size(); ++index) {
        auto lease = names.Acquire(materialSlots[index].importedSlotName);
        if (!lease) {
            return Result<void>::Failure(lease.Error());
        }

        const StringView view = lease.Value().View();
        if (!leases.TryPushBack(Move(lease.Value())) || !views.TryPushBack(view)) {
            return Result<void>::Failure(ErrorCode::OutOfMemory);
        }
    }

    return Result<void>::Success();
}

// 섹션 하나의 유효 섹션 정보다(found면 표 항목, 아니면 슬롯 대응 — 5.5.2절 6).
struct SectionChoice {
    bool found = false;
    MeshSectionInfo info;
};

inline const MeshSectionInfoEntry* FindSection(Span<const MeshSectionInfoEntry> table, int32 lod,
                                               int32 section) noexcept {
    for (const MeshSectionInfoEntry& entry : table) {
        if (entry.lod == lod && entry.section == section) {
            return &entry;
        }
    }

    return nullptr;
}

// 원본 LOD lod의 섹션 section 정보다(그 LOD → LOD 0 → 없음 — UE FMeshSectionInfoMap::Get).
inline SectionChoice ChooseSection(Span<const MeshSectionInfoEntry> table, uint32 lod,
                                   uint32 section) noexcept {
    SectionChoice choice;
    const MeshSectionInfoEntry* entry =
        FindSection(table, static_cast<int32>(lod), static_cast<int32>(section));
    if (entry == nullptr) {
        entry = FindSection(table, 0, static_cast<int32>(section));
    }

    if (entry != nullptr) {
        choice.found = true;
        choice.info = entry->info;
    }

    return choice;
}

// 원본 LOD lod의 섹션 groups개 정보를 빌드 입력(표에 없으면 슬롯 대응)과 패킹 플래그(표에 없으면
// 기본값)로 고른다.
inline Result<void> ChooseSections(Span<const MeshSectionInfoEntry> table, uint32 lod,
                                   uint32 groups, Array<MeshBuildSectionInfo>& build,
                                   Array<MeshSectionInfo>& pack) noexcept {
    if (!build.TryReserve(groups) || !pack.TryReserve(groups)) {
        return Result<void>::Failure(ErrorCode::OutOfMemory);
    }

    for (uint32 section = 0; section < groups; ++section) {
        const SectionChoice choice = ChooseSection(table, lod, section);
        MeshBuildSectionInfo info;
        info.castShadow = choice.info.castShadow;
        if (choice.found && choice.info.materialIndex >= 0) {
            info.materialIndex = static_cast<uint32>(choice.info.materialIndex);
        }

        (void)build.TryPushBack(info);
        (void)pack.TryPushBack(choice.info);
    }

    return Result<void>::Success();
}

// 원본 LOD의 정체성(형식 버전·요소 수·위상·속성 레코드)을 키에 쓴다(5.5.2절 10).
inline Result<void> WriteSourceKey(NuggetKeyWriter& writer, NamePool& names,
                                   const MeshSourceData& data) noexcept {
    writer.WriteU32(data.formatVersion);
    writer.WriteU32(data.vertexCount);
    writer.WriteU32(data.cornerCount);
    writer.WriteU32(data.edgeCount);
    writer.WriteU32(data.polygonCount);
    writer.WriteU32(data.triangleCount);
    writer.WriteU32(data.polygonGroupCount);
    writer.WriteDigest(data.topology.sha256);
    writer.WriteU64(data.topology.rawBytes);
    writer.WriteList(data.attributes.Size());
    for (const MeshAttributeRecord& record : data.attributes) {
        auto name = names.Acquire(record.name);
        if (!name) {
            return Result<void>::Failure(name.Error());
        }

        writer.WriteString(name.Value().View());
        writer.WriteU32(static_cast<uint32>(record.element));
        writer.WriteU32(static_cast<uint32>(record.type));
        writer.WriteU32(record.channels);
        writer.WriteDigest(record.data.sha256);
        writer.WriteU64(record.data.rawBytes);
    }

    return Result<void>::Success();
}

// 원본 LOD lod의 섹션 groups개의 유효 섹션 정보를 키에 쓴다(5.5.2절 10).
inline void WriteSectionKey(NuggetKeyWriter& writer, Span<const MeshSectionInfoEntry> table,
                            uint32 lod, uint32 groups) noexcept {
    writer.WriteList(groups);
    for (uint32 section = 0; section < groups; ++section) {
        const SectionChoice choice = ChooseSection(table, lod, section);
        writer.WriteBool(choice.found);
        writer.WriteI64(choice.info.materialIndex);
        writer.WriteBool(choice.info.enableCollision);
        writer.WriteBool(choice.info.castShadow);
        writer.WriteBool(choice.info.visibleInRayTracing);
        writer.WriteBool(choice.info.affectDistanceFieldLighting);
        writer.WriteBool(choice.info.forceOpaque);
    }
}

// 원본 LOD 번호 source를 쿠킹 LOD 번호로 옮기고 [0, 쿠킹 LOD 수 − 1]로 자른다(5.5.2절 9). Input은
// sourceLod 필드를 가진다.
template <class Input>
int32 MapLod(Span<Input> inputs, int32 source) noexcept {
    if (inputs.IsEmpty() || source <= 0) {
        return 0;
    }

    for (usize index = 0; index < inputs.Size(); ++index) {
        if (static_cast<int32>(inputs[index].sourceLod) >= source) {
            return static_cast<int32>(index);
        }
    }

    return static_cast<int32>(inputs.Size() - 1);
}

} // namespace rocks::mesh_cook
