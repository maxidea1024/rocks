#pragma once

#include <Base/Types.h>
#include <Core/Containers/SlotArray.h>
#include <Core/Containers/Span.h>
#include <Core/Text/StringView.h>

// 쿠킹된 셰이더 코드를 장치에 넘기는 값 타입이다(Docs/Design/ShaderSystem 16.1절 SSD18, 결정 9).
// 셰이더는 파이프라인과 따로 장치 셰이더(Device::CreateShader)로 만들고 파이프라인은 그 handle을
// 받는다(UE RHICreateVertexShader·RHICreatePixelShader 뒤 PSO 초기화자가 handle을 받는 구조,
// Runtime/RHI/Public/RHIResources.h). 코드와 정규화 바인딩 layout(값 1)·GLES 바인딩 머리(값 2)는
// 셰이더 라이브러리 항목 값 그대로다(ShaderCore ShaderLibrary.h).
namespace rocks::graphics {

// 셰이더 단계다.
enum class ShaderStage : uint8 {
    Vertex,
    Pixel,
    Compute
};

// 장치가 받는 셰이더 코드 종류다. 장치는 자기 backend 종류만 받는다(ShaderCodeKindOf).
enum class ShaderCodeKind : uint8 {
    // D3D12: DXIL 컨테이너(DXC)
    Dxil,
    // D3D11: DXBC 컨테이너(FXC 또는 SPIRV-Cross HLSL 5.0 → FXC)
    Dxbc,
    // Vulkan: SPIR-V word(little-endian)
    Spirv,
    // OpenGL ES: ESSL 3.20 원문(UTF-8, NUL 없음)과 GLES 바인딩 머리
    Essl,
    // Metal: metallib(SH5 전에는 장치가 없다)
    Metallib
};

// "Dxil" 같은 이름이다. 범위 밖이면 "Unknown"이다.
ROCKS_GRAPHICS_API const char* ShaderCodeKindName(ShaderCodeKind kind) noexcept;

// 장치 셰이더 handle이다(Device::CreateShader). 발급한 장치 안에서만 의미가 있다.
struct ShaderHandle {
    SlotHandle slot;

    bool IsNull() const noexcept {
        return slot.IsNull();
    }

    bool operator==(const ShaderHandle&) const noexcept = default;
};

// 장치 셰이더 생성 설정이다. byte는 호출 안에서 복사하므로 호출 뒤 버려도 된다.
// - code: kind 형식의 코드(Spirv는 4 byte 배수).
// - bindingLayout: ShaderCore EncodeShaderBindingLayout byte(값 1). stage와 같은 단계여야 한다.
// - glesBindingHeader: Essl만, ShaderCore EncodeShaderGlesBindingHeader byte(값 2). 다른 종류는
// 비운다.
struct ShaderCodeDesc {
    ShaderCodeKind kind = ShaderCodeKind::Dxil;
    ShaderStage stage = ShaderStage::Vertex;
    Span<const uint8> code;
    Span<const uint8> bindingLayout;
    Span<const uint8> glesBindingHeader;
};

// 그리기 기록의 고정 slot(CommandContext SetTexture·SetBufferView·SetWritableBufferView·
// SetWritableTexture)과 셰이더 자원 이름을 잇는 표다. 파라미터 묶음(RenderBase GX6) 전의 개발
// 경로다. 셰이더 원문은 register를 적지 않으므로(ShaderSystem 7.2절 1항) slot은 이름으로 정한다.
// 셰이더가 선언한 자원이 이 표에 없으면 파이프라인 생성이 InvalidArgument다(루트 상수는 표 밖이다 —
// drawConstantBytes).
// - texture: SetTexture가 묶는 SRV 텍스처. 짝 샘플러는 같은 단계의 <texture>Sampler다
//   (ShaderSystem 7.2절 9항). 그래픽은 PS, compute는 CS만 읽는다.
// - bufferViews[n]: SetBufferView(n)이 묶는 typed(SRV 텍스처 종류)·구조화·raw(구조화 SRV 종류)
//   버퍼. VS·PS가 같은 이름이면 같은 view를 본다.
// - writableBuffers[n]·writableTextures[n]: compute 쓰기 slot n(같은 번호 공간이라 한 slot에 둘 중
//   하나만 이름을 준다).
//
// 사용 예(PS가 Texture2D Albedo·SamplerState AlbedoSampler와 Buffer<float4> Lights를 읽는다):
//   desc.bindings.texture = "Albedo";
//   desc.bindings.bufferViews[0] = "Lights";
inline constexpr uint32 MaxPipelineNamedSlots = 4;

struct PipelineBindingNames {
    StringView texture;
    StringView bufferViews[MaxPipelineNamedSlots];
    StringView writableBuffers[MaxPipelineNamedSlots];
    StringView writableTextures[MaxPipelineNamedSlots];
};

} // namespace rocks::graphics
