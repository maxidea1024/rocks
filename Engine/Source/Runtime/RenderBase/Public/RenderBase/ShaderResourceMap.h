#pragma once

#include <Core/Containers/Array.h>
#include <Core/Containers/Span.h>
#include <Core/Memory/Allocator.h>
#include <Core/Result.h>
#include <Core/Text/StringView.h>
#include <Graphics/Device.h>
#include <ShaderCore/ShaderBindingLayout.h>
#include <ShaderCore/ShaderParameterLayout.h>

// 자원 지도(Docs/Design/RenderBase 9.13절 5항)다: 자원 표를 단계 layout들과 대조해 표 항목마다
// 단계별 자리(단계, slot, 모양, 원소 타입)를 만든다. 묶음 설정 ParameterBatch와 렌더 그래프 pass
// 문맥(GraphPassContext::SetParameters)이 이 지도로 값을 장치에 올린다. 입력은 둘이다: 손으로 쓴
// 자원 표(이름·종류 — 이름으로 찾는다)와 생성 파라미터 구조체 표(ShaderSystem 16.2절 SSD64 — 자원
// 멤버를 바인딩 layout v5의 구조체 identity·멤버 오프셋으로 찾는다).
namespace rocks::render {

// 자원 표 항목의 종류다.
enum class ResourceParameterKind : uint8 {
    // SRV 텍스처(텍스처 모양) — CommandContext::SetTexture
    Texture,
    // 자기·공유 샘플러 — CommandContext::SetSampler
    Sampler,
    // typed buffer SRV·구조화·raw SRV — CommandContext::SetBufferView(이름 표 slot)
    BufferView,
    // storage image(UAV 텍스처) — CommandContext::SetWritableTexture(이름 표 slot). 생성 표만이다.
    WritableTexture,
    // 구조화·raw·typed 버퍼 UAV — CommandContext::SetWritableBufferView(이름 표 slot). 생성
    // 표만이다.
    WritableBuffer
};

// "Texture"·"Sampler"·"BufferView"·"WritableTexture"·"WritableBuffer"다(범위 밖은 "unknown").
ROCKS_RENDERBASE_API const char* ResourceParameterKindName(ResourceParameterKind kind) noexcept;

// 자원 표 항목 하나다. 이름은 셰이더 자원 이름이고 표보다 오래 산다(정적 표).
struct ResourceParameter {
    StringView name;
    ResourceParameterKind kind = ResourceParameterKind::Texture;
    // 생성 표 항목이면 값이 놓인 자원 멤버의 C++ 오프셋(짝 샘플러는 그 텍스처 멤버)과 그 종류다.
    // 손으로 쓴 표와 공유 샘플러는 shader::NoShaderParameterMember다.
    uint32 memberOffset = shader::NoShaderParameterMember;
    shader::ShaderParameterResourceKind memberKind =
        shader::ShaderParameterResourceKind::DeviceTexture;

    bool HasMember() const noexcept {
        return memberOffset != shader::NoShaderParameterMember;
    }
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

    // 생성 파라미터 구조체 표(ShaderParameterLayoutOf<T>())로 만든다(ShaderSystem 16.2절 SSD64).
    // 항목은 표의 셰이더 자원 멤버(SRV 텍스처와 짝 샘플러, 버퍼 SRV, UAV)와 단계가 읽는 공유
    // 샘플러(멤버 없음 — 이름으로 값을 준다)다. 단계 자원은 이름이 아니라 (구조체 identity, 멤버
    // 오프셋, 종류)로 찾는다. 셰이더가 쓰지 않는 멤버는 자리 없는 항목이다(오류 아님). 상수 버퍼와
    // 멤버가 아닌 쓰기 자원(PS 진단 버퍼 등)은 지도 밖이다. 실패는 InvalidArgument이고 reason에
    // 영어 이유를 둔다: 표가 생성 표가 아니거나 규칙 위반(ValidateShaderParameterLayout), 단계가
    // 읽는 텍스처·샘플러·버퍼 view가 이 구조체의 멤버가 아님(공유 샘플러 제외), 같은 멤버 오프셋의
    // 다른 종류, 버퍼 view·쓰기 자원이 이름 표에 없음, 세 단계 이상. 할당 실패는 OutOfMemory다.
    // 표는 지도보다 오래 산다.
    //
    // 사용 예:
    //   const shader::ShaderBindingLayout* stages[] = {set.BindingLayout(vs),
    //   set.BindingLayout(ps)}; auto map = ShaderResourceMap::Build(allocator,
    //   shader::ShaderParameterLayoutOf<DecalParameters>(),
    //                                       stages, desc.bindings);
    static Result<ShaderResourceMap> Build(Allocator& allocator,
                                           const shader::ShaderParameterLayout& layout,
                                           Span<const shader::ShaderBindingLayout* const> stages,
                                           const graphics::PipelineBindingNames& names,
                                           const char** reason = nullptr) noexcept;

    ShaderResourceMap(ShaderResourceMap&&) noexcept = default;
    ShaderResourceMap& operator=(ShaderResourceMap&&) noexcept = default;

    Span<const ResourceParameter> Table() const noexcept {
        return table_;
    }

    // 생성 표로 만든 지도의 표다(손으로 쓴 표 지도는 null).
    const shader::ShaderParameterLayout* Layout() const noexcept {
        return layout_;
    }

    // 표 항목 parameter(0 – Table().Size()-1)의 자리들이다(단계 순서).
    Span<const ResourcePlacement> PlacementsOf(usize parameter) const noexcept;

    // 이름의 표 항목 번호다. 없으면 NotFound다.
    Result<usize> Find(StringView name) const noexcept;

private:
    explicit ShaderResourceMap(Allocator& allocator) noexcept
        : entries_(allocator), placements_(allocator), counts_(allocator) {
    }

    // 단계 자원 하나의 자리를 항목 entry에 더한다(버퍼 view·쓰기 자원은 이름 표 slot).
    const char* Place(usize entry, graphics::ShaderStage stage,
                      const shader::ShaderBindingResource& resource,
                      const graphics::PipelineBindingNames& names) noexcept;

    Span<const ResourceParameter> table_;
    const shader::ShaderParameterLayout* layout_ = nullptr;
    // 생성 표 지도가 만든 항목이다(table_이 가리킨다).
    Array<ResourceParameter> entries_;
    // 항목 i의 자리는 placements_[i * MaxResourcePlacements, + counts_[i])다.
    Array<ResourcePlacement> placements_;
    Array<uint8> counts_;
};

} // namespace rocks::render
