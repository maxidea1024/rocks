#include <ShaderCore/ShaderLibraryContents.h>

#include <ShaderCore/ShaderPermutation.h>

// 라이브러리 조합 열거(Docs/Design/ShaderSystem 12.2.1절 SSD5·SSD6, 16.5절 SSD88)다. 규칙은 공개
// 헤더 주석을 따른다.
namespace rocks::shader {

namespace contents_detail {

// 머티리얼 묶음의 거르기 입력이다(엔진 셰이더 열거는 null).
struct MaterialFilter {
    Span<const uint8> traits;
    uint64 bundleId = EngineShaderBundleId;
};

// 타입 하나의 조합을 inputs(정점 입력 축일 때)마다 out 뒤에 더한다(SSD5 결합 거르기).
Result<void> AppendTypeCombinations(const ShaderTypeDesc& type,
                                    Span<const ShaderVertexInputType> inputs,
                                    const ShaderFormatFacts& facts, bool includeDevelopment,
                                    const MaterialFilter* material,
                                    Array<ShaderLibraryCombination>& out) noexcept {
    if ((type.requiredFeatures & ~facts.features) != 0) {
        return Result<void>::Success();
    }

    const bool typeDevelopment = (type.flags & ShaderTypeDevelopmentOnly) != 0;
    if (typeDevelopment && !includeDevelopment) {
        return Result<void>::Success();
    }

    const Result<uint32> size = ValidateShaderPermutationDomain(type.permutation);
    if (!size) {
        return Result<void>::Failure(size.Error());
    }

    const bool axis = (type.flags & ShaderTypeVertexInputAxis) != 0;
    const usize inputCount = axis ? inputs.Size() : 1;
    for (usize input = 0; input < inputCount; ++input) {
        const VertexInputTypeId vertexInput = axis ? inputs[input].id : NoVertexInputType;
        for (uint32 permutation = 0; permutation < size.Value(); ++permutation) {
            const bool development = typeDevelopment || IsDevelopmentOnlyShaderPermutation(
                                                            type.permutation, permutation);
            if (development && !includeDevelopment) {
                continue;
            }

            // 개발 전용 PS 퍼뮤테이션은 진단 버퍼 기능을 요구한다(16.1절 SSD34).
            if (development && type.stage == ShaderStagePixel &&
                (facts.features & ShaderFeatureDebugPixelUav) == 0) {
                continue;
            }

            if (material == nullptr) {
                if (type.shouldCompile != nullptr &&
                    !type.shouldCompile(vertexInput, permutation, facts)) {
                    continue;
                }
            } else if (type.materialShouldCompile != nullptr &&
                       !type.materialShouldCompile(material->traits, vertexInput, permutation,
                                                   facts)) {
                continue;
            }

            if (axis && inputs[input].shouldCompile != nullptr &&
                !inputs[input].shouldCompile(type, permutation, facts)) {
                continue;
            }

            ShaderLibraryCombination combination{&type, EngineShaderBundleId, vertexInput,
                                                 permutation, development};
            if (material != nullptr) {
                combination.bundleId = material->bundleId;
            }

            if (!out.TryPushBack(combination)) {
                return Result<void>::Failure(ErrorCode::OutOfMemory);
            }
        }
    }

    return Result<void>::Success();
}

} // namespace contents_detail

Result<void> EnumerateShaderLibraryCombinations(Span<const ShaderTypeDesc* const> types,
                                                Span<const ShaderVertexInputType> inputs,
                                                const ShaderFormatFacts& facts,
                                                ShaderLibraryKind library, bool includeDevelopment,
                                                Array<ShaderLibraryCombination>& out) noexcept {
    out.Clear();
    for (const ShaderTypeDesc* type : types) {
        // 머티리얼 셰이더는 묶음마다 곱한다(16.5절 SSD88).
        if ((type->flags & ShaderTypeMaterialShader) != 0 ||
            ShaderLibraryKindOf(EngineShaderBundleId, type->owner) != library) {
            continue;
        }

        if (auto appended = contents_detail::AppendTypeCombinations(
                *type, inputs, facts, includeDevelopment, nullptr, out);
            !appended) {
            return appended;
        }
    }

    return Result<void>::Success();
}

Result<void> EnumerateMaterialShaderCombinations(Span<const ShaderTypeDesc* const> types,
                                                 Span<const ShaderVertexInputType> inputs,
                                                 const ShaderFormatFacts& facts,
                                                 Span<const uint8> materialTraits, uint64 bundleId,
                                                 bool includeDevelopment,
                                                 Array<ShaderLibraryCombination>& out) noexcept {
    if (bundleId == EngineShaderBundleId) {
        return Result<void>::Failure(ErrorCode::InvalidArgument);
    }

    const contents_detail::MaterialFilter material{materialTraits, bundleId};
    for (const ShaderTypeDesc* type : types) {
        if ((type->flags & ShaderTypeMaterialShader) == 0) {
            continue;
        }

        if (auto appended = contents_detail::AppendTypeCombinations(
                *type, inputs, facts, includeDevelopment, &material, out);
            !appended) {
            return appended;
        }
    }

    return Result<void>::Success();
}

} // namespace rocks::shader
