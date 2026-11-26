#include <Texture/TextureSettingsCheck.h>

#include <Base/Compiler.h>
#include <Core/Algorithms.h>
#include <Object/ObjectRegistry.h>
#include <ObjectSerialization/ClassDefaults.h>
#include <Texture/Texture.generated.h>
#include <Texture/Texture.h>

// 텍스처 설정 판정이다(Docs/Design/AssetSystem 12.8절). 계약 필드 표와 영향 조건, 값 판정을 둔다.
// 판정 대상이 에디터 데이터라 에디터 데이터가 없는 빌드에는 판정이 없다(Unsupported — 빌드·쿠킹은
// 에디터 빌드에서만 돈다).
namespace rocks {

#if ROCKS_WITH_EDITOR_DATA
namespace {

// 계약 필드가 결과를 바꾸는 조건이다(거짓이면 판정하지 않는다 — UE 새 텍스처 설정이 걸리지 않게).
enum class ContractCondition : uint8 {
    Always,
    ChromaKey,
    AlphaCoverage,
    Composite,
    CustomColorSpace,
    VirtualTexture
};

// 계약 필드 하나다(도구 이름 — member가 있으면 그 struct 멤버만).
struct ContractField {
    const char* field;
    const char* member;
    ContractCondition condition;
};

constexpr ContractField ContractFields[] = {
    {"adjustBrightness", nullptr, ContractCondition::Always},
    {"adjustBrightnessCurve", nullptr, ContractCondition::Always},
    {"adjustVibrance", nullptr, ContractCondition::Always},
    {"adjustSaturation", nullptr, ContractCondition::Always},
    {"adjustRGBCurve", nullptr, ContractCondition::Always},
    {"adjustHue", nullptr, ContractCondition::Always},
    {"adjustMinAlpha", nullptr, ContractCondition::Always},
    {"adjustMaxAlpha", nullptr, ContractCondition::Always},
    {"chromaKeyTexture", nullptr, ContractCondition::Always},
    {"chromaKeyThreshold", nullptr, ContractCondition::ChromaKey},
    {"chromaKeyColor", nullptr, ContractCondition::ChromaKey},
    {"doScaleMipsForAlphaCoverage", nullptr, ContractCondition::Always},
    {"alphaCoverageThresholds", nullptr, ContractCondition::AlphaCoverage},
    {"compositeTexture", nullptr, ContractCondition::Always},
    {"compositeTextureMode", nullptr, ContractCondition::Composite},
    {"compositePower", nullptr, ContractCondition::Composite},
    {"downscale", nullptr, ContractCondition::Always},
    {"downscaleOptions", nullptr, ContractCondition::Always},
    {"sourceColorSettings", "redChromaticityCoordinate", ContractCondition::CustomColorSpace},
    {"sourceColorSettings", "greenChromaticityCoordinate", ContractCondition::CustomColorSpace},
    {"sourceColorSettings", "blueChromaticityCoordinate", ContractCondition::CustomColorSpace},
    {"sourceColorSettings", "whiteChromaticityCoordinate", ContractCondition::CustomColorSpace},
    {"sourceColorSettings", "chromaticAdaptationMethod", ContractCondition::CustomColorSpace},
    {"compressionYCoCg", nullptr, ContractCondition::Always},
    {"layerFormatSettings", nullptr, ContractCondition::Always},
    {"lossyCompressionAmount", nullptr, ContractCondition::Always},
    {"compressionQuality", nullptr, ContractCondition::Always},
    {"oodlePreserveExtremes", nullptr, ContractCondition::Always},
    {"numCinematicMipLevels", nullptr, ContractCondition::Always},
    {"virtualTextureStreaming", nullptr, ContractCondition::Always},
    {"virtualTextureStreamingPriority", nullptr, ContractCondition::VirtualTexture},
    {"useVirtualTextureStreamingPriority", nullptr, ContractCondition::VirtualTexture},
    {"virtualTexturePrefetchMips", nullptr, ContractCondition::VirtualTexture},
    {"source", "numMips", ContractCondition::Always},
    {"source", "numLayers", ContractCondition::Always},
};

// 판정에 쓰는 값 사본이다(Borrow를 오래 쥐지 않는다).
struct TextureFacts {
    bool chromaKey = false;
    bool alphaCoverage = false;
    bool composite = false;
    bool virtualTexture = false;
    TextureMipGenSettings mipGen = TextureMipGenSettings::FromTextureGroup;
    TextureSourceEncoding encoding = TextureSourceEncoding::None;
    TextureColorSpace colorSpace = TextureColorSpace::None;
    TextureMipLoadOptions mipLoad = TextureMipLoadOptions::Default;
    TextureCompressionSettings compression = TextureCompressionSettings::Default;
};

bool Holds(ContractCondition condition, const TextureFacts& facts) noexcept {
    switch (condition) {
    case ContractCondition::Always:
        return true;
    case ContractCondition::ChromaKey:
        return facts.chromaKey;
    case ContractCondition::AlphaCoverage:
        return facts.alphaCoverage;
    case ContractCondition::Composite:
        return facts.composite;
    case ContractCondition::CustomColorSpace:
        return facts.colorSpace == TextureColorSpace::Custom;
    case ContractCondition::VirtualTexture:
        return facts.virtualTexture;
    }

    return false;
}

bool NameIs(const FieldSchema& field, const char* name) noexcept {
    return StringView(field.name.Data(), field.name.Size()) == StringView(name);
}

// schema에서 도구 이름의 필드 위치다(없으면 IndexNone).
usize FieldIndexNamed(const TypeSchema& schema, const char* name) noexcept {
    for (usize index = 0; index < schema.fields.Size(); ++index) {
        if (NameIs(schema.fields[index], name)) {
            return index;
        }
    }

    return IndexNone;
}

// 이슈 하나를 정렬 키와 함께 더한다(key = 최상위 필드 위치 × 멤버 칸 + 멤버 위치 + 1).
struct KeyedIssue {
    uint64 key = 0;
    TextureSettingIssue issue;
};

constexpr uint64 MemberSlots = 1u << 16;

Result<void> AddIssue(Array<KeyedIssue>& out, AssetDiagnosticCode code, const TypeSchema& schema,
                      usize fieldIndex, StructView view, usize memberIndex) noexcept {
    KeyedIssue keyed;
    keyed.key = static_cast<uint64>(fieldIndex) * MemberSlots +
                (memberIndex == IndexNone ? 0 : memberIndex + 1);
    keyed.issue.code = code;
    keyed.issue.path.steps[0] = PathStep{schema.fields[fieldIndex].id};
    keyed.issue.path.count = 1;
    if (memberIndex != IndexNone) {
        keyed.issue.path.steps[1] = PathStep{view.Schema().fields[memberIndex].id};
        keyed.issue.path.count = 2;
    }

    // 같은 위치는 한 번만(Downscale처럼 멤버 여럿이 다르면 필드 이슈 하나).
    for (const KeyedIssue& existing : out) {
        if (existing.key == keyed.key) {
            return Result<void>::Success();
        }
    }

    return out.TryPushBack(keyed);
}

// 값 판정 이슈를 더한다(최상위 또는 SourceColorSettings 멤버).
Result<void> AddValueIssues(Array<KeyedIssue>& out, const TypeLease& lease,
                            const TextureFacts& facts) noexcept {
    const TypeSchema& schema = lease.Schema();
    auto top = [&](const char* name, AssetDiagnosticCode code) noexcept {
        const usize index = FieldIndexNamed(schema, name);
        return index == IndexNone ? Result<void>::Failure(ErrorCode::NotFound)
                                  : AddIssue(out, code, schema, index, StructView{}, IndexNone);
    };
    auto member = [&](const char* name) noexcept {
        const usize index = FieldIndexNamed(schema, "sourceColorSettings");
        if (index == IndexNone) {
            return Result<void>::Failure(ErrorCode::NotFound);
        }

        const StructView view = lease.FieldStruct(index);
        const usize at = FieldIndexNamed(view.Schema(), name);
        return at == IndexNone ? Result<void>::Failure(ErrorCode::NotFound)
                               : AddIssue(out, AssetDiagnosticCode::UnsupportedSetting, schema,
                                          index, view, at);
    };

    Result<void> added = Result<void>::Success();
    switch (facts.mipGen) {
    case TextureMipGenSettings::FromTextureGroup:
    case TextureMipGenSettings::SimpleAverage:
    case TextureMipGenSettings::NoMipmaps:
    case TextureMipGenSettings::Unfiltered:
        break;
    default:
        added = top("mipGenSettings", AssetDiagnosticCode::UnsupportedSetting);
        break;
    }

    if (added && facts.encoding != TextureSourceEncoding::None &&
        facts.encoding != TextureSourceEncoding::Linear &&
        facts.encoding != TextureSourceEncoding::sRGB) {
        added = member("encodingOverride");
    }

    if (added && facts.colorSpace != TextureColorSpace::None &&
        facts.colorSpace != TextureColorSpace::sRGB) {
        added = member("colorSpace");
    }

    if (added && facts.mipLoad == TextureMipLoadOptions::OnlyFirstMip) {
        added = top("mipLoadOptions", AssetDiagnosticCode::UnsupportedSetting);
    }

    if (added && (facts.compression == TextureCompressionSettings::LQ ||
                  facts.compression == TextureCompressionSettings::EncodedReflectionCapture)) {
        added = top("compressionSettings", AssetDiagnosticCode::InvalidSetting);
    }

    return added;
}

// 다른 위치(FindChangedFields) 하나가 계약 필드면 이슈로 더한다.
Result<void> AddContractIssue(Array<KeyedIssue>& out, const TypeLease& lease,
                              const PropertyPathBuffer& path, const TextureFacts& facts) noexcept {
    const TypeSchema& schema = lease.Schema();
    const usize fieldIndex = lease.FieldIndex(path.steps[0].fieldId);
    if (fieldIndex == TypeLease::NoField) {
        return Result<void>::Success();
    }

    const FieldSchema& field = schema.fields[fieldIndex];
    for (const ContractField& contract : ContractFields) {
        if (!NameIs(field, contract.field) || !Holds(contract.condition, facts)) {
            continue;
        }

        if (contract.member == nullptr) {
            return AddIssue(out, AssetDiagnosticCode::UnsupportedSetting, schema, fieldIndex,
                            StructView{}, IndexNone);
        }

        if (path.count < 2) {
            continue;
        }

        const StructView view = lease.FieldStruct(fieldIndex);
        const usize memberIndex =
            view.IsEmpty() ? IndexNone : view.FieldIndex(path.steps[1].fieldId);
        if (memberIndex != IndexNone &&
            NameIs(view.Schema().fields[memberIndex], contract.member)) {
            return AddIssue(out, AssetDiagnosticCode::UnsupportedSetting, schema, fieldIndex, view,
                            memberIndex);
        }
    }

    return Result<void>::Success();
}

} // namespace

Result<void> CheckTextureSettings(ObjectRegistry& registry, const ObjectAccessContext& access,
                                  ObjectHandle texture, Allocator& scratch,
                                  Array<TextureSettingIssue>& out) noexcept {
    out.Clear();
    auto lease = registry.AcquireType(access, texture);
    if (!lease) {
        return Result<void>::Failure(lease.Error());
    }

    if (!lease.Value().IsA(TypeIdOf<Texture>())) {
        return Result<void>::Failure(ErrorCode::TypeMismatch);
    }

    TextureFacts facts;
    {
        auto borrowed = registry.Resolve<Texture>(access, texture);
        if (!borrowed) {
            return Result<void>::Failure(borrowed.Error());
        }

        auto value = borrowed.Value().Get();
        if (!value) {
            return Result<void>::Failure(value.Error());
        }

        const Texture& read = *value.Value();
        // 에디터 데이터가 없는 빌드는 EditorOnly 설정이 없으므로 그 사실은 기본값(거짓)이다.
#if ROCKS_WITH_EDITOR_DATA
        facts.chromaKey = read.chromaKeyTexture;
        facts.alphaCoverage = read.doScaleMipsForAlphaCoverage;
        facts.composite = !read.compositeTexture.Handle().IsNull();
        facts.mipGen = read.mipGenSettings;
        facts.encoding = read.sourceColorSettings.encodingOverride;
        facts.colorSpace = read.sourceColorSettings.colorSpace;
#endif
        facts.virtualTexture = read.virtualTextureStreaming != 0;
        facts.mipLoad = read.mipLoadOptions;
        facts.compression = read.compressionSettings;
    }

    Array<KeyedIssue> issues(scratch);
    // 레코드 없는 타입은 값 판정만 한다 — 분류를 먼저 보아 인코딩 중 Unsupported와 가른다.
    auto status = ClassifyClassDefaults(lease.Value(), scratch);
    if (!status) {
        return Result<void>::Failure(status.Error());
    }

    if (status.Value().availability != ClassDefaultsAvailability::WithoutRecord) {
        Array<PropertyPathBuffer> changed(scratch);
        if (auto found = FindChangedFields(registry, access, texture, scratch, changed); !found) {
            return found;
        }

        for (const PropertyPathBuffer& path : changed) {
            if (auto added = AddContractIssue(issues, lease.Value(), path, facts); !added) {
                return added;
            }
        }
    }

    if (auto added = AddValueIssues(issues, lease.Value(), facts); !added) {
        return added;
    }

    algo::Sort(issues.View(), [](const KeyedIssue& left, const KeyedIssue& right) noexcept {
        return left.key < right.key;
    });
    if (auto reserved = out.TryReserve(issues.Size()); !reserved) {
        return reserved;
    }

    for (const KeyedIssue& keyed : issues) {
        (void)out.TryPushBack(keyed.issue);
    }

    return Result<void>::Success();
}

#else

Result<void> CheckTextureSettings(ObjectRegistry&, const ObjectAccessContext&, ObjectHandle,
                                  Allocator&, Array<TextureSettingIssue>& out) noexcept {
    out.Clear();
    return Result<void>::Failure(ErrorCode::Unsupported);
}

#endif

} // namespace rocks
