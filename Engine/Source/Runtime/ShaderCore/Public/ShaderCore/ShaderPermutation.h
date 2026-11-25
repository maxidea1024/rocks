#pragma once

#include <Base/Types.h>
#include <Core/Containers/Span.h>
#include <Core/Result.h>
#include <Core/Text/StringView.h>

// 셰이더 퍼뮤테이션 도메인이다(Docs/Design/ShaderSystem 12.2.1절 SSD4, UE ShaderPermutation.h
// TShaderPermutationDomain 대응).
//
// 도메인은 차원 목록이다. 차원마다 전처리 정의 하나(defineName)로 원문에 들어가고, 차원 값마다
// value id(0부터 count - 1)가 있다:
// - Bool: value id = 정의 값(0·1). UE SHADER_PERMUTATION_BOOL.
// - IntRange: 값 first … first + count - 1, value id = 값 - first. UE SHADER_PERMUTATION_RANGE_INT.
// - SparseInt: 값 목록 values. UE와 같게 목록의 **첫 값이 가장 큰 value id**다(values[i]의 value id
// =
//   count - 1 - i, UE ShaderPermutation.h:208–214의 재귀 정의). UE SHADER_PERMUTATION_SPARSE_INT.
// - Enum: 값 이름 목록 enumNames, value id = 순번 = 정의 값. UE SHADER_PERMUTATION_ENUM_CLASS.
//
// 퍼뮤테이션 ID는 UE와 같은 혼합 기수다: 선언 첫 차원이 가장 낮은 자리(ID = 첫 차원 value id + 첫
// 차원 수 × 나머지 ID). 차원을 선언 끝에 더하면(새 차원 value id 0 = 기본값) 기존 ID가 보존된다.
// 도메인 크기(차원 수의 곱)는 MaxShaderPermutations를 넘을 수 없다.
//
// 사용 예:
//   constexpr int32 SampleCounts[] = {8, 4, 1};      // value id: 8 → 2, 4 → 1, 1 → 0
//   constexpr ShaderPermutationDimension Dimensions[] = {
//       ShaderPermutationDimension::Bool("USE_FOG"),
//       ShaderPermutationDimension::SparseInt("SAMPLES", SampleCounts)};
//   constexpr ShaderPermutationDomain Domain{Dimensions};
//   uint32 values[2] = {1, 2};                       // USE_FOG = 1, SAMPLES = 8
//   const Result<uint32> id = EncodeShaderPermutation(Domain, values);   // 1 + 2 × 2 = 5
namespace rocks::shader {

// 도메인 크기 상한이다(넘으면 등록 오류 — 조합 폭발을 등록 때 잡는다. UE에는 상한이 없다).
inline constexpr uint32 MaxShaderPermutations = 65'536;
// 도메인 차원 수 상한이다(bool 차원만으로 상한을 채우는 수).
inline constexpr usize MaxShaderPermutationDimensions = 16;
// 정의 이름·열거 값 이름의 최대 byte 수(NUL 제외)다.
inline constexpr usize MaxShaderPermutationNameBytes = 64;

enum class ShaderPermutationKind : uint8 {
    Bool,
    IntRange,
    SparseInt,
    Enum
};

// 차원 하나다(읽기 전용 값 — 가리키는 목록은 정적 수명이어야 한다). 아래 생성 함수로 만든다.
struct ShaderPermutationDimension {
    StringView defineName;
    ShaderPermutationKind kind = ShaderPermutationKind::Bool;
    // IntRange의 첫 값과 값 수다.
    int32 first = 0;
    uint32 count = 0;
    // SparseInt의 값 목록이다.
    Span<const int32> values;
    // Enum의 값 이름 목록이다.
    Span<const StringView> enumNames;
    // 이 차원이 기본값(value id 0)이 아닌 퍼뮤테이션은 개발 전용이다(7.2절 12항, SSD4).
    bool developmentOnly = false;

    static constexpr ShaderPermutationDimension Bool(StringView name,
                                                     bool developmentOnly = false) noexcept {
        ShaderPermutationDimension dimension;
        dimension.defineName = name;
        dimension.kind = ShaderPermutationKind::Bool;
        dimension.developmentOnly = developmentOnly;
        return dimension;
    }

    static constexpr ShaderPermutationDimension IntRange(StringView name, int32 first,
                                                         uint32 count) noexcept {
        ShaderPermutationDimension dimension;
        dimension.defineName = name;
        dimension.kind = ShaderPermutationKind::IntRange;
        dimension.first = first;
        dimension.count = count;
        return dimension;
    }

    static constexpr ShaderPermutationDimension SparseInt(StringView name,
                                                          Span<const int32> values) noexcept {
        ShaderPermutationDimension dimension;
        dimension.defineName = name;
        dimension.kind = ShaderPermutationKind::SparseInt;
        dimension.values = values;
        return dimension;
    }

    static constexpr ShaderPermutationDimension Enum(StringView name,
                                                     Span<const StringView> names) noexcept {
        ShaderPermutationDimension dimension;
        dimension.defineName = name;
        dimension.kind = ShaderPermutationKind::Enum;
        dimension.enumNames = names;
        return dimension;
    }
};

// 도메인이다. 차원이 없으면 퍼뮤테이션 하나(ID 0)다.
struct ShaderPermutationDomain {
    Span<const ShaderPermutationDimension> dimensions;
};

// 차원의 값 수다. 차원 정의가 잘못되었으면(IntRange count 0, 빈 목록) 0이다.
ROCKS_SHADERCORE_API uint32
ShaderPermutationValueCount(const ShaderPermutationDimension& dimension) noexcept;

// 도메인을 검사하고 크기(퍼뮤테이션 수)를 돌려준다. 오류(InvalidArgument): 차원 수가
// MaxShaderPermutationDimensions 초과, 정의 이름이 식별자가 아니거나 ROCKS_·__ 접두(SC10), 정의
// 이름 중복, 값 수 0, SparseInt 값 중복, Enum 이름이 식별자가 아니거나 중복, IntRange 값이 int32
// 범위 밖, 크기가 MaxShaderPermutations 초과.
ROCKS_SHADERCORE_API Result<uint32>
ValidateShaderPermutationDomain(const ShaderPermutationDomain& domain) noexcept;

// 차원 value id 목록(선언 순서)을 퍼뮤테이션 ID로 바꾼다. 오류: 목록 길이가 차원 수와 다름·value
// id가 범위 밖(InvalidArgument). 도메인은 ValidateShaderPermutationDomain을 통과해야 한다.
ROCKS_SHADERCORE_API Result<uint32> EncodeShaderPermutation(const ShaderPermutationDomain& domain,
                                                            Span<const uint32> valueIds) noexcept;

// 퍼뮤테이션 ID를 차원 value id 목록으로 바꾼다(EncodeShaderPermutation의 역함수). 오류: 목록
// 길이가 차원 수와 다름·ID가 도메인 크기 이상(InvalidArgument).
ROCKS_SHADERCORE_API Result<void> DecodeShaderPermutation(const ShaderPermutationDomain& domain,
                                                          uint32 permutationId,
                                                          Span<uint32> valueIds) noexcept;

// 차원 value id의 정의 값(원문에 들어가는 정수)이다. value id가 범위 밖이면 InvalidArgument다.
ROCKS_SHADERCORE_API Result<int32>
ShaderPermutationDefineValue(const ShaderPermutationDimension& dimension, uint32 valueId) noexcept;

// 정의 값의 value id다(ShaderPermutationDefineValue의 역함수). 없는 값이면 NotFound다.
ROCKS_SHADERCORE_API Result<uint32>
ShaderPermutationValueId(const ShaderPermutationDimension& dimension, int32 defineValue) noexcept;

// 퍼뮤테이션이 개발 전용인가(developmentOnly 차원 가운데 value id가 0이 아닌 것이 있다). ID가 범위
// 밖이면 false다.
ROCKS_SHADERCORE_API bool IsDevelopmentOnlyShaderPermutation(const ShaderPermutationDomain& domain,
                                                             uint32 permutationId) noexcept;

} // namespace rocks::shader
