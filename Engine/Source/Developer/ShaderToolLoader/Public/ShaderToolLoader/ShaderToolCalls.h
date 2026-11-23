#pragma once

#include <Base/Types.h>
#include <Core/Containers/Array.h>
#include <Core/Containers/Span.h>
#include <Core/Text/String.h>
#include <Core/Text/StringView.h>
#include <ShaderToolLoader/ShaderToolLoader.h>

// 형식 처리기가 함께 쓰는 도구 호출이다(Docs/Design/ShaderSystem 11.1절 SH1e E2). 여러 형식이 같은
// 도구 단계(HLSL → SPIR-V, SPIR-V 검증, SPIRV-Cross 변환, FXC)를 쓰므로 도구를 여는 이 계층에 둔다
// (정책 shader-tool-binary-load 안 — 형식 처리기와 워커만 부른다).
//
// 결과 분류(ShaderToolOutcome):
// - Success: 결과가 out에 있다(경고가 diagnostics에 남을 수 있다 — 형식 처리기는 경고를 오류로
// 다루는
//   인자를 준다).
// - CompileError: 원문 때문의 실패다(DXC·FXC 컴파일 오류, SPIRV-Cross가 받지 않는 구성).
// - ToolError: 도구 호출 실패(함수 표가 비었음, COM·HRESULT 실패, spirv-val 거부, 메모리 부족)다.
// - UnsupportedHost: 이 호스트에 그 도구 구현이 없다(Windows 밖의 DXC·FXC).
// 실패하면 diagnostics에 영어 진단(도구 원문 포함)이 있다. 결과·진단은 먼저 비운다.
//
// 사용 예(형식 처리기):
//   SpirvCompileRequest request;
//   request.source = job.source.View();
//   request.entry = job.entry.View();
//   request.profile = "ps_6_0";
//   Array<uint8> spirv(allocator);
//   String diagnostics(allocator);
//   if (CompileHlslToSpirv(*tools, request, spirv, diagnostics) != ShaderToolOutcome::Success) {
//       ...
//   }
namespace rocks::shader {

enum class ShaderToolOutcome : uint8 {
    Success,
    CompileError,
    ToolError,
    UnsupportedHost
};

// DXC 최적화 인자 수(O0–O3)다.
inline constexpr uint32 ShaderOptimizationLevels = 4;

// HLSL → SPIR-V(DXC) 입력이다. view는 호출 동안 살아 있어야 한다.
struct SpirvCompileRequest {
    StringView source;
    StringView entry;
    // DXC 프로필(예: "ps_6_0" — SPIR-V 경로는 셰이더 모델 6.0 이상)이다.
    StringView profile;
    bool hlsl2021 = false;
    // 0–3(O0–O3)이다.
    uint32 optimization = ShaderOptimizationLevels - 1;
    // -fvk-use-dx-position-w(7.2절 8항)다.
    bool dxPositionW = false;
};

// DXC로 SPIR-V(-spirv -fspv-target-env=vulkan1.1, -Zpr, -WX)를 만들고 spirv-val(Vulkan 1.1)로
// 검증한다. 검증 거부는 ToolError다(DXC 출력이 검증을 통과하지 못하면 도구 쪽 결함이다).
ROCKS_SHADERTOOLLOADER_API ShaderToolOutcome CompileHlslToSpirv(const ShaderToolSet& tools,
                                                                const SpirvCompileRequest& request,
                                                                Array<uint8>& spirv,
                                                                String& diagnostics) noexcept;

// spirv-val(Vulkan 1.1 환경)이다. 실패하면 false와 diagnostics(검증기 문구)다.
ROCKS_SHADERTOOLLOADER_API bool ValidateSpirv(const SpirvToolsFunctions& tools,
                                              Span<const uint8> spirv,
                                              String& diagnostics) noexcept;

// SPIRV-Cross 출력 언어다.
enum class SpirvCrossTarget : uint8 {
    // HLSL 셰이더 모델 5.0(D3D11 대체 경로 — 엔트리 이름은 main이 된다).
    Hlsl50,
    // ESSL(GLSL ES).
    Essl,
    // MSL.
    Msl
};

struct SpirvCrossRequest {
    Span<const uint8> spirv;
    SpirvCrossTarget target = SpirvCrossTarget::Essl;
    // ESSL #version 숫자(예: 320)다.
    uint32 esslVersion = 0;
    // SPIRV-Cross fixup_clipspace·flip_vert_y(7.2절 좌표 행)다.
    bool fixupClipSpace = false;
    bool flipVertexY = false;
    // MSL 버전(major.minor)이다.
    uint32 mslMajor = 0;
    uint32 mslMinor = 0;
};

// SPIRV-Cross C API로 spirv를 target 원문으로 바꾼다. ESSL은 결합 sampler를 만들고 기본 float
// 정밀도를 highp로 둔다. MSL은 macOS platform이고 typed buffer를 texture_buffer로 낸다(측정 S13).
// SPIRV-Cross가 받지 않는 구성은 CompileError(진단에 SPIRV-Cross 문구)다.
ROCKS_SHADERTOOLLOADER_API ShaderToolOutcome CrossCompileSpirv(const SpirvCrossFunctions& cross,
                                                               const SpirvCrossRequest& request,
                                                               String& text,
                                                               String& diagnostics) noexcept;

// FXC(D3DCompile) 입력이다.
struct FxcCompileRequest {
    StringView source;
    StringView entry;
    // FXC 프로필(예: "vs_5_0")이다.
    StringView profile;
    // 0–3(D3DCOMPILE_OPTIMIZATION_LEVEL0–3)이다.
    uint32 optimization = ShaderOptimizationLevels - 1;
};

// FXC로 DXBC를 만든다(행 우선 D3DCOMPILE_PACK_MATRIX_ROW_MAJOR, 경고는 오류
// D3DCOMPILE_WARNINGS_ARE_ERRORS, 원문 이름 없음 — 11.1절 E3). E_FAIL과 오류 blob이면
// CompileError, 그 밖의 실패는 ToolError(진단에 HRESULT)다. Windows 구현만 있다(그 밖은
// UnsupportedHost).
ROCKS_SHADERTOOLLOADER_API ShaderToolOutcome CompileWithFxc(const FxcFunctions& fxc,
                                                            const FxcCompileRequest& request,
                                                            Array<uint8>& object,
                                                            String& diagnostics) noexcept;

} // namespace rocks::shader
