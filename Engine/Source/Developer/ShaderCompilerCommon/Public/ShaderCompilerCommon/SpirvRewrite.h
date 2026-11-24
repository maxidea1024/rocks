#pragma once

#include <Base/Types.h>
#include <Core/Containers/Array.h>
#include <Core/Containers/Span.h>
#include <Core/Result.h>
#include <Core/Text/String.h>
#include <Core/Text/StringView.h>
#include <ShaderCore/ShaderFormatTypes.h>

// SPIR-V 고쳐 쓰기다(Docs/Design/ShaderSystem 7.2절 2·4·7항). SPIR-V 경로 형식 처리기가 함께
// 쓴다(도구를 열지 않는 순수 변환이라 ShaderCompilerCommon에 둔다).
//
// 사용 예:
//   SpirvRewriteOptions options;
//   options.stage = ShaderStagePixel;
//   options.stageLocations = true;
//   Array<uint32> rewritten(allocator);
//   if (auto done = RewriteSpirv(raw.View(), options, rewritten, diagnostics); !done) { ... }
namespace rocks::shader {

// SPIR-V 고쳐 쓰기 선택이다(ShaderSystem 16.1절 SSD29·SSD38 — 셰이더 워커의
// VULKAN_SM5·METAL_SM5·D3D11 대체 경로가 쓴다).
struct SpirvRewriteOptions {
    // ShaderStageBits 한 비트다.
    uint32 stage = 0;
    // push constant로 바꿀 상수 버퍼 이름이다(빈 값이면 바꾸지 않는다 — 워커는
    // ROCKS_ROOT_CONSTANTS가 이미 push constant를 낸다).
    StringView rootConstantsName;
    // 단계 사이 변수(VS 출력·PS 입력)의 location을 시맨틱 TEXCOORDn의 n으로 바꾼다(7.2절 7항). 켜면
    // TEXCOORDn(n ≤ 15)이 아닌 단계 사이 변수와 겹치는 n은 InvalidArgument다.
    bool stageLocations = false;
    // RWTextureCube·RWTextureCubeArray 표지(Platform.rsh)를 단 storage image를 cube·cube 배열
    // 이미지(Dim Cube)로 바꾼다(16.1절 SH2g — GLES imageCube·imageCubeArray). 끄면 2D 배열
    // 그대로다(D3D·Vulkan·Metal은 cube 쓰기 view를 면 단위 2D 배열로 묶는다). 켜면 배열로 선언한
    // 표지 이미지와 그 이미지의 GetDimensions(OpImageQuerySize)는 InvalidArgument다.
    bool cubeStorageImages = false;
};

// DXC SPIR-V(-fspv-reflect)를 Rocks 규약으로 고쳐 쓴다(ShaderSystem 7.2절 2·4·7항): 정점 입력
// location = ATTRIBUTEn, (선택) 단계 사이 location = TEXCOORDn, (선택) 이름의 상수 버퍼 → push
// constant, set = 단계· binding = 종류 순서(상수 버퍼 → 구조화·raw → storage image → SRV 텍스처 →
// 샘플러, set 3 PS 진단 버퍼는 그대로 — SSD34), SPV_GOOGLE 반영 확장·장식 제거. 실행 모델이 단계와
// 다르거나 규칙 위반·망가진 SPIR-V는 InvalidArgument와 diagnostics다(output은 정해지지 않는다).
ROCKS_SHADERCOMPILERCOMMON_API Result<void> RewriteSpirv(Span<const uint32> input,
                                                         const SpirvRewriteOptions& options,
                                                         Array<uint32>& output,
                                                         String& diagnostics) noexcept;

} // namespace rocks::shader
