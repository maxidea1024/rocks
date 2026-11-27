#pragma once

#include <ShaderCompilerCommon/ShaderFormatBackend.h>

// D3D 형식 처리기다(Docs/Design/ShaderSystem 2절, 11.1절 SC1). D3D12_SM6은 DXC → DXIL이고
// 내장 validator 서명을 확인한다(10절 "dxil 서명": container hash가 0이거나 preview 우회 값이면
// ToolError). D3D11_SM5는 FXC → DXBC이고(SH1e E3), HLSL 2021이거나 FXC가 컴파일 오류(원문 진단이
// 있는 실패 — 16.4절 SSD80)를 내면 대체 경로 DXC → SPIR-V(셰이더 모델 6.0) → SPIRV-Cross HLSL
// 5.0 → `#pragma warning(disable : 3571)` → FXC다(SC10, E4, SSD81). 그 밖의 FXC 실패는
// ToolError이고 대체 경로를 타지 않는다. 대체 경로가 성공하면 1차 FXC 진단을 결과 진단에 남긴다.
// FXC가 없는 호스트는 UnsupportedCapability다.
//
// DXC 인자: -E 엔트리, -T <단계>_<셰이더 모델>, -HV 2021|2018, -O0–-O3, -Zpr(행 우선 — 7.2절 6항),
// 옵션 warningsAsErrors면 -WX(결정 7, SSD79). FXC 플래그: 최적화 단계, 행 우선, warningsAsErrors면
// 경고는 오류. 성공 job의 경고는 결과 진단(`DXC: `·`FXC: `·`fallback …: `)이다. 결과 값 0(Code)은
// DXIL·DXBC container byte다.
//
// 사용 예(워커):
//   D3DShaderFormatBackend d3d;
//   ShaderBackendContext context;
//   context.tools = &tools;
//   d3d.Compile(format, job, context, result);
namespace rocks::shader {

// 처리기 버전이다(형식 정의 processor.version — 출력 의미를 바꾸면 올리고 형식 정의도 같이 올린다).
// 7: FXC 결과 분류·대체 경로 X3571 끄기·경고 옵션(ShaderSystem 16.4절 SSD79–SSD81).
inline constexpr uint32 D3DShaderProcessorVersion = 7;

class ROCKS_SHADERFORMATD3D_API D3DShaderFormatBackend final : public ShaderFormatBackend {
public:
    ShaderFormatModule Module() const noexcept override {
        return ShaderFormatModule::D3D;
    }

    uint32 ProcessorVersion() const noexcept override {
        return D3DShaderProcessorVersion;
    }

    void Compile(const ShaderFormatDefinition& format, const ShaderWorkerJob& job,
                 const ShaderBackendContext& context,
                 ShaderWorkerJobResult& result) noexcept override;
};

// DXIL container가 서명되었는지다(magic "DXBC"이고 hash가 0도 preview 우회 값 0x02×16도 아니다).
ROCKS_SHADERFORMATD3D_API bool IsSignedDxilContainer(Span<const uint8> container) noexcept;

} // namespace rocks::shader
