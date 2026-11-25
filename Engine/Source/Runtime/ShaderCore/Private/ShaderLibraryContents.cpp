#include <ShaderCore/ShaderLibraryContents.h>

#include <ShaderCore/ShaderPermutation.h>

// 라이브러리 조합 열거(Docs/Design/ShaderSystem 12.2.1절 SSD5·SSD6)다. 규칙은 공개 헤더 주석을
// 따른다.
namespace rocks::shader {

Result<void> EnumerateShaderLibraryCombinations(Span<const ShaderTypeDesc* const> types,
                                                Span<const ShaderVertexInputType> inputs,
                                                const ShaderFormatFacts& facts,
                                                ShaderLibraryKind library, bool includeDevelopment,
                                                Array<ShaderLibraryCombination>& out) noexcept {
    out.Clear();
    for (const ShaderTypeDesc* type : types) {
        if (ShaderLibraryKindOf(type->owner) != library ||
            (type->requiredFeatures & ~facts.features) != 0) {
            continue;
        }

        const bool typeDevelopment = (type->flags & ShaderTypeDevelopmentOnly) != 0;
        if (typeDevelopment && !includeDevelopment) {
            continue;
        }

        const Result<uint32> size = ValidateShaderPermutationDomain(type->permutation);
        if (!size) {
            return Result<void>::Failure(size.Error());
        }

        const bool axis = (type->flags & ShaderTypeVertexInputAxis) != 0;
        const usize inputCount = axis ? inputs.Size() : 1;
        for (usize input = 0; input < inputCount; ++input) {
            const VertexInputTypeId vertexInput = axis ? inputs[input].id : NoVertexInputType;
            for (uint32 permutation = 0; permutation < size.Value(); ++permutation) {
                const bool development = typeDevelopment || IsDevelopmentOnlyShaderPermutation(
                                                                type->permutation, permutation);
                if (development && !includeDevelopment) {
                    continue;
                }

                // 개발 전용 PS 퍼뮤테이션은 진단 버퍼 기능을 요구한다(16.1절 SSD34).
                if (development && type->stage == ShaderStagePixel &&
                    (facts.features & ShaderFeatureDebugPixelUav) == 0) {
                    continue;
                }

                if (type->shouldCompile != nullptr &&
                    !type->shouldCompile(vertexInput, permutation, facts)) {
                    continue;
                }

                if (axis && inputs[input].shouldCompile != nullptr &&
                    !inputs[input].shouldCompile(*type, permutation, facts)) {
                    continue;
                }

                if (!out.TryPushBack(
                        ShaderLibraryCombination{type, vertexInput, permutation, development})) {
                    return Result<void>::Failure(ErrorCode::OutOfMemory);
                }
            }
        }
    }

    return Result<void>::Success();
}

} // namespace rocks::shader
