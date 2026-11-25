#pragma once

#include <Base/Types.h>
#include <Base/NumericLimits.h>
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
    // -fspv-reflect(시맨틱·user type 장식 — 셰이더 워커 VULKAN_SM5가 리플렉션에 쓰고 고쳐 쓰기에서
    // 지운다, ShaderSystem 16.1절 SSD29)다.
    bool reflect = false;
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

// MSL 자원 배정 하나다(SPIRV-Cross spvc_msl_resource_binding_2 — set이 SpirvMslPushConstantSet이면
// push constant, 배정 없는 공간은 SpirvMslNoIndex).
struct SpirvMslBinding {
    uint32 set = 0;
    uint32 binding = 0;
    uint32 buffer = 0;
    uint32 texture = 0;
    uint32 sampler = 0;
};

inline constexpr uint32 SpirvMslNoIndex = NumericLimits<uint32>::Max();
inline constexpr uint32 SpirvMslPushConstantSet = NumericLimits<uint32>::Max();

// ESSL 자원 binding 하나다(SPIR-V (set, binding) → GLES binding — storage block·image·typed
// buffer).
struct SpirvGlslBinding {
    uint32 set = 0;
    uint32 binding = 0;
    uint32 glBinding = 0;
};

// ESSL 결합 sampler 하나의 이름과 텍스처 유닛이다((텍스처, 샘플러) 변수 이름으로 찾는다).
struct SpirvGlslCombined {
    StringView texture;
    StringView sampler;
    StringView name;
    uint32 unit = 0;
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
    // ShaderStageBits 한 비트(자원 배정의 실행 모델)다. HLSL 배정·MSL 배정에만 쓴다.
    uint32 stage = 0;
    // HLSL 5.0: register를 종류의 공간(b·t·u·s)마다 0부터 (set, binding) 순으로 정하고 이름이
    // RootConstants로 끝나는 cbuffer는 b13, 정점 입력 location n은 시맨틱 ATTRIBUTEn이다(SPIR-V를
    // 먼저 RewriteSpirv로 고쳐 써 location = n이어야 한다 — D3D11 대체 경로, ShaderSystem 16.1절
    // SSD38).
    bool hlslAssignRegisters = false;
    // MSL: 자원 인덱스 배정(SSD39)과 구조화 버퍼 크기 side table buffer 인덱스(없으면
    // SpirvMslNoIndex)다.
    Span<const SpirvMslBinding> mslBindings;
    uint32 mslBufferSizeIndex = SpirvMslNoIndex;
    // ESSL: 상수 버퍼를 plain uniform 구조체로 낸다(GLES 에뮬레이션 재작성의 입력 — 16.1절 SSD42).
    bool glslPlainUniforms = false;
    // ESSL: 자원 binding과 결합 sampler 이름·유닛(SSD42)이다.
    Span<const SpirvGlslBinding> glslBindings;
    Span<const SpirvGlslCombined> glslCombined;
    // ESSL: 샘플러 없이 Load만 쓰는 텍스처(texelFetch)를 위해 dummy 샘플러를 만든다
    // (build_dummy_sampler_for_combined_images, 16.1절 SH2e2 메모). glslCombined의 sampler가 빈
    // 항목이 그 텍스처의 결합 sampler 이름·유닛이다.
    bool glslDummySampler = false;
};

// SPIRV-Cross C API로 spirv를 target 원문으로 바꾼다. ESSL은 결합 sampler를 만들고 기본 float
// 정밀도를 highp로 둔다. MSL은 macOS platform이고 typed buffer를 texture_buffer로 낸다(측정 S13).
// SPIRV-Cross가 받지 않는 구성은 CompileError(진단에 SPIRV-Cross 문구)다.
ROCKS_SHADERTOOLLOADER_API ShaderToolOutcome CrossCompileSpirv(const SpirvCrossFunctions& cross,
                                                               const SpirvCrossRequest& request,
                                                               String& text,
                                                               String& diagnostics) noexcept;

// (텍스처, 샘플러) 사용 쌍 하나다(이름은 SPIR-V OpName — 변수 이름).
struct SpirvSamplerUse {
    explicit SpirvSamplerUse(Allocator& allocator) noexcept
        : texture(allocator), sampler(allocator) {
    }

    String texture;
    String sampler;
};

// SPIRV-Cross build_combined_image_samplers로 spirv의 (텍스처, 샘플러) 사용 쌍을 out에
// 둔다(ShaderSystem 16.1절 SSD36 — 같은 쌍은 한 번). 실패는 ToolError(진단)다.
ROCKS_SHADERTOOLLOADER_API ShaderToolOutcome QuerySpirvSamplerUses(const SpirvCrossFunctions& cross,
                                                                   Span<const uint8> spirv,
                                                                   Array<SpirvSamplerUse>& out,
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
