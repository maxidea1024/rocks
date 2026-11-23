#pragma once

#include <ShaderCompilerCommon/ShaderFormatBackend.h>

// D3D 형식 처리기다(Docs/Design/ShaderSystem.ko.md 2절, 11.1절 SC1). D3D12_SM6은 DXC → DXIL이고
// 내장 validator 서명을 확인한다(10절 "dxil 서명": container hash가 0이거나 preview 우회 값이면
// ToolError). D3D11_SM5(FXC·대체 경로)는 SH1e에서 더한다 — 지금은 UnsupportedCapability다.
//
// DXC 인자: -E 엔트리, -T <단계>_<셰이더 모델>, -HV 2021|2018, -O0–-O3, -Zpr(행 우선 — 7.2절 6항),
// -WX(경고는 오류 — 결정 7). 결과 값 0(Code)은 DXIL container byte다.
//
// 사용 예(워커):
//   D3DShaderFormatBackend d3d;
//   d3d.Compile(format, job, &tools, result);
namespace rocks::shader {

// 처리기 버전이다(형식 정의 processor.version — 출력 의미를 바꾸면 올리고 형식 정의도 같이 올린다).
inline constexpr uint32 D3DShaderProcessorVersion = 1;

class ROCKS_SHADERFORMATD3D_API D3DShaderFormatBackend final : public ShaderFormatBackend {
public:
    ShaderFormatModule Module() const noexcept override {
        return ShaderFormatModule::D3D;
    }

    uint32 ProcessorVersion() const noexcept override {
        return D3DShaderProcessorVersion;
    }

    void Compile(const ShaderFormatDefinition& format, const ShaderWorkerJob& job,
                 const ShaderToolSet* tools, ShaderWorkerJobResult& result) noexcept override;
};

// DXIL container가 서명되었는지다(magic "DXBC"이고 hash가 0도 preview 우회 값 0x02×16도 아니다).
ROCKS_SHADERFORMATD3D_API bool IsSignedDxilContainer(Span<const uint8> container) noexcept;

} // namespace rocks::shader
