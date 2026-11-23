#pragma once

#include <Base/Types.h>
#include <Core/Containers/Array.h>
#include <Core/Containers/Span.h>
#include <Core/Text/String.h>
#include <ShaderCompilerCommon/ShaderFormatDefinition.h>
#include <ShaderCompiler/ShaderPreprocessor.h>

// 전처리 정의 조립이다(Docs/Design/ShaderSystem 11.1절 SC10). 메인 프로세스가 job마다 형식
// 정의에서 정의를 만들고 job이 준 정의(퍼뮤테이션 — P14)를 더한다.
//
// 만드는 정의(값):
// - ROCKS_FORMAT_<형식 id> = 1
// - ROCKS_BACKEND_<D3D12|D3D11|VULKAN|METAL|GLES> = 1 (형식 정의 backend)
// - ROCKS_COMPILER_DXC 또는 ROCKS_COMPILER_FXC = 1 (형식 정의의 1차 경로 compiler.path. D3D11_SM5는
//   대체 경로를 타도 FXC 정의로 전처리한 같은 원문을 쓴다)
// - ROCKS_SHADER_MODEL_MAJOR·ROCKS_SHADER_MODEL_MINOR = compiler.shaderModel(내장
//   __SHADER_TARGET_* 대신)
// - ROCKS_HLSL2021 = 0·1 (job 옵션, 기본은 형식 정의 language.hlsl2021)
// - ROCKS_VERTEX_SHADER·ROCKS_PIXEL_SHADER·ROCKS_COMPUTE_SHADER 가운데 job 단계 하나 = 1
// - job 정의. 이름이 ROCKS_·__로 시작하면 ReservedDefine, 같은 이름이 두 번이면 DuplicateDefine,
//   이름이 식별자가 아니거나 값에 줄바꿈이 있으면 InvalidDefine이다.
// 결과는 이름 byte 순이다. 형식·컴파일러 매크로의 파생(ROCKS_SPIRV, ROCKS_IMAGE_FORMAT)은
// Engine/Shaders/Public/Platform.rsh가 이 정의에서 만든다.
//
// 사용 예:
//   ShaderDefineSet defines(allocator);
//   const ShaderDefine permutation[] = {{"USE_FOG", "1"}};
//   if (auto built = BuildShaderPreprocessDefines(format, ShaderStagePixel, format.hlsl2021,
//                                                 permutation, defines); !built) {
//       // built.code == ShaderSourceError::ReservedDefine 등, built.message에 이름
//   }
//   PreprocessShader(tree, entry, defines.Defines(), shader);
namespace rocks::shader {

// 정의 이름의 최대 byte 수다.
inline constexpr usize MaxShaderDefineNameBytes = 128;

// 조립한 정의 목록이다. 이름·값 byte를 소유하며 Defines()의 view는 다음 조립·파괴 전까지 유효하다.
class ROCKS_SHADERCOMPILER_API ShaderDefineSet final {
public:
    explicit ShaderDefineSet(Allocator& allocator) noexcept
        : text_(allocator), entries_(allocator), defines_(allocator) {
    }

    // 이름 byte 순 정의 목록이다.
    Span<const ShaderDefine> Defines() const noexcept {
        return defines_.View();
    }

    // 이름이 name인 정의의 값이다. 없으면 found가 false다.
    StringView Find(StringView name, bool& found) const noexcept;

    void Reset() noexcept;

private:
    friend ROCKS_SHADERCOMPILER_API ShaderSourceStatus BuildShaderPreprocessDefines(
        const ShaderFormatDefinition& format, uint32 stage, bool hlsl2021,
        Span<const ShaderDefine> jobDefines, ShaderDefineSet& out) noexcept;

    struct Entry {
        usize nameOffset = 0;
        usize nameBytes = 0;
        usize valueOffset = 0;
        usize valueBytes = 0;
    };

    String text_;
    Array<Entry> entries_;
    Array<ShaderDefine> defines_;
};

// format(Supported 형식 정의)·stage(ShaderStageBits 비트 하나, SC8)·hlsl2021·jobDefines로 정의를
// 조립해 out에 둔다(out은 먼저 비운다). 오류: UnsupportedStage(자리만 둔 형식, 비트가 하나가 아님,
// 형식의 stages에 없음), ReservedDefine, DuplicateDefine, InvalidDefine, OutOfMemory. message에
// 문제 이름이 있다.
ROCKS_SHADERCOMPILER_API ShaderSourceStatus
BuildShaderPreprocessDefines(const ShaderFormatDefinition& format, uint32 stage, bool hlsl2021,
                             Span<const ShaderDefine> jobDefines, ShaderDefineSet& out) noexcept;

} // namespace rocks::shader
