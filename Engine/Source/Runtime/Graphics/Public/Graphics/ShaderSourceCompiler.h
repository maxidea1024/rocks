#pragma once

#include <Core/Containers/Array.h>
#include <Core/Result.h>
#include <Core/Text/String.h>
#include <Core/Text/StringView.h>
#include <Graphics/ShaderCode.h>
#include <GraphicsTypes/PixelFormat.h>
#include <GraphicsTypes/VertexFormat.h>

// 실행 중 컴파일러가 없는 backend의 개발용 셰이더 컴파일 훅이다(Docs/Design/VulkanGraphics.ko.md
// 13절). 쿠킹된 셰이더를 받는 Graphics API(ShaderSystem SH2)가 생기기 전까지 PipelineDesc의 HLSL
// 원문을 backend 코드(지금은 SPIR-V)로 바꾼다. 구현은 Developer 모듈(ShaderFormatVulkan)에 있고
// 개발 프로그램만 DeviceDesc::shaderCompiler로 넘긴다. Release 실행 파일에는 컴파일러가 들어가지
// 않는다(ShaderSystem P4).
namespace rocks::graphics {

// 셰이더가 쓰는 descriptor 종류다. 값 순서가 binding 번호를 매기는 종류 순서다(ShaderSystem 7.2절
// 3·4항: 상수 버퍼 → 구조화·raw 버퍼 → storage image → SRV 텍스처 → 샘플러).
enum class ShaderBindingKind : uint8 {
    ConstantBuffer,
    StorageBuffer,
    StorageImage,
    SampledImage,
    Sampler
};

// 자원의 모양이다. 같은 종류 안에서 Vulkan descriptor 타입과 Graphics 버퍼 view 종류를 가른다.
enum class ShaderResourceShape : uint8 {
    // 텍스처·storage image(SAMPLED_IMAGE·STORAGE_IMAGE)
    Image,
    // typed buffer(HLSL Buffer<T>·RWBuffer<T>, UNIFORM·STORAGE_TEXEL_BUFFER)
    TexelBuffer,
    // 구조화 버퍼(StructuredBuffer<T>, STORAGE_BUFFER)
    StructuredBuffer,
    // byte address 버퍼(ByteAddressBuffer, STORAGE_BUFFER)
    RawBuffer,
    // 상수 버퍼(UNIFORM_BUFFER)
    ConstantBuffer,
    Sampler
};

// 컴파일한 셰이더의 descriptor binding 하나다(set = 단계, binding = 종류 순서 번호).
struct ShaderBinding {
    uint32 set = 0;
    uint32 binding = 0;
    ShaderBindingKind kind = ShaderBindingKind::ConstantBuffer;
    ShaderResourceShape shape = ShaderResourceShape::Image;
    // HLSL 원문의 register 번호(t·s·b·u 뒤의 수)와 space다(DXC 기본 대응: 고쳐 쓰기 전 binding =
    // register, set = space). 장치가 고정 배치 slot을 찾는 데 쓴다.
    uint32 sourceRegister = 0;
    uint32 sourceSpace = 0;
    // 이미지·texel buffer가 돌려주는 값 계열이다(그 밖은 None).
    PixelShaderType sampledType = PixelShaderType::None;
    // 셰이더가 쓰기도 한다(UAV·storage). 읽기 전용 storage buffer·이미지는 거짓이다.
    bool writable = false;
    // 구조화 버퍼의 원소 byte(런타임 배열의 ArrayStride)다. 그 밖이나 알 수 없으면 0이다.
    uint32 structureStride = 0;
};

// 정점 셰이더가 읽는 입력 하나다. location은 시맨틱 ATTRIBUTEn의 n이다.
struct ShaderVertexInput {
    uint32 location = 0;
    VertexShaderType type = VertexShaderType::Float;
};

// 컴파일 결과다. code는 backend 코드(SPIR-V word)이고 나머지는 장치가 파이프라인 layout·정점
// 배치와 대조할 리플렉션이다.
struct CompiledShader {
    explicit CompiledShader(Allocator& allocator) noexcept
        : code(allocator), vertexInputs(allocator), bindings(allocator) {
    }

    Array<uint32> code;
    // 정점 셰이더만 채운다(시스템 값 입력은 뺀다).
    Array<ShaderVertexInput> vertexInputs;
    Array<ShaderBinding> bindings;
    // 그리기별 상수 블록(cbuffer DrawConstants, push constant)을 읽으면 읽는 끝 byte, 안 읽으면
    // 0이다. 장치는 이 값이 파이프라인의 drawConstantBytes 이하인지 본다.
    uint32 rootConstantBytes = 0;
};

// HLSL 원문을 backend 코드로 바꾸는 쪽이다. 장치가 CreatePipeline 안에서 부르며, 장치 내부
// 잠금으로 직렬화되므로 구현은 한 번에 한 호출만 받는다(장치를 여러 개 만들어 한 훅을 나누면
// 구현이 스스로 잠가야 한다).
//
// 사용 예(개발 프로그램):
//   auto compiler = shader::CreateVulkanShaderCompiler(allocator, diagnostics);   // Developer 모듈
//   graphics::DeviceDesc desc;
//   desc.shaderCompiler = compiler.Value().Get();
//   auto device = graphics::CreateVulkanDevice(allocator, desc);
class ShaderSourceCompiler {
public:
    // stage 진입점(VSMain·PSMain)을 SPIR-V(Vulkan 1.1 환경)로 바꿔 out에 채운다. 실패하면
    // diagnostics에 영어 진단(컴파일러 메시지 포함)을 쓴다. 실패: 컴파일 오류 DependencyFailed,
    // 원문이 개발 경로 규칙(시맨틱 ATTRIBUTEn 등)을 어기면 InvalidArgument, 할당 실패는 그 오류다.
    virtual Result<void> CompileSpirv(ShaderStage stage, StringView hlsl, CompiledShader& out,
                                      String& diagnostics) noexcept = 0;

protected:
    ~ShaderSourceCompiler() = default;
};

} // namespace rocks::graphics
