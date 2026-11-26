#pragma once

#include <Core/Containers/Array.h>
#include <Core/Containers/Span.h>
#include <Core/Memory/Allocator.h>
#include <Core/Result.h>
#include <Core/Text/StringView.h>
#include <Graphics/Device.h>
#include <ShaderCore/ShaderBindingLayout.h>

// 자원 지도(Docs/Design/RenderBase 9.13절 5항)다: 손으로 쓴 자원 표(이름·종류)를 단계 layout들과
// 대조해 표 항목마다 단계별 자리(단계, slot, 모양, 원소 타입)를 만든다. 묶음 설정 ParameterBatch가
// 이 지도로 값을 장치에 올린다. P7(ShaderSystem 13절) 전의 입력은 손으로 쓴 표이고, 생성 표는
// SH3이다(RenderBase 18.4절 SM11).
namespace rocks::render {

// 자원 표 항목의 종류다.
enum class ResourceParameterKind : uint8 {
    // SRV 텍스처(텍스처 모양) — CommandContext::SetTexture
    Texture,
    // 자기·공유 샘플러 — CommandContext::SetSampler
    Sampler,
    // typed buffer SRV·구조화·raw SRV — CommandContext::SetBufferView(이름 표 slot)
    BufferView
};

// "Texture"·"Sampler"·"BufferView"다(범위 밖은 "unknown").
ROCKS_RENDERBASE_API const char* ResourceParameterKindName(ResourceParameterKind kind) noexcept;

// 자원 표 항목 하나다. 이름은 셰이더 자원 이름이고 표보다 오래 산다(정적 표).
struct ResourceParameter {
    StringView name;
    ResourceParameterKind kind = ResourceParameterKind::Texture;
};

// 표 항목 하나가 단계 하나에 놓이는 곳이다.
struct ResourcePlacement {
    graphics::ShaderStage stage = graphics::ShaderStage::Pixel;
    // 텍스처·샘플러는 값 1 slot(backend 위치), 버퍼 view는 파이프라인 이름 표 slot이다.
    uint32 slot = 0;
    shader::ShaderResourceShape shape = shader::ShaderResourceShape::None;
    shader::ShaderComponentType component = shader::ShaderComponentType::None;
    // 구조화 버퍼의 셰이더 선언 원소 byte다(모르면 0).
    uint32 structureStride = 0;
    // 공유 샘플러(SharedWrapSampler·SharedClampSampler)다 — 값이 있어야 그린다.
    bool sharedSampler = false;
};

// 표 항목 하나의 최대 자리 수다(그래픽 파이프라인의 VS·PS).
inline constexpr usize MaxResourcePlacements = 2;

// 자원 표 하나의 지도다.
//
// 사용 예(PS가 Texture2D Albedo·SamplerState AlbedoSampler·Buffer<float4> Lights를 읽는다):
//   constexpr ResourceParameter MaterialResources[] = {
//       {"Albedo", ResourceParameterKind::Texture},
//       {"AlbedoSampler", ResourceParameterKind::Sampler},
//       {"Lights", ResourceParameterKind::BufferView}};
//   const shader::ShaderBindingLayout* stages[] = {set.BindingLayout(vs), set.BindingLayout(ps)};
//   auto map = ShaderResourceMap::Build(allocator, MaterialResources, stages, desc.bindings);
class ROCKS_RENDERBASE_API ShaderResourceMap {
public:
    // 표와 단계 layout들(null은 건너뛴다), 파이프라인 이름 표(버퍼 view의 slot)로 만든다. 실패는
    // InvalidArgument이고 reason에 영어 이유(정적 문구)를 둔다: 빈 이름·중복 이름·모르는 종류,
    // 같은 이름의 셰이더 자원이 다른 종류(종류 불일치), 셰이더가 읽는 텍스처·샘플러·읽기 버퍼
    // view가 표에 없음(layout과 표 불일치), 버퍼 view가 이름 표에 없음, 어느 단계도 쓰지 않는 표
    // 항목. 상수 버퍼·compute 쓰기 자원·PS 진단 버퍼는 지도 밖이다. 할당 실패는 OutOfMemory다.
    static Result<ShaderResourceMap> Build(Allocator& allocator,
                                           Span<const ResourceParameter> table,
                                           Span<const shader::ShaderBindingLayout* const> stages,
                                           const graphics::PipelineBindingNames& names,
                                           const char** reason = nullptr) noexcept;

    ShaderResourceMap(ShaderResourceMap&&) noexcept = default;
    ShaderResourceMap& operator=(ShaderResourceMap&&) noexcept = default;

    Span<const ResourceParameter> Table() const noexcept {
        return table_;
    }

    // 표 항목 parameter(0 – Table().Size()-1)의 자리들이다(단계 순서).
    Span<const ResourcePlacement> PlacementsOf(usize parameter) const noexcept;

    // 이름의 표 항목 번호다. 없으면 NotFound다.
    Result<usize> Find(StringView name) const noexcept;

private:
    explicit ShaderResourceMap(Allocator& allocator) noexcept
        : placements_(allocator), counts_(allocator) {
    }

    Span<const ResourceParameter> table_;
    // 항목 i의 자리는 placements_[i * MaxResourcePlacements, + counts_[i])다.
    Array<ResourcePlacement> placements_;
    Array<uint8> counts_;
};

} // namespace rocks::render
