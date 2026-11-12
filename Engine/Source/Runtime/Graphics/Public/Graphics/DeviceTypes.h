#pragma once

#include <Core/Containers/SlotArray.h>
#include <Core/Containers/Span.h>
#include <Core/Result.h>
#include <Core/Text/StringView.h>
#include <GraphicsTypes/PixelFormat.h>
#include <GraphicsTypes/Sampler.h>

// 그래픽 장치 계약의 값 타입(handle·장치·자원 설정)이다. backend와 무관하며 API 객체를 담지
// 않는다. 픽셀 형식·샘플러는 GraphicsTypes 모듈에 있다.
namespace rocks::graphics {

// 그래픽 backend다. D3D12·OpenGL은 구현 모듈(D3D12Graphics·OpenGLGraphics)이 있고, Vulkan·Metal은
// 자리만 둔다(구현 모듈이 아직 없어 선택하면 Unsupported).
enum class Backend : uint8 {
    D3D12,
    OpenGL,
    Vulkan,
    Metal
};

// "D3D12" 같은 이름이다. 범위 밖이면 "Unknown"이다.
const char* BackendName(Backend backend) noexcept;

// 정점 속성 형식이다.
enum class VertexFormat : uint8 {
    Float2,
    Float3,
    Float4
};

// 형식의 바이트 크기다.
uint32 VertexFormatSize(VertexFormat format) noexcept;

// 자원 handle이다. 발급한 장치 안에서만 의미가 있으며 파괴 후에는 stale이 된다(slot+generation).
struct BufferHandle {
    SlotHandle slot;

    bool IsNull() const noexcept {
        return slot.IsNull();
    }

    bool operator==(const BufferHandle&) const noexcept = default;
};

struct TextureHandle {
    SlotHandle slot;

    bool IsNull() const noexcept {
        return slot.IsNull();
    }

    bool operator==(const TextureHandle&) const noexcept = default;
};

struct PipelineHandle {
    SlotHandle slot;

    bool IsNull() const noexcept {
        return slot.IsNull();
    }

    bool operator==(const PipelineHandle&) const noexcept = default;
};

struct SwapChainHandle {
    SlotHandle slot;

    bool IsNull() const noexcept {
        return slot.IsNull();
    }

    bool operator==(const SwapChainHandle&) const noexcept = default;
};

// 장치 생성 설정이다.
struct DeviceDesc {
    // API 검증 계층(D3D12 debug layer, GL debug context)을 켠다. 없으면 켜지 않고 계속한다.
    bool enableValidation = false;
    // D3D12: 하드웨어 대신 소프트웨어 장치(WARP)를 쓴다. 헤드리스 CI 검증용이다. OpenGL은 무시한다.
    bool preferSoftware = false;
};

// 창에 그리기 위한 OS 표면이다. Windows: window = HWND, instance = HINSTANCE.
struct NativeSurface {
    void* window = nullptr;
    void* instance = nullptr;
};

struct SwapChainDesc {
    NativeSurface surface;
    uint32 width = 0;
    uint32 height = 0;
    // 수직 동기화로 present한다.
    bool vsync = true;
};

enum class BufferUsage : uint8 {
    Vertex
};

struct BufferDesc {
    usize size = 0;
    BufferUsage usage = BufferUsage::Vertex;
};

enum class TextureUsage : uint8 {
    // 셰이더에서 샘플링한다(초기 픽셀 필요).
    Sampled,
    // 렌더 타깃으로 그리고 ReadPixels로 읽거나 샘플링한다(초기 픽셀 없음).
    RenderTarget
};

// 텍스처 설정이다.
//
// 초기 픽셀 배치(Sampled): mip 0부터 mipLevels개를 빈틈 없이 잇는다. 각 mip은 MipExtent(width,
// level) × MipExtent(height, level) 픽셀, 첫 행이 위, 행 사이 padding이 없다(MipChainBytes 크기).
struct TextureDesc {
    uint32 width = 0;
    uint32 height = 0;
    Format format = Format::RGBA8UnormSrgb;
    TextureUsage usage = TextureUsage::Sampled;
    // 1–FullMipCount(width, height)다. RenderTarget은 1이어야 한다.
    uint32 mipLevels = 1;
    // 이 텍스처를 묶을 때 쓰는 샘플러다.
    SamplerDesc sampler;
};

// 정점 속성 하나다. location은 GLSL의 layout(location), HLSL에서는 TEXCOORD{location} 의미 체계로
// 연결한다(셰이더 작성 규칙).
struct VertexAttribute {
    uint32 location = 0;
    VertexFormat format = VertexFormat::Float3;
    uint32 offset = 0;
};

// backend별 셰이더 원문이다. 장치는 자기 backend 원문만 쓴다. 진입점 이름은 HLSL에서 VSMain·PSMain,
// GLSL에서 main이다.
struct ShaderSource {
    StringView hlsl;
    StringView glsl;
};

// 그리기 파이프라인 설정이다. 모든 파이프라인은 같은 고정 자원 배치를 쓴다: 그리기 상수
// (DrawConstants, HLSL b0 / GLSL uniform uTransform·uColor)와 텍스처 하나(HLSL t0·s0 /
// GLSL binding 0 uTexture). 샘플러는 묶은 텍스처의 TextureDesc::sampler를 따른다. 깊이·컬링은
// 쓰지 않는다.
struct PipelineDesc {
    ShaderSource vertex;
    ShaderSource pixel;
    Span<const VertexAttribute> attributes;
    uint32 vertexStride = 0;
    Format colorFormat = Format::RGBA8UnormSrgb;
    // 소스 알파로 섞는다(premultiplied가 아닌 일반 알파).
    bool alphaBlend = false;
};

// 그리기마다 넘기는 상수다. transform은 열 우선(column-major) 4x4 행렬(Rocks Mat4 저장 순서
// 그대로), color는 선형 RGBA다. 셰이더는 mul(transform, position)으로 쓴다.
struct DrawConstants {
    float transform[16] = {1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1};
    float color[4] = {1, 1, 1, 1};
};

struct ClearColor {
    float r = 0.0f;
    float g = 0.0f;
    float b = 0.0f;
    float a = 1.0f;
};

// 장치 정보다. 문자열은 장치가 소유하며 장치 수명 동안 유효하다.
struct DeviceInfo {
    Backend backend = Backend::D3D12;
    StringView adapterName;
    bool validationEnabled = false;
    bool software = false;
};

} // namespace rocks::graphics
