#pragma once

#include <Base/Types.h>

// 머티리얼 IR 값 타입이다(Docs/Design/MaterialSystem 6.2·6.8절). 수 타입은 스칼라 종류와 성분
// 수(1–4)이고, 행렬은 float 3×3·4×4뿐이다(5.3절). 핀 타입 Numeric·Any는 IR 타입이 아니다:
// lowering이 연결된 값으로 정한다. 자원·표면·속성 집합 타입은 그 단계(MS2c·MS2d·MS2e)가 더한다.
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
    Double = 4
};

struct IrType {
    IrScalar scalar = IrScalar::Float;
    // 벡터 성분 수(1–4)다. 행렬이면 열 수다.
    uint8 components = 1;
    // 1이면 스칼라·벡터, 3·4면 행렬 행 수다.
    uint8 rows = 1;

    constexpr bool IsScalar() const noexcept {
        return rows == 1 && components == 1;
    }

    constexpr bool IsMatrix() const noexcept {
        return rows > 1;
    }

    friend constexpr bool operator==(const IrType&, const IrType&) noexcept = default;
};

constexpr IrType IrFloat(uint8 components) noexcept {
    return IrType{IrScalar::Float, components, 1};
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
    if (static_cast<uint8>(type.scalar) > static_cast<uint8>(IrScalar::Double)) {
        return false;
    }

    if (type.rows == 1) {
        return type.components >= 1 && type.components <= MaxIrComponents;
    }

    return type.scalar == IrScalar::Float && type.rows == type.components &&
           (type.rows == 3 || type.rows == 4);
}

// 같은 스칼라 종류의 성분 수만 바꾼 벡터 타입이다.
constexpr IrType WithComponents(IrType type, uint8 components) noexcept {
    return IrType{type.scalar, components, 1};
}

} // namespace rocks::material
