#pragma once

#include <Base/Types.h>

// 머티리얼 IR 값 타입이다(Docs/Design/MaterialSystem 6.2·6.8절). 수 타입은 스칼라 종류와 성분
// 수(1–4)이고, 행렬은 float 3×3·4×4뿐이다(5.3절). 핀 타입 Numeric·Any는 IR 타입이 아니다:
// lowering이 연결된 값으로 정한다. 텍스처는 스칼라 종류 Texture와 텍스처 종류 칸이다(6.10절 MS2c3 —
// 성분 1, 산술·비교·선택에 들어가지 못한다). 표면·속성 집합 타입은 그 단계(MS2d·MS2e)가 더한다.
//
// 사용 예:
//   const IrType color = IrFloat(3);
//   if (color.components == 3 && !color.IsMatrix()) { ... }
namespace rocks::material {

// IR 벡터 성분의 최대 수다.
inline constexpr uint8 MaxIrComponents = 4;

// 스칼라 종류다. Double은 큰 좌표 값(LWC — 5.3절 Double1–4)이다.
enum class IrScalar : uint8 {
    Float = 0,
    Int = 1,
    UInt = 2,
    Bool = 3,
    Double = 4,
    // 텍스처 자원이다(종류는 IrType::texture).
    Texture = 5,
    // 표면 값이다(번역기 표면 기록을 가리킨다 — 6.11절 MS2d1). 산술·변환·선택에 들어가지 못한다.
    Surface = 6,
    // 속성 집합 값이다(번역기 속성 기록을 가리킨다 — 6.12절 MS2e1). 산술·변환에 들어가지 못한다.
    Attributes = 7
};

// 텍스처 자원 종류다(UE MCT_Texture2D·TextureCube·Texture2DArray·TextureCubeArray·VolumeTexture·
// TextureExternal).
enum class IrTextureKind : uint8 {
    None = 0,
    Texture2D = 1,
    TextureCube = 2,
    Texture2DArray = 3,
    TextureCubeArray = 4,
    Texture3D = 5,
    External = 6
};

struct IrType {
    IrScalar scalar = IrScalar::Float;
    // 벡터 성분 수(1–4)다. 행렬이면 열 수다.
    uint8 components = 1;
    // 1이면 스칼라·벡터, 3·4면 행렬 행 수다.
    uint8 rows = 1;
    // scalar가 Texture일 때만 None이 아니다.
    IrTextureKind texture = IrTextureKind::None;

    constexpr bool IsScalar() const noexcept {
        return rows == 1 && components == 1;
    }

    constexpr bool IsMatrix() const noexcept {
        return rows > 1;
    }

    constexpr bool IsTexture() const noexcept {
        return scalar == IrScalar::Texture;
    }

    constexpr bool IsSurface() const noexcept {
        return scalar == IrScalar::Surface;
    }

    constexpr bool IsAttributes() const noexcept {
        return scalar == IrScalar::Attributes;
    }

    // 수가 아닌 값(텍스처·표면·속성 집합)인가다.
    constexpr bool IsOpaque() const noexcept {
        return scalar == IrScalar::Texture || scalar == IrScalar::Surface ||
               scalar == IrScalar::Attributes;
    }

    friend constexpr bool operator==(const IrType&, const IrType&) noexcept = default;
};

constexpr IrType IrFloat(uint8 components) noexcept {
    return IrType{IrScalar::Float, components, 1};
}

constexpr IrType IrTextureType(IrTextureKind kind) noexcept {
    return IrType{IrScalar::Texture, 1, 1, kind};
}

constexpr IrType IrSurfaceType() noexcept {
    return IrType{IrScalar::Surface, 1, 1, IrTextureKind::None};
}

constexpr IrType IrAttributesType() noexcept {
    return IrType{IrScalar::Attributes, 1, 1, IrTextureKind::None};
}

constexpr IrType IrInt(uint8 components) noexcept {
    return IrType{IrScalar::Int, components, 1};
}

constexpr IrType IrUInt(uint8 components) noexcept {
    return IrType{IrScalar::UInt, components, 1};
}

constexpr IrType IrBool(uint8 components) noexcept {
    return IrType{IrScalar::Bool, components, 1};
}

constexpr IrType IrDouble(uint8 components) noexcept {
    return IrType{IrScalar::Double, components, 1};
}

// float rows×columns 행렬이다(3×3·4×4).
constexpr IrType IrFloatMatrix(uint8 rows, uint8 columns) noexcept {
    return IrType{IrScalar::Float, columns, rows};
}

// 정의된 타입인가: 벡터는 성분 1–4, 행렬은 float 3×3·4×4다.
constexpr bool IsValidIrType(IrType type) noexcept {
    if (type.scalar == IrScalar::Texture) {
        return type.components == 1 && type.rows == 1 && type.texture != IrTextureKind::None &&
               static_cast<uint8>(type.texture) <= static_cast<uint8>(IrTextureKind::External);
    }

    if (type.scalar == IrScalar::Surface || type.scalar == IrScalar::Attributes) {
        return type.components == 1 && type.rows == 1 && type.texture == IrTextureKind::None;
    }

    if (static_cast<uint8>(type.scalar) > static_cast<uint8>(IrScalar::Double) ||
        type.texture != IrTextureKind::None) {
        return false;
    }

    if (type.rows == 1) {
        return type.components >= 1 && type.components <= MaxIrComponents;
    }

    return type.scalar == IrScalar::Float && type.rows == type.components &&
           (type.rows == 3 || type.rows == 4);
}

// 같은 스칼라 종류의 성분 수만 바꾼 벡터 타입이다.
// Double 값(LWC) tile 한 칸의 크기(cm)다(MaterialDouble.rsh MaterialDoubleTileSize — UE
// UE_LWC_RENDER_TILE_SIZE, Core/Public/Misc/LargeWorldRenderPosition.h:14).
inline constexpr double IrDoubleTileSize = 2097152.0;

// 텍스처 종류의 uv 폭이다(UE TextureSample의 ForceCast 대상 — CubeArray 4, Cube·2DArray·3D 3,
// 그 밖 2). None이면 0이다.
constexpr uint8 IrTextureUvComponents(IrTextureKind kind) noexcept {
    switch (kind) {
    case IrTextureKind::None:
        return 0;
    case IrTextureKind::TextureCubeArray:
        return 4;
    case IrTextureKind::TextureCube:
    case IrTextureKind::Texture2DArray:
    case IrTextureKind::Texture3D:
        return 3;
    default:
        return 2;
    }
}

// 명시 미분(SampleGrad) 폭이다(HLSL 서명 — 2D·2DArray·External 2, Cube·CubeArray·3D 3).
constexpr uint8 IrTextureGradientComponents(IrTextureKind kind) noexcept {
    switch (kind) {
    case IrTextureKind::None:
        return 0;
    case IrTextureKind::TextureCube:
    case IrTextureKind::TextureCubeArray:
    case IrTextureKind::Texture3D:
        return 3;
    default:
        return 2;
    }
}

constexpr IrType WithComponents(IrType type, uint8 components) noexcept {
    return IrType{type.scalar, components, 1};
}

} // namespace rocks::material
