#include <ShaderCore/ShaderPermutation.h>

#include "ShaderCoreInternal.h"

#include <Base/NumericLimits.h>

// 셰이더 퍼뮤테이션 도메인(Docs/Design/ShaderSystem 12.2.1절 SSD4)이다. 규칙은 공개 헤더 주석을
// 따른다.
namespace rocks::shader {

namespace permutation_detail {

constexpr StringView ReservedPrefix("ROCKS_");
constexpr StringView CompilerReservedPrefix("__");

bool IsIdentifier(StringView name) noexcept {
    return core_detail::IsShaderIdentifier(name, MaxShaderPermutationNameBytes);
}

bool IsValidDefineName(StringView name) noexcept {
    return IsIdentifier(name) &&
           !name.StartsWith(ReservedPrefix, StringComparison::CaseSensitive) &&
           !name.StartsWith(CompilerReservedPrefix, StringComparison::CaseSensitive);
}

// 차원 하나의 내용(이름 밖)을 검사한다.
bool IsValidDimension(const ShaderPermutationDimension& dimension) noexcept {
    switch (dimension.kind) {
    case ShaderPermutationKind::Bool:
        return true;
    case ShaderPermutationKind::IntRange:
        return dimension.count > 0 &&
               static_cast<int64>(dimension.first) + static_cast<int64>(dimension.count) - 1 <=
                   static_cast<int64>(NumericLimits<int32>::Max());
    case ShaderPermutationKind::SparseInt:
        for (usize index = 0; index < dimension.values.Size(); ++index) {
            for (usize other = 0; other < index; ++other) {
                if (dimension.values[other] == dimension.values[index]) {
                    return false;
                }
            }
        }

        return !dimension.values.IsEmpty();
    case ShaderPermutationKind::Enum:
        for (usize index = 0; index < dimension.enumNames.Size(); ++index) {
            if (!IsIdentifier(dimension.enumNames[index])) {
                return false;
            }

            for (usize other = 0; other < index; ++other) {
                if (dimension.enumNames[other] == dimension.enumNames[index]) {
                    return false;
                }
            }
        }

        return !dimension.enumNames.IsEmpty();
    }

    return false;
}

} // namespace permutation_detail

namespace core_detail {

bool IsShaderIdentifier(StringView name, usize maxBytes) noexcept {
    if (name.IsEmpty() || name.ByteCount() > maxBytes) {
        return false;
    }

    for (usize index = 0; index < name.ByteCount(); ++index) {
        const char value = name.Data()[index];
        const bool letter =
            (value >= 'a' && value <= 'z') || (value >= 'A' && value <= 'Z') || value == '_';
        const bool digit = value >= '0' && value <= '9';
        if (!letter && !(digit && index > 0)) {
            return false;
        }
    }

    return true;
}

} // namespace core_detail

uint32 ShaderPermutationValueCount(const ShaderPermutationDimension& dimension) noexcept {
    switch (dimension.kind) {
    case ShaderPermutationKind::Bool:
        return 2;
    case ShaderPermutationKind::IntRange:
        return dimension.count;
    case ShaderPermutationKind::SparseInt:
        return dimension.values.Size() > MaxShaderPermutations
                   ? 0
                   : static_cast<uint32>(dimension.values.Size());
    case ShaderPermutationKind::Enum:
        return dimension.enumNames.Size() > MaxShaderPermutations
                   ? 0
                   : static_cast<uint32>(dimension.enumNames.Size());
    }

    return 0;
}

Result<uint32> ValidateShaderPermutationDomain(const ShaderPermutationDomain& domain) noexcept {
    using namespace permutation_detail;
    const Span<const ShaderPermutationDimension> dimensions = domain.dimensions;
    if (dimensions.Size() > MaxShaderPermutationDimensions) {
        return Result<uint32>::Failure(ErrorCode::InvalidArgument);
    }

    uint64 size = 1;
    for (usize index = 0; index < dimensions.Size(); ++index) {
        const ShaderPermutationDimension& dimension = dimensions[index];
        if (!IsValidDefineName(dimension.defineName) || !IsValidDimension(dimension)) {
            return Result<uint32>::Failure(ErrorCode::InvalidArgument);
        }

        for (usize other = 0; other < index; ++other) {
            if (dimensions[other].defineName == dimension.defineName) {
                return Result<uint32>::Failure(ErrorCode::InvalidArgument);
            }
        }

        const uint32 count = ShaderPermutationValueCount(dimension);
        if (count == 0) {
            return Result<uint32>::Failure(ErrorCode::InvalidArgument);
        }

        size *= count;
        if (size > MaxShaderPermutations) {
            return Result<uint32>::Failure(ErrorCode::InvalidArgument);
        }
    }

    return Result<uint32>::Success(static_cast<uint32>(size));
}

Result<uint32> EncodeShaderPermutation(const ShaderPermutationDomain& domain,
                                       Span<const uint32> valueIds) noexcept {
    const Span<const ShaderPermutationDimension> dimensions = domain.dimensions;
    if (valueIds.Size() != dimensions.Size()) {
        return Result<uint32>::Failure(ErrorCode::InvalidArgument);
    }

    // 마지막 차원부터 쌓는다: ID = v0 + n0 × (v1 + n1 × (…)).
    uint64 id = 0;
    for (usize index = dimensions.Size(); index > 0; --index) {
        const uint32 count = ShaderPermutationValueCount(dimensions[index - 1]);
        const uint32 value = valueIds[index - 1];
        if (count == 0 || value >= count) {
            return Result<uint32>::Failure(ErrorCode::InvalidArgument);
        }

        id = id * count + value;
        if (id >= MaxShaderPermutations) {
            return Result<uint32>::Failure(ErrorCode::InvalidArgument);
        }
    }

    return Result<uint32>::Success(static_cast<uint32>(id));
}

Result<void> DecodeShaderPermutation(const ShaderPermutationDomain& domain, uint32 permutationId,
                                     Span<uint32> valueIds) noexcept {
    const Span<const ShaderPermutationDimension> dimensions = domain.dimensions;
    if (valueIds.Size() != dimensions.Size()) {
        return Result<void>::Failure(ErrorCode::InvalidArgument);
    }

    uint32 rest = permutationId;
    for (usize index = 0; index < dimensions.Size(); ++index) {
        const uint32 count = ShaderPermutationValueCount(dimensions[index]);
        if (count == 0) {
            return Result<void>::Failure(ErrorCode::InvalidArgument);
        }

        valueIds[index] = rest % count;
        rest /= count;
    }

    // 남은 자리가 있으면 ID가 도메인 크기 이상이다.
    if (rest != 0) {
        return Result<void>::Failure(ErrorCode::InvalidArgument);
    }

    return Result<void>::Success();
}

Result<int32> ShaderPermutationDefineValue(const ShaderPermutationDimension& dimension,
                                           uint32 valueId) noexcept {
    const uint32 count = ShaderPermutationValueCount(dimension);
    if (valueId >= count) {
        return Result<int32>::Failure(ErrorCode::InvalidArgument);
    }

    switch (dimension.kind) {
    case ShaderPermutationKind::Bool:
    case ShaderPermutationKind::Enum:
        return Result<int32>::Success(static_cast<int32>(valueId));
    case ShaderPermutationKind::IntRange:
        return Result<int32>::Success(
            static_cast<int32>(static_cast<int64>(dimension.first) + valueId));
    case ShaderPermutationKind::SparseInt:
        // 첫 값이 가장 큰 value id다(UE와 같다).
        return Result<int32>::Success(dimension.values[count - 1 - valueId]);
    }

    return Result<int32>::Failure(ErrorCode::InvalidArgument);
}

Result<uint32> ShaderPermutationValueId(const ShaderPermutationDimension& dimension,
                                        int32 defineValue) noexcept {
    const uint32 count = ShaderPermutationValueCount(dimension);
    switch (dimension.kind) {
    case ShaderPermutationKind::Bool:
    case ShaderPermutationKind::Enum:
        if (defineValue >= 0 && static_cast<uint32>(defineValue) < count) {
            return Result<uint32>::Success(static_cast<uint32>(defineValue));
        }

        break;
    case ShaderPermutationKind::IntRange: {
        const int64 offset = static_cast<int64>(defineValue) - dimension.first;
        if (offset >= 0 && offset < static_cast<int64>(count)) {
            return Result<uint32>::Success(static_cast<uint32>(offset));
        }

        break;
    }
    case ShaderPermutationKind::SparseInt:
        for (uint32 index = 0; index < count; ++index) {
            if (dimension.values[index] == defineValue) {
                return Result<uint32>::Success(count - 1 - index);
            }
        }

        break;
    }

    return Result<uint32>::Failure(ErrorCode::NotFound);
}

bool IsDevelopmentOnlyShaderPermutation(const ShaderPermutationDomain& domain,
                                        uint32 permutationId) noexcept {
    uint32 valueIds[MaxShaderPermutationDimensions] = {};
    if (domain.dimensions.Size() > MaxShaderPermutationDimensions ||
        !DecodeShaderPermutation(domain, permutationId,
                                 Span<uint32>(valueIds, domain.dimensions.Size()))) {
        return false;
    }

    for (usize index = 0; index < domain.dimensions.Size(); ++index) {
        if (domain.dimensions[index].developmentOnly && valueIds[index] != 0) {
            return true;
        }
    }

    return false;
}

} // namespace rocks::shader
