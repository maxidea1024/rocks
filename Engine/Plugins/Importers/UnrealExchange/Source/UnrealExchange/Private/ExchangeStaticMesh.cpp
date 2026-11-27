#include <Core/Text/StringBuilder.h>
#include <Mesh/Common/MeshReductionSettings.generated.h>
#include <Mesh/Common/MeshSectionInfo.generated.h>
#include <Mesh/Common/MeshUvDensity.generated.h>
#include <Mesh/Common/VirtualGeometrySettings.generated.h>
#include <Mesh/MeshBuildSettings.generated.h>
#include <ReflectionText/TextJson.h>
#include <UnrealExchange/ExchangeStaticMesh.h>
#include <UnrealExchange/ExchangeStaticMeshSettings.h>

#include "ExchangeMeshCommon.h"

// 교환 정적 메시 → 정적 메시 임포트 명세 변환이다(Docs/Design/UnrealExchange 5.5절 1–9번).

namespace rocks {

namespace exchange_static_mesh {

using namespace exchange_detail;
using SettingRow = ExchangeStaticMeshSettingRow;

// 표에 없는 자리다(LodMeshes).
constexpr uint32 NoMesh = NumericLimits<uint32>::Max();
// UE FMeshSectionInfoMap 키 = LODIndex << 16 | SectionIndex(StaticMesh.cpp GetMeshMaterialKey)다.
constexpr uint32 SectionKeyShift = 16;
constexpr uint64 SectionKeyMask = 0xffff;
// UE ECollisionEnabled 값 이름(CollisionEnabled 순서 — MeshAnimationAssets 4.8절 1)이다.
constexpr StringView CollisionEnabledNames[] = {"NoCollision",     "QueryOnly", "PhysicsOnly",
                                                "QueryAndPhysics", "ProbeOnly", "QueryAndProbe"};
// UE ECollisionTraceFlag 값 이름(CollisionComplexity 순서)이다.
constexpr StringView TraceFlagNames[] = {"CTF_UseDefault", "CTF_UseSimpleAndComplex",
                                         "CTF_UseSimpleAsComplex", "CTF_UseComplexAsSimple"};

// 받지 않는 level set 계열 형상 배열이다(5.5절 9번).
struct UnsupportedShapeKey {
    StringView key;
    UnsupportedCollisionFeature feature;
};

constexpr UnsupportedShapeKey UnsupportedShapes[] = {
    {"LevelSetElems", UnsupportedCollisionFeature::LevelSet},
    {"SkinnedLevelSetElems", UnsupportedCollisionFeature::SkinnedLevelSet},
    {"MLLevelSetElems", UnsupportedCollisionFeature::MLLevelSet},
    {"SkinnedTriangleMeshElems", UnsupportedCollisionFeature::SkinnedTriangleMesh},
};

} // namespace exchange_static_mesh

// ExchangeStaticMesh::Build의 단계들이다(private 상태에 쓴다).
struct ExchangeStaticMeshBuilder : exchange_detail::ExchangeMeshContext {
    ExchangeStaticMesh& mesh;
    const ExchangeStaticMeshInput& input;
    // 모든 기록에서 NFC로 바뀐 이름 수다.
    uint64 normalized = 0;

    // 설정 덤프를 표로 번역해 mesh.translated_에 둔다(경고는 mesh.warnings_ 뒤).
    Result<void> Translate() noexcept {
        return ExchangeMeshContext::Translate(input.asset->settings,
                                              ExchangeStaticMeshSettingRows(),
                                              ExchangeClass::StaticMesh, mesh.translated_);
    }

    // 기록 하나를 적재해 mesh.meshes_ 번호를 index에 둔다.
    Result<void> LoadRecord(JsonValue record, uint64 lod, uint32& index) noexcept {
        auto made = mesh.meshes_.TryEmplaceBack(allocator);
        if (!made) {
            return Result<void>::Failure(made.Error());
        }

        uint64 changed = 0;
        if (auto loaded = LoadExchangeMeshSource(*input.asset, input.blobs, record, names,
                                                 *made.Value(), changed, failure);
            !loaded) {
            failure.detail = lod;
            return loaded;
        }

        normalized += changed;
        index = static_cast<uint32>(mesh.meshes_.Size() - 1);
        return Result<void>::Success();
    }

    // LOD마다 설정·원본이다(5.5절 2·3번).
    Result<void> ReadLods() noexcept {
        using namespace exchange_static_mesh;
        const JsonValue models = Member(mesh.translated_.Root(), "SourceModels");
        const JsonValue dumpModels = Member(input.asset->settings, "SourceModels");
        const JsonValue lods = Member(input.asset->data, "lods");
        if (!models.IsArray() || !dumpModels.IsArray() || !lods.IsArray() ||
            models.Size() != lods.Size() || dumpModels.Size() != lods.Size()) {
            return Corrupt(0);
        }

        for (usize index = 0; index < lods.Size(); ++index) {
            const JsonValue model = models.At(index);
            const JsonValue lod = lods.At(index);
            StaticMeshImportLod entry;
            if (auto read = ImportValue(Member(model, "BuildSettings"), entry.build, index);
                !read) {
                return read;
            }

            if (auto read = ImportValue(Member(model, "ReductionSettings"), entry.reduction, index);
                !read) {
                return read;
            }

            if (!OptFloat(Member(Member(model, "ScreenSize"), "defaultValue"), entry.screenSize)) {
                return Corrupt(index);
            }

            bool hasSource = false;
            bool generated = false;
            if (!ReadBool(Member(lod, "hasSource"), hasSource) ||
                !ReadBool(Member(lod, "generated"), generated)) {
                return Corrupt(index);
            }

            ExchangeStaticMesh::LodMeshes meshes;
            const bool unreal = input.normals == ExchangeMeshNormals::Unreal;
            if (hasSource) {
                // 원본이 있다는 LOD에 기록이 없으면 플러그인 진단(MeshSourceUnavailable)이 이미
                // 문서를 막았다 — 여기 오면 형식 위반이다.
                const JsonValue record = Member(lod, unreal ? "prepared" : "source");
                if (!record.IsObject()) {
                    return Corrupt(index);
                }

                if (auto loaded = LoadRecord(record, index, meshes.source); !loaded) {
                    return loaded;
                }

                if (unreal) {
                    // UE가 계산한 값이다(5.3절) — 원래 플래그는 ImportRecord 원본(에셋 JSON)에
                    // 남는다.
                    const JsonValue prepared = Member(lod, "preparedBuild");
                    entry.build.recomputeNormals = false;
                    entry.build.recomputeTangents = false;
                    entry.build.generateLightmapUVs = false;
                    if (!ReadInt32(Member(prepared, "SrcLightmapIndex"),
                                   entry.build.srcLightmapIndex) ||
                        !ReadInt32(Member(prepared, "DstLightmapIndex"),
                                   entry.build.dstLightmapIndex)) {
                        return Corrupt(index);
                    }
                }
            }

            const JsonValue baked = Member(lod, "baked");
            if (generated && baked.IsObject()) {
                if (auto loaded = LoadRecord(baked, index, meshes.baked); !loaded) {
                    return loaded;
                }
            } else if (baked.IsValid() && !baked.IsNull()) {
                return Corrupt(index);
            }

            // 거리장 대체 메시는 LOD 0 값만 에셋 칸이다(5.5절 3번).
            AssetId replacement;
            bool present = false;
            const JsonValue field = Member(Member(dumpModels.At(index), "BuildSettings"),
                                           "DistanceFieldReplacementMesh");
            if (auto read = DumpReference(field, replacement, present, index); !read) {
                return read;
            }

            if (index == 0) {
                mesh.spec_.distanceFieldReplacementMesh = replacement;
            } else if (present) {
                if (auto warned = Warn(AssetDiagnosticCode::UnsupportedSetting,
                                       static_cast<uint64>(SettingRow::SourceModels),
                                       "DistanceFieldReplacementMesh");
                    !warned) {
                    return warned;
                }
            }

            if (auto pushed = mesh.lods_.TryPushBack(entry); !pushed) {
                return pushed;
            }

            if (auto pushed = mesh.lodMeshes_.TryPushBack(meshes); !pushed) {
                return pushed;
            }
        }

        return Result<void>::Success();
    }

    // 슬롯이다(5.5절 6번).
    Result<void> ReadSlots() noexcept {
        using namespace exchange_static_mesh;
        return ExchangeMeshContext::ReadSlots(Member(mesh.translated_.Root(), "StaticMaterials"),
                                              mesh.slots_,
                                              static_cast<uint64>(SettingRow::StaticMaterials));
    }

    // 섹션 정보다(5.5절 7번 — 키를 풀어 (lod, section) 엄격 증가로 넣는다).
    Result<void> ReadSections() noexcept {
        using namespace exchange_static_mesh;
        constexpr uint64 Detail = static_cast<uint64>(SettingRow::SectionInfoMap);
        const JsonValue map = Member(mesh.translated_.Root(), "SectionInfoMap");
        const JsonValue items = Member(map, "Map");
        if ((map.IsValid() && !map.IsObject()) || !OptArray(items)) {
            return Corrupt(Detail);
        }

        for (usize index = 0; index < items.Size(); ++index) {
            const JsonValue item = items.At(index);
            uint64 key = 0;
            if (!ReadUInt(Member(item, "key"), key) || key > NumericLimits<uint32>::Max()) {
                return Corrupt(Detail);
            }

            MeshSectionInfoEntry entry;
            entry.lod = static_cast<int32>(key >> SectionKeyShift);
            entry.section = static_cast<int32>(key & SectionKeyMask);
            if (auto read = ImportValue(Member(item, "value"), entry.info, Detail); !read) {
                return read;
            }

            // 삽입 자리(UE 맵 덤프는 키 JSON byte 순이라 숫자 순과 다를 수 있다).
            usize at = mesh.sections_.Size();
            while (at > 0 && (mesh.sections_[at - 1].lod > entry.lod ||
                              (mesh.sections_[at - 1].lod == entry.lod &&
                               mesh.sections_[at - 1].section >= entry.section))) {
                if (mesh.sections_[at - 1].lod == entry.lod &&
                    mesh.sections_[at - 1].section == entry.section) {
                    return Corrupt(Detail);
                }

                --at;
            }

            if (auto inserted = mesh.sections_.TryInsert(entry, at); !inserted) {
                return inserted;
            }
        }

        return Result<void>::Success();
    }

    // 소켓이다(5.5절 8번).
    Result<void> ReadSockets() noexcept {
        using namespace exchange_static_mesh;
        constexpr uint64 Detail = static_cast<uint64>(SettingRow::Sockets);
        const JsonValue sockets = Member(mesh.translated_.Root(), "Sockets");
        if (!OptArray(sockets)) {
            return Corrupt(Detail);
        }

        for (usize index = 0; index < sockets.Size(); ++index) {
            // 같은 하위 객체를 다시 가리키는 항목({subobject}만)은 공유라 받지 않는다.
            const JsonValue properties = Member(sockets.At(index), "properties");
            StaticMeshImportSocket socket;
            socket.createdAtImport = false;
            if (!properties.IsObject() ||
                !OptString(Member(properties, "SocketName"), socket.name) ||
                !OptVector(Member(properties, "RelativeLocation"), socket.relativeLocation) ||
                !OptRotator(Member(properties, "RelativeRotation"), socket.relativeRotation) ||
                !OptVector(Member(properties, "RelativeScale"), socket.relativeScale) ||
                !OptString(Member(properties, "Tag"), socket.tag) ||
                !OptBool(Member(properties, "SocketCreatedAtImport"), socket.createdAtImport)) {
                return Corrupt(Detail);
            }

            socket.name = WithoutNone(socket.name);

            if (auto pushed = mesh.sockets_.TryPushBack(socket); !pushed) {
                return pushed;
            }
        }

        return Result<void>::Success();
    }

    // 형상 공통 필드(UE FKShapeElem)다.
    template <class Shape>
    Result<void> ReadShapeCommon(JsonValue item, Shape& shape, uint64 detail) noexcept {
        using namespace exchange_static_mesh;
        StringView name;
        StringView enabled = CollisionEnabledNames[static_cast<usize>(shape.collisionEnabled)];
        if (!item.IsObject() || !OptString(Member(item, "Name"), name) ||
            !OptBool(Member(item, "bContributeToMass"), shape.contributeToMass) ||
            !OptString(Member(item, "CollisionEnabled"), enabled) ||
            !OptFloat(Member(item, "RestOffset"), shape.restOffset)) {
            return Corrupt(detail);
        }

        const usize state = FindName(CollisionEnabledNames, enabled);
        if (state == sizeof(CollisionEnabledNames) / sizeof(CollisionEnabledNames[0])) {
            return Corrupt(detail);
        }

        shape.collisionEnabled = static_cast<CollisionEnabled>(state);
        auto interned = names.TryIntern(WithoutNone(name));
        if (!interned) {
            return Result<void>::Failure(interned.Error());
        }

        shape.name = interned.Value();
        return Result<void>::Success();
    }

    Result<void> ReadSpheres(JsonValue items) noexcept {
        using namespace exchange_static_mesh;
        for (usize index = 0; index < items.Size(); ++index) {
            const JsonValue item = items.At(index);
            CollisionSphere shape;
            if (auto read = ReadShapeCommon(item, shape, index); !read) {
                return read;
            }

            if (!OptVector(Member(item, "Center"), shape.center) ||
                !OptFloat(Member(item, "Radius"), shape.radius)) {
                return Corrupt(index);
            }

            if (auto pushed = mesh.collision_.spheres.TryPushBack(shape); !pushed) {
                return pushed;
            }
        }

        return Result<void>::Success();
    }

    Result<void> ReadBoxes(JsonValue items) noexcept {
        using namespace exchange_static_mesh;
        for (usize index = 0; index < items.Size(); ++index) {
            const JsonValue item = items.At(index);
            CollisionBox shape;
            if (auto read = ReadShapeCommon(item, shape, index); !read) {
                return read;
            }

            if (!OptVector(Member(item, "Center"), shape.center) ||
                !OptRotator(Member(item, "Rotation"), shape.rotation) ||
                !OptNumber(Member(item, "X"), shape.size.x) ||
                !OptNumber(Member(item, "Y"), shape.size.y) ||
                !OptNumber(Member(item, "Z"), shape.size.z)) {
                return Corrupt(index);
            }

            if (auto pushed = mesh.collision_.boxes.TryPushBack(shape); !pushed) {
                return pushed;
            }
        }

        return Result<void>::Success();
    }

    Result<void> ReadCapsules(JsonValue items) noexcept {
        using namespace exchange_static_mesh;
        for (usize index = 0; index < items.Size(); ++index) {
            const JsonValue item = items.At(index);
            CollisionCapsule shape;
            if (auto read = ReadShapeCommon(item, shape, index); !read) {
                return read;
            }

            if (!OptVector(Member(item, "Center"), shape.center) ||
                !OptRotator(Member(item, "Rotation"), shape.rotation) ||
                !OptFloat(Member(item, "Radius"), shape.radius) ||
                !OptFloat(Member(item, "Length"), shape.length)) {
                return Corrupt(index);
            }

            if (auto pushed = mesh.collision_.capsules.TryPushBack(shape); !pushed) {
                return pushed;
            }
        }

        return Result<void>::Success();
    }

    // 테이퍼드 캡슐이다. Width ≠ 0·bOneSidedCollision은 Rocks 필드가 없어 센다(MeshAnimationAssets
    // 4.8절 6).
    Result<void> ReadTaperedCapsules(JsonValue items, uint64& widths, uint64& oneSided) noexcept {
        using namespace exchange_static_mesh;
        for (usize index = 0; index < items.Size(); ++index) {
            const JsonValue item = items.At(index);
            CollisionTaperedCapsule shape;
            if (auto read = ReadShapeCommon(item, shape, index); !read) {
                return read;
            }

            double width = 0.0;
            bool sided = false;
            if (!OptVector(Member(item, "Center"), shape.center) ||
                !OptRotator(Member(item, "Rotation"), shape.rotation) ||
                !OptFloat(Member(item, "Radius0"), shape.radius0) ||
                !OptFloat(Member(item, "Radius1"), shape.radius1) ||
                !OptFloat(Member(item, "Length"), shape.length) ||
                !OptNumber(Member(item, "Width"), width) ||
                !OptBool(Member(item, "bOneSidedCollision"), sided)) {
                return Corrupt(index);
            }

            widths += width != 0.0 ? 1 : 0;
            oneSided += sided ? 1 : 0;
            if (auto pushed = mesh.collision_.taperedCapsules.TryPushBack(shape); !pushed) {
                return pushed;
            }
        }

        return Result<void>::Success();
    }

    // 볼록 형상이다(점·index는 ConvexGeometry, transform은 UE FTransform 덤프).
    Result<void> ReadConvexes(JsonValue items) noexcept {
        using namespace exchange_static_mesh;
        for (usize index = 0; index < items.Size(); ++index) {
            const JsonValue item = items.At(index);
            CollisionConvex shape;
            if (auto read = ReadShapeCommon(item, shape, index); !read) {
                return read;
            }

            const JsonValue vertices = Member(item, "VertexData");
            const JsonValue indices = Member(item, "IndexData");
            const JsonValue transform = Member(item, "Transform");
            if (!OptArray(vertices) || !OptArray(indices) ||
                (transform.IsValid() && !transform.IsObject()) ||
                !OptQuat(Member(transform, "Rotation"), shape.transform.rotation) ||
                !OptVector(Member(transform, "Translation"), shape.transform.translation) ||
                !OptVector(Member(transform, "Scale3D"), shape.transform.scale)) {
                return Corrupt(index);
            }

            auto geometry = mesh.convexes_.TryEmplaceBack(allocator);
            if (!geometry) {
                return Result<void>::Failure(geometry.Error());
            }

            for (usize point = 0; point < vertices.Size(); ++point) {
                Vec3d value;
                if (!vertices.At(point).IsValid() || !OptVector(vertices.At(point), value)) {
                    return Corrupt(index);
                }

                if (auto pushed = geometry.Value()->vertices.TryPushBack(value); !pushed) {
                    return pushed;
                }
            }

            for (usize at = 0; at < indices.Size(); ++at) {
                uint64 value = 0;
                if (!ReadUInt(indices.At(at), value) || value > NumericLimits<uint32>::Max()) {
                    return Corrupt(index);
                }

                if (auto pushed = geometry.Value()->indices.TryPushBack(static_cast<uint32>(value));
                    !pushed) {
                    return pushed;
                }
            }

            if (auto pushed = mesh.collision_.convexes.TryPushBack(shape); !pushed) {
                return pushed;
            }
        }

        return Result<void>::Success();
    }

    // 충돌이다(5.5절 9번 — data.collision = BodySetup 덤프, null이면 없음).
    Result<void> ReadCollision() noexcept {
        using namespace exchange_static_mesh;
        const JsonValue body = Member(input.asset->data, "collision");
        if (!body.IsValid() || body.IsNull()) {
            return Result<void>::Success();
        }

        const JsonValue geometry = Member(body, "AggGeom");
        const JsonValue spheres = Member(geometry, "SphereElems");
        const JsonValue boxes = Member(geometry, "BoxElems");
        const JsonValue capsules = Member(geometry, "SphylElems");
        const JsonValue tapered = Member(geometry, "TaperedCapsuleElems");
        const JsonValue convexes = Member(geometry, "ConvexElems");
        StringView trace = TraceFlagNames[0];
        if (!body.IsObject() || !geometry.IsObject() || !OptArray(spheres) || !OptArray(boxes) ||
            !OptArray(capsules) || !OptArray(tapered) || !OptArray(convexes) ||
            !OptString(Member(body, "CollisionTraceFlag"), trace) ||
            !OptBool(Member(body, "bDoubleSidedGeometry"), mesh.spec_.doubleSidedGeometry)) {
            return Corrupt(0);
        }

        const usize complexity = FindName(TraceFlagNames, trace);
        if (complexity == sizeof(TraceFlagNames) / sizeof(TraceFlagNames[0])) {
            return Corrupt(0);
        }

        mesh.spec_.complexity = static_cast<CollisionComplexity>(complexity);
        uint64 widths = 0;
        uint64 oneSided = 0;
        if (auto read = ReadSpheres(spheres); !read) {
            return read;
        }

        if (auto read = ReadBoxes(boxes); !read) {
            return read;
        }

        if (auto read = ReadCapsules(capsules); !read) {
            return read;
        }

        if (auto read = ReadTaperedCapsules(tapered, widths, oneSided); !read) {
            return read;
        }

        if (auto read = ReadConvexes(convexes); !read) {
            return read;
        }

        for (const UnsupportedShapeKey& shape : UnsupportedShapes) {
            const JsonValue items = Member(geometry, shape.key);
            if (!OptArray(items)) {
                return Corrupt(0);
            }

            if (items.Size() != 0) {
                if (auto warned = Warn(AssetDiagnosticCode::UnsupportedCollisionShape,
                                       static_cast<uint64>(shape.feature), shape.key);
                    !warned) {
                    return warned;
                }
            }
        }

        if (widths != 0) {
            if (auto warned = Warn(
                    AssetDiagnosticCode::UnsupportedCollisionShape,
                    static_cast<uint64>(UnsupportedCollisionFeature::TaperedCapsuleWidth), "Width");
                !warned) {
                return warned;
            }
        }

        if (oneSided != 0) {
            if (auto warned =
                    Warn(AssetDiagnosticCode::UnsupportedCollisionShape,
                         static_cast<uint64>(UnsupportedCollisionFeature::TaperedCapsuleOneSided),
                         "bOneSidedCollision");
                !warned) {
                return warned;
            }
        }

        mesh.spec_.collision = &mesh.collision_;
        return Result<void>::Success();
    }

    // 에셋 설정이다(5.5절 5번).
    Result<void> ReadAssetSettings() noexcept {
        using namespace exchange_static_mesh;
        const JsonValue root = mesh.translated_.Root();
        StaticMeshImportSpec& spec = mesh.spec_;
        struct Check {
            bool ok;
            SettingRow row;
        };

        const Check checks[] = {
            {OptInt32(Member(root, "LightMapResolution"), spec.lightMapResolution),
             SettingRow::LightMapResolution},
            {OptInt32(Member(root, "LightMapCoordinateIndex"), spec.lightMapCoordinateIndex),
             SettingRow::LightMapCoordinateIndex},
            {OptVector(Member(root, "PositiveBoundsExtension"), spec.positiveBoundsExtension),
             SettingRow::PositiveBoundsExtension},
            {OptVector(Member(root, "NegativeBoundsExtension"), spec.negativeBoundsExtension),
             SettingRow::NegativeBoundsExtension},
            {OptBool(Member(root, "AutoComputeLODScreenSize"), spec.autoComputeLodScreenSize),
             SettingRow::AutoComputeLodScreenSize},
            {OptString(Member(root, "LODGroup"), spec.lodGroup), SettingRow::LodGroup},
            {PerPlatformInt(Member(root, "MinLOD"), spec.minLod), SettingRow::MinLod},
            {PerPlatformInt(Member(root, "NumStreamedLODs"), spec.numStreamedLods),
             SettingRow::NumStreamedLods},
            {OptInt32(Member(root, "LODForCollision"), spec.lodForCollision),
             SettingRow::LodForCollision},
        };

        for (const Check& check : checks) {
            if (!check.ok) {
                return Corrupt(static_cast<uint64>(check.row));
            }
        }

        spec.lodGroup = WithoutNone(spec.lodGroup);
        if (auto read = ImportValue(Member(root, "NaniteSettings"), spec.virtualGeometry,
                                    static_cast<uint64>(SettingRow::NaniteSettings));
            !read) {
            return read;
        }

        return TranslatedReference(Member(root, "ComplexCollisionMesh"), spec.complexCollisionMesh,
                                   static_cast<uint64>(SettingRow::ComplexCollisionMesh));
    }

    // 명세의 포인터·span을 채운다(배열이 더 자라지 않은 뒤).
    void Finish() noexcept {
        for (usize index = 0; index < mesh.lods_.Size(); ++index) {
            mesh.lods_[index].source = mesh.LodSource(index);
            mesh.lods_[index].bakedSource = mesh.LodBakedSource(index);
        }

        StaticMeshImportSpec& spec = mesh.spec_;
        spec.lods = mesh.lods_.View();
        spec.slots = mesh.slots_.View();
        spec.sectionInfo = mesh.sections_.View();
        spec.sockets = mesh.sockets_.View();
        spec.convexes = mesh.convexes_.View();
        spec.replaceSettingsOnReimport = true;
    }
};

ExchangeStaticMesh::ExchangeStaticMesh(Allocator& allocator) noexcept
    : allocator_(&allocator), translated_(allocator), meshes_(allocator), lodMeshes_(allocator),
      lods_(allocator), slots_(allocator), sections_(allocator), sockets_(allocator),
      collision_(allocator), convexes_(allocator), warnings_(allocator) {
}

const MeshSource* ExchangeStaticMesh::LodSource(usize lod) const noexcept {
    if (lod >= lodMeshes_.Size() || lodMeshes_[lod].source == exchange_static_mesh::NoMesh) {
        return nullptr;
    }

    return &meshes_[lodMeshes_[lod].source];
}

const MeshSource* ExchangeStaticMesh::LodBakedSource(usize lod) const noexcept {
    if (lod >= lodMeshes_.Size() || lodMeshes_[lod].baked == exchange_static_mesh::NoMesh) {
        return nullptr;
    }

    return &meshes_[lodMeshes_[lod].baked];
}

Result<void> ExchangeStaticMesh::Build(const ExchangeStaticMeshInput& input,
                                       ExchangeMeshFailure& failure) noexcept {
    failure = ExchangeMeshFailure{};
    if (input.asset == nullptr || input.registry == nullptr || input.registry->Names() == nullptr ||
        input.asset->exchangeClass != ExchangeClass::StaticMesh ||
        input.blobs.Size() != input.asset->blobs.Size() || started_) {
        failure.code = AssetDiagnosticCode::InvalidSource;
        return Result<void>::Failure(ErrorCode::InvalidArgument);
    }

    started_ = true;

    ExchangeStaticMeshBuilder builder{{failure, *allocator_, *input.registry, input.access,
                                       input.references, *input.registry->Names(), warnings_},
                                      *this,
                                      input};
    if (auto done = builder.Translate(); !done) {
        return done;
    }

    if (auto done = builder.ReadLods(); !done) {
        return done;
    }

    if (auto done = builder.ReadSlots(); !done) {
        return done;
    }

    if (auto done = builder.ReadSections(); !done) {
        return done;
    }

    if (auto done = builder.ReadSockets(); !done) {
        return done;
    }

    if (auto done = builder.ReadCollision(); !done) {
        return done;
    }

    if (auto done = builder.ReadAssetSettings(); !done) {
        return done;
    }

    if (builder.normalized != 0) {
        if (auto warned =
                builder.Warn(AssetDiagnosticCode::UnrealNameNormalized, builder.normalized, {});
            !warned) {
            return warned;
        }
    }

    builder.Finish();
    return Result<void>::Success();
}

} // namespace rocks
