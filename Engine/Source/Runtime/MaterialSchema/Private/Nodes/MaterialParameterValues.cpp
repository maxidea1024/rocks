#include "MaterialParameterValues.h"

#include <CoreTypeBindings/CoreValueTypes.generated.h>
#include <CoreTypeBindings/CoreValueTypes.h>
#include <MaterialSchema/MaterialAssetReference.generated.h>
#include <MaterialSchema/MaterialAssetReference.h>
#include <MaterialSchema/NodePropertyReader.h>

// 파라미터 노드 값 hook이다(Docs/Design/MaterialSystem 12.8절 MS3d1 — UE GetParameterValue).
// 기록되지 않은 필드는 속성 struct 초기값이다(누락 = 기준 — 색은 노드마다, sortPriority 32).
namespace rocks::material::detail {

namespace {

// 속성 struct의 sortPriority 초기값이다(UE UMaterialExpressionParameter::SortPriority 기본 32).
constexpr int64 DefaultSortPriority = 32;
// 속성 struct의 색 기본값 초기값이다(VectorParameter (0,0,0,0), ChannelMaskParameter (1,0,0,0) — UE
// MaterialExpressionChannelMaskParameter 생성자).
constexpr float ZeroColor[MaterialParameterComponents] = {0.0f, 0.0f, 0.0f, 0.0f};
constexpr float RedMaskColor[MaterialParameterComponents] = {1.0f, 0.0f, 0.0f, 0.0f};

// 공통 필드(이름·ParameterId·그룹·순위)를 읽는다.
Result<void> ReadCommon(NodePropertyReader& properties, MaterialParameterType type,
                        MaterialParameterMetadata& out) noexcept {
    out = MaterialParameterMetadata{};
    out.type = type;
    auto name = properties.ReadText("parameterName");
    if (!name) {
        return Result<void>::Failure(name.Error());
    }

    auto id = properties.ReadGuid("parameterId", Guid{});
    if (!id) {
        return Result<void>::Failure(id.Error());
    }

    auto group = properties.ReadText("group");
    if (!group) {
        return Result<void>::Failure(group.Error());
    }

    auto priority = properties.ReadInteger("sortPriority", DefaultSortPriority);
    if (!priority) {
        return Result<void>::Failure(priority.Error());
    }

    out.name = name.Value();
    out.parameterId = id.Value();
    out.group = group.Value();
    out.sortPriority = static_cast<int32>(priority.Value());
    return Result<void>::Success();
}

// custom primitive data 번호다(UE GetParameterValue — 쓰지 않으면 INDEX_NONE).
Result<void> ReadPrimitiveData(NodePropertyReader& properties,
                               MaterialParameterMetadata& out) noexcept {
    auto used = properties.ReadBool("useCustomPrimitiveData", false);
    if (!used) {
        return Result<void>::Failure(used.Error());
    }

    if (!used.Value()) {
        return Result<void>::Success();
    }

    auto index = properties.ReadInteger("primitiveDataIndex", 0);
    if (!index) {
        return Result<void>::Failure(index.Error());
    }

    out.primitiveDataIndex = static_cast<int32>(index.Value());
    return Result<void>::Success();
}

// MaterialAssetReference 필드의 AssetId다.
Result<Guid> ReadAsset(NodePropertyReader& properties, StringView field) noexcept {
    auto reference = properties.OpenStruct(field, GeneratedType<MaterialAssetReference>::Schema());
    if (!reference) {
        return Result<Guid>::Failure(reference.Error());
    }

    return reference.Value().ReadGuid("assetId", Guid{});
}

// LinearColor 필드다(r·g·b·a).
// 필드가 기록되지 않았으면 fallback(속성 struct 초기값 — 누락 = 기준 규칙)이다.
Result<void> ReadColor(NodePropertyReader& properties, StringView field,
                       const float (&fallback)[MaterialParameterComponents],
                       MaterialParameterMetadata& out) noexcept {
    auto found = properties.Find(field);
    if (!found) {
        return Result<void>::Failure(found.Error());
    }

    if (!found.Value()) {
        for (usize index = 0; index < MaterialParameterComponents; ++index) {
            out.defaults[index] = fallback[index];
        }

        return Result<void>::Success();
    }

    auto color = properties.OpenStruct(field, GeneratedType<LinearColor>::Schema());
    if (!color) {
        return Result<void>::Failure(color.Error());
    }

    static constexpr StringView Channels[MaterialParameterComponents] = {"r", "g", "b", "a"};
    for (usize index = 0; index < MaterialParameterComponents; ++index) {
        auto channel = color.Value().ReadFloat(Channels[index], 0.0f);
        if (!channel) {
            return Result<void>::Failure(channel.Error());
        }

        out.defaults[index] = channel.Value();
    }

    return Result<void>::Success();
}

// 공통 필드와 에셋 기본값 하나다.
Result<bool> AssetParameterValue(NodePropertyReader& properties, MaterialParameterType type,
                                 StringView field, MaterialParameterMetadata& out) noexcept {
    if (auto common = ReadCommon(properties, type, out); !common) {
        return Result<bool>::Failure(common.Error());
    }

    auto asset = ReadAsset(properties, field);
    if (!asset) {
        return Result<bool>::Failure(asset.Error());
    }

    out.texture = asset.Value();
    return Result<bool>::Success(true);
}

} // namespace

Result<bool> ScalarParameterValue(NodePropertyReader& properties,
                                  MaterialParameterMetadata& out) noexcept {
    if (auto common = ReadCommon(properties, MaterialParameterType::Scalar, out); !common) {
        return Result<bool>::Failure(common.Error());
    }

    auto value = properties.ReadFloat("defaultValue", 0.0f);
    if (!value) {
        return Result<bool>::Failure(value.Error());
    }

    out.defaults[0] = value.Value();
    if (auto data = ReadPrimitiveData(properties, out); !data) {
        return Result<bool>::Failure(data.Error());
    }

    return Result<bool>::Success(true);
}

Result<bool> VectorParameterValue(NodePropertyReader& properties,
                                  MaterialParameterMetadata& out) noexcept {
    if (auto common = ReadCommon(properties, MaterialParameterType::Vector, out); !common) {
        return Result<bool>::Failure(common.Error());
    }

    if (auto color = ReadColor(properties, "defaultValue", ZeroColor, out); !color) {
        return Result<bool>::Failure(color.Error());
    }

    if (auto data = ReadPrimitiveData(properties, out); !data) {
        return Result<bool>::Failure(data.Error());
    }

    return Result<bool>::Success(true);
}

Result<bool> ChannelMaskParameterValue(NodePropertyReader& properties,
                                       MaterialParameterMetadata& out) noexcept {
    if (auto common = ReadCommon(properties, MaterialParameterType::Vector, out); !common) {
        return Result<bool>::Failure(common.Error());
    }

    if (auto color = ReadColor(properties, "defaultValue", RedMaskColor, out); !color) {
        return Result<bool>::Failure(color.Error());
    }

    // UE ChannelMaskParameter::GetParameterValue는 VectorParameter의 것을 불러 custom primitive
    // data 번호를 채운다(번역이 그 값을 쓰지 않는 것과 별개다).
    if (auto data = ReadPrimitiveData(properties, out); !data) {
        return Result<bool>::Failure(data.Error());
    }

    return Result<bool>::Success(true);
}

Result<bool> DoubleVectorParameterValue(NodePropertyReader& properties,
                                        MaterialParameterMetadata& out) noexcept {
    if (auto common = ReadCommon(properties, MaterialParameterType::DoubleVector, out); !common) {
        return Result<bool>::Failure(common.Error());
    }

    auto vector = properties.OpenStruct("defaultValue", GeneratedType<Vec4d>::Schema());
    if (!vector) {
        return Result<bool>::Failure(vector.Error());
    }

    static constexpr StringView Axes[MaterialParameterComponents] = {"x", "y", "z", "w"};
    for (usize index = 0; index < MaterialParameterComponents; ++index) {
        auto axis = vector.Value().ReadDouble(Axes[index], 0.0);
        if (!axis) {
            return Result<bool>::Failure(axis.Error());
        }

        out.defaults[index] = axis.Value();
    }

    return Result<bool>::Success(true);
}

Result<bool> StaticSwitchParameterValue(NodePropertyReader& properties,
                                        MaterialParameterMetadata& out) noexcept {
    if (auto common = ReadCommon(properties, MaterialParameterType::StaticSwitch, out); !common) {
        return Result<bool>::Failure(common.Error());
    }

    auto value = properties.ReadBool("defaultValue", false);
    if (!value) {
        return Result<bool>::Failure(value.Error());
    }

    out.defaults[0] = value.Value() ? 1.0 : 0.0;
    return Result<bool>::Success(true);
}

Result<bool> StaticComponentMaskParameterValue(NodePropertyReader& properties,
                                               MaterialParameterMetadata& out) noexcept {
    if (auto common = ReadCommon(properties, MaterialParameterType::StaticComponentMask, out);
        !common) {
        return Result<bool>::Failure(common.Error());
    }

    static constexpr StringView Channels[MaterialParameterComponents] = {"defaultR", "defaultG",
                                                                         "defaultB", "defaultA"};
    for (usize index = 0; index < MaterialParameterComponents; ++index) {
        auto channel = properties.ReadBool(Channels[index], false);
        if (!channel) {
            return Result<bool>::Failure(channel.Error());
        }

        out.defaults[index] = channel.Value() ? 1.0 : 0.0;
    }

    return Result<bool>::Success(true);
}

Result<bool> CurveAtlasRowParameterValue(NodePropertyReader& properties,
                                         MaterialParameterMetadata& out) noexcept {
    if (auto common = ReadCommon(properties, MaterialParameterType::Scalar, out); !common) {
        return Result<bool>::Failure(common.Error());
    }

    auto curve = ReadAsset(properties, "curve");
    if (!curve) {
        return Result<bool>::Failure(curve.Error());
    }

    auto atlas = ReadAsset(properties, "atlas");
    if (!atlas) {
        return Result<bool>::Failure(atlas.Error());
    }

    // UE: atlas와 커브가 둘 다 있을 때만 atlas 위치다(값은 행 번호 — ResolveParameterDefaults).
    // custom primitive data 번호는 채우지 않는다(UE CurveAtlasRowParameter::GetParameterValue).
    if (!(curve.Value() == Guid{}) && !(atlas.Value() == Guid{})) {
        out.atlasCurve = curve.Value();
        out.atlas = atlas.Value();
        out.usedAsAtlasPosition = true;
    }

    return Result<bool>::Success(true);
}

Result<bool> TextureParameterValue(NodePropertyReader& properties,
                                   MaterialParameterMetadata& out) noexcept {
    return AssetParameterValue(properties, MaterialParameterType::Texture, "texture", out);
}

Result<bool> TextureCollectionParameterValue(NodePropertyReader& properties,
                                             MaterialParameterMetadata& out) noexcept {
    return AssetParameterValue(properties, MaterialParameterType::TextureCollection,
                               "textureCollection", out);
}

Result<bool> SparseVolumeTextureParameterValue(NodePropertyReader& properties,
                                               MaterialParameterMetadata& out) noexcept {
    return AssetParameterValue(properties, MaterialParameterType::SparseVolumeTexture,
                               "sparseVolumeTexture", out);
}

Result<bool> RuntimeVirtualTextureParameterValue(NodePropertyReader& properties,
                                                 MaterialParameterMetadata& out) noexcept {
    return AssetParameterValue(properties, MaterialParameterType::RuntimeVirtualTexture,
                               "virtualTexture", out);
}

Result<bool> FontParameterValue(NodePropertyReader& properties,
                                MaterialParameterMetadata& out) noexcept {
    return AssetParameterValue(properties, MaterialParameterType::Font, "font", out);
}

} // namespace rocks::material::detail
