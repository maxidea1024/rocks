#include <Mesh/Common/VirtualGeometrySettings.generated.h>
#include <Mesh/MeshBuildSettings.generated.h>
#include <Mesh/SkeletalMesh/SkeletalMeshReductionSettings.generated.h>
#include <UnrealExchange/ExchangeSkeletalMesh.h>
#include <UnrealExchange/ExchangeSkeletalMeshSettings.h>

#include "ExchangeMeshCommon.h"

// 교환 스킨 메시 → 스킨 메시 임포트 명세 변환이다(Docs/Design/UnrealExchange 6.4절 2–6번).

namespace rocks {

namespace exchange_skeletal_mesh {

using namespace exchange_detail;
using SettingRow = ExchangeSkeletalMeshSettingRow;

constexpr uint32 NoMesh = NumericLimits<uint32>::Max();

} // namespace exchange_skeletal_mesh

// ExchangeSkeletalMesh::Build의 단계들이다.
struct ExchangeSkeletalMeshBuilder : exchange_detail::ExchangeMeshContext {
    ExchangeSkeletalMesh& mesh;
    const ExchangeSkeletalMeshInput& input;
    // 모든 기록에서 NFC로 바뀐 이름 수다.
    uint64 normalized = 0;

    // data.materials를 슬롯 표로 번역한다. 슬롯 멤버 경고의 detail은
    // ExchangeSkeletalMeshMaterialsDetail이다(settings 표 밖).
    Result<void> TranslateData() noexcept {
        Array<ExchangeSettingWarning> dataWarnings(allocator);
        ExchangeMeshContext data{failure,    allocator, registry,    access,
                                 references, names,     dataWarnings};
        if (auto done = data.Translate(input.asset->data, ExchangeSkeletalMeshDataRows(),
                                       ExchangeClass::SkeletalMesh, mesh.translatedData_);
            !done) {
            return done;
        }

        for (ExchangeSettingWarning warning : dataWarnings) {
            warning.detail = ExchangeSkeletalMeshMaterialsDetail;
            if (auto pushed = warnings.TryPushBack(warning); !pushed) {
                return pushed;
            }
        }

        return Result<void>::Success();
    }

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

    // UE FBoneReference `{BoneName}` 배열의 이름을 mesh.boneNames_ 뒤에 더한다.
    Result<void> ReadBoneReferences(JsonValue items, usize& start, usize& count,
                                    uint64 detail) noexcept {
        using namespace exchange_skeletal_mesh;
        start = mesh.boneNames_.Size();
        count = 0;
        if (!OptArray(items)) {
            return Corrupt(detail);
        }

        for (usize index = 0; index < items.Size(); ++index) {
            StringView name;
            if (!ReadString(Member(items.At(index), "BoneName"), name)) {
                return Corrupt(detail);
            }

            if (auto pushed = mesh.boneNames_.TryPushBack(name); !pushed) {
                return pushed;
            }

            ++count;
        }

        return Result<void>::Success();
    }

    // LOD마다 설정·원본이다(6.4절 2·3번).
    Result<void> ReadLods() noexcept {
        using namespace exchange_skeletal_mesh;
        const JsonValue models = Member(mesh.translated_.Root(), "SourceModels");
        const JsonValue lods = Member(input.asset->data, "lods");
        if (!models.IsArray() || !lods.IsArray() || models.Size() != lods.Size()) {
            return Corrupt(0);
        }

        for (usize index = 0; index < lods.Size(); ++index) {
            const JsonValue model = models.At(index);
            const JsonValue lod = lods.At(index);
            SkeletalMeshImportLod entry;
            ExchangeSkeletalMesh::LodParts parts;
            if (auto read = ImportValue(Member(model, "BuildSettings"), entry.build, index);
                !read) {
                return read;
            }

            if (auto read = ImportValue(Member(model, "ReductionSettings"), entry.reduction, index);
                !read) {
                return read;
            }

            const JsonValue materialMap = Member(model, "LODMaterialMap");
            if (!OptFloat(Member(Member(model, "ScreenSize"), "defaultValue"), entry.screenSize) ||
                !OptFloat(Member(model, "LODHysteresis"), entry.lodHysteresis) ||
                !OptFloat(Member(model, "WeightOfPrioritization"), entry.weightOfPrioritization) ||
                !OptArray(materialMap)) {
                return Corrupt(index);
            }

            parts.materialMapStart = mesh.materialMaps_.Size();
            parts.materialMapCount = materialMap.Size();
            for (usize at = 0; at < materialMap.Size(); ++at) {
                int32 value = 0;
                if (!ReadInt32(materialMap.At(at), value)) {
                    return Corrupt(index);
                }

                if (auto pushed = mesh.materialMaps_.TryPushBack(value); !pushed) {
                    return pushed;
                }
            }

            if (auto read = ReadBoneReferences(Member(model, "BonesToRemove"), parts.removeStart,
                                               parts.removeCount, index);
                !read) {
                return read;
            }

            if (auto read = ReadBoneReferences(Member(model, "BonesToPrioritize"),
                                               parts.prioritizeStart, parts.prioritizeCount, index);
                !read) {
                return read;
            }

            bool hasSource = false;
            bool generated = false;
            if (!ReadBool(Member(lod, "hasSource"), hasSource) ||
                !ReadBool(Member(lod, "generated"), generated)) {
                return Corrupt(index);
            }

            if (hasSource) {
                const JsonValue record = Member(lod, "source");
                if (!record.IsObject()) {
                    return Corrupt(index);
                }

                if (auto loaded = LoadRecord(record, index, parts.source); !loaded) {
                    return loaded;
                }
            }

            const JsonValue baked = Member(lod, "baked");
            if (generated && baked.IsObject()) {
                if (auto loaded = LoadRecord(baked, index, parts.baked); !loaded) {
                    return loaded;
                }
            } else if (baked.IsValid() && !baked.IsNull()) {
                return Corrupt(index);
            }

            if (auto pushed = mesh.lods_.TryPushBack(entry); !pushed) {
                return pushed;
            }

            if (auto pushed = mesh.lodParts_.TryPushBack(parts); !pushed) {
                return pushed;
            }
        }

        return Result<void>::Success();
    }

    // UE PerPlatformBool·PerPlatformInt `{Default, PerPlatform}`(번역 Same — 원문 모양)의
    // 기본값이다. PerPlatform이 비지 않으면 경고다(플랫폼 이름 대응은 후속).
    template <class T>
    Result<void> ReadPerPlatform(JsonValue value, T& out, StringView key, uint64 detail) noexcept {
        using namespace exchange_skeletal_mesh;
        if (!value.IsValid()) {
            return Result<void>::Success();
        }

        const JsonValue platforms = Member(value, "PerPlatform");
        bool read = false;
        if constexpr (sizeof(T) == sizeof(bool)) {
            read = ReadBool(Member(value, "Default"), out);
        } else {
            read = ReadInt32(Member(value, "Default"), out);
        }

        if (!read || !OptArray(platforms)) {
            return Corrupt(detail);
        }

        return platforms.Size() != 0 ? Warn(AssetDiagnosticCode::UnsupportedSetting, detail, key)
                                     : Result<void>::Success();
    }

    Result<void> ReadProfiles() noexcept {
        using namespace exchange_skeletal_mesh;
        constexpr uint64 Detail = static_cast<uint64>(SettingRow::SkinWeightProfiles);
        const JsonValue profiles = Member(mesh.translated_.Root(), "SkinWeightProfiles");
        if (!OptArray(profiles)) {
            return Corrupt(Detail);
        }

        for (usize index = 0; index < profiles.Size(); ++index) {
            const JsonValue item = profiles.At(index);
            SkeletalMeshImportProfile profile;
            if (!ReadString(Member(item, "Name"), profile.name)) {
                return Corrupt(Detail);
            }

            if (auto read = ReadPerPlatform(Member(item, "DefaultProfile"), profile.defaultProfile,
                                            "DefaultProfile", Detail);
                !read) {
                return read;
            }

            if (auto read = ReadPerPlatform(Member(item, "DefaultProfileFromLODIndex"),
                                            profile.defaultProfileFromLod,
                                            "DefaultProfileFromLODIndex", Detail);
                !read) {
                return read;
            }

            if (auto pushed = mesh.profiles_.TryPushBack(profile); !pushed) {
                return pushed;
            }
        }

        return Result<void>::Success();
    }

    Result<void> ReadAssetSettings() noexcept {
        using namespace exchange_skeletal_mesh;
        const JsonValue root = mesh.translated_.Root();
        SkeletalMeshImportSpec& spec = mesh.spec_;
        if (!OptVector(Member(root, "PositiveBoundsExtension"), spec.positiveBoundsExtension)) {
            return Corrupt(static_cast<uint64>(SettingRow::PositiveBoundsExtension));
        }

        if (!OptVector(Member(root, "NegativeBoundsExtension"), spec.negativeBoundsExtension)) {
            return Corrupt(static_cast<uint64>(SettingRow::NegativeBoundsExtension));
        }

        if (!PerPlatformInt(Member(root, "MinLod"), spec.minLod)) {
            return Corrupt(static_cast<uint64>(SettingRow::MinLod));
        }

        if (auto read = ImportValue(Member(root, "NaniteSettings"), spec.virtualGeometry,
                                    static_cast<uint64>(SettingRow::NaniteSettings));
            !read) {
            return read;
        }

        return TranslatedReference(Member(root, "Skeleton"), spec.skeleton,
                                   static_cast<uint64>(SettingRow::Skeleton));
    }

    void Finish() noexcept {
        for (usize index = 0; index < mesh.lods_.Size(); ++index) {
            const ExchangeSkeletalMesh::LodParts& parts = mesh.lodParts_[index];
            SkeletalMeshImportLod& lod = mesh.lods_[index];
            lod.source = mesh.LodSource(index);
            lod.bakedSource = mesh.LodBakedSource(index);
            lod.lodMaterialMap = Span<const int32>(
                mesh.materialMaps_.Data() + parts.materialMapStart, parts.materialMapCount);
            lod.bonesToRemove = Span<const StringView>(mesh.boneNames_.Data() + parts.removeStart,
                                                       parts.removeCount);
            lod.bonesToPrioritize = Span<const StringView>(
                mesh.boneNames_.Data() + parts.prioritizeStart, parts.prioritizeCount);
        }

        SkeletalMeshImportSpec& spec = mesh.spec_;
        spec.lods = mesh.lods_.View();
        spec.bones = mesh.bones_.View();
        spec.slots = mesh.slots_.View();
        spec.sockets = mesh.sockets_.View();
        spec.skinWeightProfiles = mesh.profiles_.View();
        spec.replaceSettingsOnReimport = true;
    }
};

ExchangeSkeletalMesh::ExchangeSkeletalMesh(Allocator& allocator) noexcept
    : allocator_(&allocator), translated_(allocator), translatedData_(allocator),
      meshes_(allocator), lodParts_(allocator), lods_(allocator), materialMaps_(allocator),
      boneNames_(allocator), bones_(allocator), slots_(allocator), sockets_(allocator),
      profiles_(allocator), warnings_(allocator) {
}

const MeshSource* ExchangeSkeletalMesh::LodSource(usize lod) const noexcept {
    if (lod >= lodParts_.Size() || lodParts_[lod].source == exchange_skeletal_mesh::NoMesh) {
        return nullptr;
    }

    return &meshes_[lodParts_[lod].source];
}

const MeshSource* ExchangeSkeletalMesh::LodBakedSource(usize lod) const noexcept {
    if (lod >= lodParts_.Size() || lodParts_[lod].baked == exchange_skeletal_mesh::NoMesh) {
        return nullptr;
    }

    return &meshes_[lodParts_[lod].baked];
}

Result<void> ExchangeSkeletalMesh::Build(const ExchangeSkeletalMeshInput& input,
                                         ExchangeMeshFailure& failure) noexcept {
    using namespace exchange_skeletal_mesh;
    failure = ExchangeMeshFailure{};
    if (input.asset == nullptr || input.registry == nullptr || input.registry->Names() == nullptr ||
        input.asset->exchangeClass != ExchangeClass::SkeletalMesh ||
        input.blobs.Size() != input.asset->blobs.Size() || started_) {
        failure.code = AssetDiagnosticCode::InvalidSource;
        return Result<void>::Failure(ErrorCode::InvalidArgument);
    }

    started_ = true;
    ExchangeSkeletalMeshBuilder builder{{failure, *allocator_, *input.registry, input.access,
                                         input.references, *input.registry->Names(), warnings_},
                                        *this,
                                        input};
    if (auto done = builder.Translate(input.asset->settings, ExchangeSkeletalMeshSettingRows(),
                                      ExchangeClass::SkeletalMesh, translated_);
        !done) {
        return done;
    }

    if (auto done = builder.TranslateData(); !done) {
        return done;
    }

    if (auto done = builder.ReadBones(Member(input.asset->data, "bones"), bones_, nullptr); !done) {
        return done;
    }

    if (auto done = builder.ReadLods(); !done) {
        return done;
    }

    if (auto done = builder.ReadSlots(Member(translatedData_.Root(), "materials"), slots_,
                                      ExchangeSkeletalMeshMaterialsDetail);
        !done) {
        return done;
    }

    if (auto done = builder.ReadBoneSockets(Member(translated_.Root(), "Sockets"), sockets_,
                                            static_cast<uint64>(SettingRow::Sockets));
        !done) {
        return done;
    }

    if (auto done = builder.ReadProfiles(); !done) {
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
