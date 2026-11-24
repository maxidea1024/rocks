#include <ShaderFormatVulkan/SpirvRewrite.h>

// Graphics 개발 훅의 고쳐 쓰기 입구다(공용 구현은 ShaderCompilerCommon/SpirvRewrite.h).
namespace rocks::shader {

Result<void> RewriteSpirv(Span<const uint32> input, graphics::ShaderStage stage,
                          StringView rootConstantsName, Array<uint32>& output,
                          String& diagnostics) noexcept {
    SpirvRewriteOptions options;
    options.stage =
        stage == graphics::ShaderStage::Vertex
            ? ShaderStageVertex
            : (stage == graphics::ShaderStage::Compute ? ShaderStageCompute : ShaderStagePixel);
    options.rootConstantsName = rootConstantsName;
    return RewriteSpirv(input, options, output, diagnostics);
}

} // namespace rocks::shader
