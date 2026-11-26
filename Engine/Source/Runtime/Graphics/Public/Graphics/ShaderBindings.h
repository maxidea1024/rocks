#pragma once

#include <Core/Containers/Array.h>
#include <Core/Containers/SlotArray.h>
#include <Core/Memory/Allocator.h>
#include <Core/Result.h>
#include <Graphics/ConstantBuffer.h>
#include <Graphics/DeviceTypes.h>
#include <Graphics/ShaderCode.h>
#include <ShaderCore/ShaderBindingLayout.h>
#include <ShaderCore/ShaderGlesBindingHeader.h>

// backend 공용 쿠킹 셰이더 처리다(Docs/Design/ShaderSystem 16.1절 SSD18·SH2e2 메모). 장치 셰이더는
// 코드와 정규화 바인딩 layout(값 1)·GLES 바인딩 머리(값 2)의 CPU 사본이고, backend 객체(D3D11
// shader·Vulkan module·GL shader)는 파이프라인을 만들 때 만든다 — 만든 파이프라인은 장치 셰이더를
// 붙잡지 않는다. 파이프라인 생성은 두 단계 layout과 slot 이름 표(PipelineBindingNames)로 자원의
// backend 위치 표(PipelineSlots)를 만든다. 상수 버퍼·텍스처·샘플러는 값 1 slot
// 그대로이고(RenderBase 9.11·9.13절), 버퍼 view·쓰기 자원은 이름 표의 고정 slot이다.
namespace rocks::graphics {

// 장치 셰이더 하나다(CreateShader가 검사·복사·decode한다). 옮겨도 layout 이름이 가리키는 byte는
// 그대로다(Array 버퍼가 함께 옮겨진다).
class ROCKS_GRAPHICS_API DeviceShaderCode {
public:
    explicit DeviceShaderCode(Allocator& allocator) noexcept;
    DeviceShaderCode(DeviceShaderCode&&) noexcept = default;
    DeviceShaderCode& operator=(DeviceShaderCode&&) noexcept = default;

    // desc를 검사해 복사·decode한다. 실패(InvalidArgument, reason에 영어 이유): 종류가 expected가
    // 아님, 코드가 비었거나 Spirv가 4 byte 배수가 아님, layout이 손상되었거나 단계가 desc.stage와
    // 다름, Essl인데 GLES 머리가 없거나 손상·단계 불일치, Essl이 아닌데 머리가 있음. 할당 실패는
    // OutOfMemory다.
    Result<void> Load(const ShaderCodeDesc& desc, ShaderCodeKind expected,
                      const char** reason) noexcept;

    ShaderStage Stage() const noexcept {
        return stage_;
    }

    Span<const uint8> Code() const noexcept {
        return code_.View();
    }

    const shader::ShaderBindingLayout& Layout() const noexcept {
        return layout_;
    }

    // Essl만 의미가 있다.
    const shader::ShaderGlesBindingHeader& GlesHeader() const noexcept {
        return header_;
    }

private:
    ShaderStage stage_ = ShaderStage::Vertex;
    Array<uint8> code_;
    Array<uint8> layoutBytes_;
    Array<uint8> headerBytes_;
    shader::ShaderBindingLayout layout_;
    shader::ShaderGlesBindingHeader header_;
};

// backend의 장치 셰이더 표다(backend가 자기 잠금 안에서 쓴다).
class ROCKS_GRAPHICS_API DeviceShaderTable {
public:
    explicit DeviceShaderTable(Allocator& allocator) noexcept
        : allocator_(allocator), shaders_(allocator) {
    }

    // CreateShader 본체다. 실패는 DeviceShaderCode::Load와 같다(reason).
    Result<ShaderHandle> Create(const ShaderCodeDesc& desc, ShaderCodeKind expected,
                                const char** reason) noexcept;

    // handle의 셰이더다. stale·null이면 null이다.
    const DeviceShaderCode* Find(ShaderHandle handle) const noexcept {
        return shaders_.Find(handle.slot);
    }

    // CPU 사본만 있으므로 바로 지운다(파이프라인은 셰이더를 붙잡지 않는다).
    void Retire(ShaderHandle handle) noexcept {
        (void)shaders_.TryRemove(handle.slot);
    }

    usize Size() const noexcept {
        return shaders_.Size();
    }

    void Clear() noexcept;

private:
    Allocator& allocator_;
    SlotArray<DeviceShaderCode> shaders_;
};

// 자원의 역할이다.
enum class PipelineSlotRole : uint8 {
    // SetTexture(stage, 값 1 slot)의 SRV 텍스처
    Texture,
    // 텍스처의 자기 샘플러(<texture>Sampler, SetSampler 값 또는 짝 텍스처 desc)
    Sampler,
    // 공유 샘플러(SharedWrapSampler·SharedClampSampler, SetSampler 값만)
    SharedSampler,
    // SetBufferView(slot)
    BufferView,
    // SetWritableBufferView(slot)
    WritableBuffer,
    // SetWritableTexture(slot)
    WritableTexture
};

// 짝 slot이 없음을 나타내는 PipelineSlotBinding::pair 값이다.
inline constexpr uint16 NoPairedSlot = 0xFFFF;

// 셰이더 자원 하나의 slot과 backend 위치다.
struct PipelineSlotBinding {
    ShaderStage stage = ShaderStage::Vertex;
    PipelineSlotRole role = PipelineSlotRole::Texture;
    // 이름 표 slot 번호다(버퍼 view·쓰기 자원만, 텍스처·샘플러는 0).
    uint8 slot = 0;
    shader::ShaderResourceKind kind = shader::ShaderResourceKind::SampledTexture;
    shader::ShaderResourceShape shape = shader::ShaderResourceShape::None;
    // backend 위치(값 1 slot — D3D register, Vulkan set = 단계 안 binding, GLES 유닛·binding)다.
    // 텍스처·샘플러는 이 값이 곧 SetTexture·SetSampler의 slot이다.
    uint16 location = 0;
    // 텍스처: 그 텍스처를 샘플링하는 샘플러의 위치(자기 샘플러, 또는 값 1 샘플러 출처가 공유면 그
    // 공유 샘플러 — SPIR-V 경로 형식만 출처를 안다), 없으면 NoPairedSlot. 자기 샘플러: 짝 텍스처의
    // 위치. 그 밖은 NoPairedSlot이다.
    uint16 pair = NoPairedSlot;
};

// 파이프라인 하나가 읽고 쓰는 자원 수의 상한이다(단계마다 SRV 텍스처 MaxStageTextures + 샘플러
// MaxStageSamplers + 구조화 버퍼 view 4 + 쓰기 4, 단계 둘).
inline constexpr uint32 MaxPipelineSlotBindings =
    2 * (MaxStageTextures + MaxStageSamplers + 2 * MaxDrawBufferViews);

// 파이프라인의 고정 slot 표다.
struct PipelineSlots {
    PipelineSlotBinding bindings[MaxPipelineSlotBindings] = {};
    uint32 count = 0;
    // 읽는 버퍼 view slot과 compute 쓰기 slot 배치(그리기 때 DrawBufferSlotsSatisfied)다.
    DrawBufferSlots bufferSlots;
    DrawBufferSlots writeSlots;
    // compute가 텍스처로 쓰는 쓰기 slot 비트다.
    uint32 writeTextures = 0;
    // 단계마다 셰이더가 읽는 루트 상수 byte 수다(VS·PS 또는 CS).
    uint32 rootConstantBytes[2] = {};
    // 단계마다 읽는 상수 버퍼 slot(값 1 slot = backend 위치)과 선언 byte 수다(RenderBase 9.11절,
    // CommandContext::SetConstantBuffer·SetConstantData).
    ConstantBufferSlots constantSlots;
    // PS가 진단 버퍼(RocksDebugBuffer, ShaderSystem 7.2절 12항)를 쓴다. 자리는 backend가 정한다
    // (9.11절 6항 — D3D12 root UAV, 그 밖은 SL2까지 Unsupported).
    bool debugBuffer = false;
};

// PS 진단 버퍼의 예약 이름이다(ShaderSystem 7.2절 12항).
inline constexpr char PixelDebugBufferName[] = "RocksDebugBuffer";

// 영어 이유 버퍼 크기다.
inline constexpr usize PipelineSlotErrorBytes = 256;

// stages(그래픽은 VS·PS, compute는 CS)의 layout과 이름 표로 out을 채운다. 상수 버퍼는 이름 표 없이
// 모두 constantSlots에 들어간다(RenderBase 9.11절 3항). 텍스처(텍스처 모양의 SRV)·샘플러도 이름 표
// 없이 값 1 slot으로 모두 받는다(9.13절 1항 — 자기 샘플러는 같은 단계의 짝 텍스처와 pair로 잇는다).
// PS의 RocksDebugBuffer는 debugBuffer로 표시한다. 실패는 InvalidArgument이고 error에 영어 이유를
// 쓴다: 단계 루트 상수가 drawConstantBytes보다 큼, 상수 버퍼 slot이 MaxConstantBufferSlots 밖이거나
// 크기가 규칙 밖, 텍스처·샘플러 slot이 MaxResourceSlots 밖, 단계 SRV 텍스처 수가 MaxStageTextures·
// 샘플러 수가 MaxStageSamplers를 넘음, 같은 단계에 짝 텍스처가 없는 샘플러, 표에 없는 버퍼 view·
// 쓰기 자원, 역할과 모양이 맞지 않음(buffer view 이름에 텍스처 등), 그래픽 단계의 UAV, 같은 slot을
// 다른 종류·값 계열로 읽음, 쓰기 slot에 버퍼와 텍스처를 함께 둠.
ROCKS_GRAPHICS_API Result<void>
ResolvePipelineSlots(Span<const DeviceShaderCode* const> stages, const PipelineBindingNames& names,
                     uint32 drawConstantBytes, PipelineSlots& out,
                     char (&error)[PipelineSlotErrorBytes]) noexcept;

// VS가 읽는 모든 attribute가 layout에 있고 형식의 셰이더 타입 계열이 입력 원소 타입과 같은지 본다
// (RenderBase 10.4절). 실패는 InvalidArgument(error에 location·형식·셰이더 타입)다.
ROCKS_GRAPHICS_API Result<void>
MatchShaderVertexInputs(const shader::ShaderBindingLayout& vertex, const VertexLayout& layout,
                        char (&error)[PipelineSlotErrorBytes]) noexcept;

// 그래픽 파이프라인 셰이더 쌍을 찾고 검사한다: 두 handle이 표에 있고 단계가 VS·PS이며 단계 사이
// 연결(ShaderCore CheckShaderStageLink)이 맞아야 한다. 실패는 InvalidArgument(error)다.
ROCKS_GRAPHICS_API Result<void> FindPipelineShaders(const DeviceShaderTable& table,
                                                    const PipelineDesc& desc,
                                                    const DeviceShaderCode*& vertex,
                                                    const DeviceShaderCode*& pixel,
                                                    char (&error)[PipelineSlotErrorBytes]) noexcept;

// compute 파이프라인 셰이더를 찾는다(단계 Compute). 실패는 InvalidArgument(error)다.
ROCKS_GRAPHICS_API Result<void> FindComputeShader(const DeviceShaderTable& table,
                                                  const ComputePipelineDesc& desc,
                                                  const DeviceShaderCode*& compute,
                                                  char (&error)[PipelineSlotErrorBytes]) noexcept;

// layout에서 이름이 name인 상수 버퍼의 slot(backend 위치)이다. 셰이더가 쓰지 않아 빠졌거나
// 이름이 없으면 NotFound다.
//
// 사용 예(Sandbox·시험 — 렌더 쪽은 RenderBase ShaderBindingMap의 자리를 쓴다):
//   auto slot = FindConstantBufferSlot(pixelLayout, "Material");
//   commands.SetConstantBuffer(ShaderStage::Pixel, slot.Value(), material);
ROCKS_GRAPHICS_API Result<uint32> FindConstantBufferSlot(const shader::ShaderBindingLayout& layout,
                                                         StringView name) noexcept;

// layout에서 이름이 name인 SRV 텍스처(텍스처 모양 — typed buffer는 버퍼 view라 빠진다)의
// slot(backend 위치, SetTexture의 slot)이다. 셰이더가 쓰지 않아 빠졌거나 이름이 없으면 NotFound다.
//
// 사용 예:
//   auto slot = FindTextureSlot(pixelLayout, "SceneColor");
//   commands.SetTexture(ShaderStage::Pixel, slot.Value(), sceneColor);
ROCKS_GRAPHICS_API Result<uint32> FindTextureSlot(const shader::ShaderBindingLayout& layout,
                                                  StringView name) noexcept;

// layout에서 이름이 name인 샘플러(자기·공유)의 slot(SetSampler의 slot)이다. 없으면 NotFound다.
ROCKS_GRAPHICS_API Result<uint32> FindSamplerSlot(const shader::ShaderBindingLayout& layout,
                                                  StringView name) noexcept;

// 셰이더 텍스처 모양이 view 차원·표본 수를 받는지다(Texture2D ↔ 2D 1표본, Texture2DMultisample ↔
// 2D 여러 표본, Texture2DArray·Texture2DMultisampleArray ↔ 2D 배열, Texture3D·TextureCube·
// TextureCubeArray ↔ 같은 차원 1표본). 1D 모양과 텍스처가 아닌 모양은 거짓이다(RenderBase 9.13절
// 6항 자원 묶음 검증).
ROCKS_GRAPHICS_API bool TextureShapeAccepts(shader::ShaderResourceShape shape,
                                            TextureDimension dimension,
                                            uint32 sampleCount) noexcept;

// 값 1 원소 타입을 PixelShaderType으로 바꾼다(None은 None).
ROCKS_GRAPHICS_API PixelShaderType
PixelShaderTypeOf(shader::ShaderComponentType component) noexcept;

// "vertex"·"pixel"·"compute"다.
ROCKS_GRAPHICS_API const char* ShaderStageName(ShaderStage stage) noexcept;

} // namespace rocks::graphics
