#pragma once

#include <Base/NumericLimits.h>
#include <Graphics/DeviceTypes.h>
#include <Graphics/ResourceAccess.h>

// 렌더 그래프의 값 타입이다(Docs/Design/RenderBase.ko.md 7절). 그래프 자원 handle, pass 종류·표시,
// 그래프 내부 접근 모델의 단위(GraphResourceAccess)와 진단 결과 타입을 둔다.
namespace rocks::render {

// "없음" 번호다(pass·자원 번호 자리).
inline constexpr uint32 GraphIndexNone = NumericLimits<uint32>::Max();

// 그래프 자원 종류다.
enum class GraphResourceKind : uint8 {
    Texture,
    Buffer
};

// 그래프 텍스처 handle이다(UE FRDGTextureRef 대응). 만든 그래프 안에서만 의미가 있다. graph는
// 그래프 일련번호로, 다른 그래프의 handle을 쓰면 검증 오류다.
struct GraphTexture {
    uint32 index = GraphIndexNone;
    uint32 graph = 0;

    bool IsNull() const noexcept {
        return index == GraphIndexNone;
    }

    bool operator==(const GraphTexture&) const noexcept = default;
};

// 그래프 버퍼 handle이다(UE FRDGBufferRef 대응).
struct GraphBuffer {
    uint32 index = GraphIndexNone;
    uint32 graph = 0;

    bool IsNull() const noexcept {
        return index == GraphIndexNone;
    }

    bool operator==(const GraphBuffer&) const noexcept = default;
};

// 그래프 텍스처 view handle이다(UE FRDGTextureSRVRef·FRDGTextureUAVRef 대응, 7.5절 mip view 그래프
// 자원). (텍스처, mip 범위, slice 범위, 읽기·쓰기)이며 RenderGraph::CreateTextureView가 만든다.
// 만든 그래프 안에서만 의미가 있다.
struct GraphTextureView {
    uint32 index = GraphIndexNone;
    uint32 graph = 0;

    bool IsNull() const noexcept {
        return index == GraphIndexNone;
    }

    bool operator==(const GraphTextureView&) const noexcept = default;
};

// 그래프에 넣은 pass handle이다. index는 선언 순서(0부터)이며 진단(culling·수명·전이)에 쓴다.
struct GraphPass {
    uint32 index = GraphIndexNone;

    bool IsNull() const noexcept {
        return index == GraphIndexNone;
    }

    bool operator==(const GraphPass&) const noexcept = default;
};

// pass 종류다(UE ERDGPassFlags의 Raster·Compute·AsyncCompute·Copy). 종류가 허용하는 접근이 다르다:
// - Raster: attachment(GraphPassAccesses::RenderTarget·ColorTarget·DepthTarget) 또는 스왑체인
//   타깃(SwapChainTarget)이 있어야 한다. 셰이더
//   읽기는 SrvGraphics·Constant·VertexOrIndex·IndirectArgs. UAV는 검증 오류다(셰이더 시스템 7.2절
//   4항: UAV는 compute만).
// - Compute: SRV·UAV·Constant·IndirectArgs. 렌더 타깃 없음(열린 기록이 없으면 attachment 없는
//   기록을 연다).
// - Copy: CopySource·CopyDestination만(그래픽 pipe, 기록은 Compute와 같다).
// - AsyncCompute: RG4에서 연다. 지금은 Unsupported다.
enum class GraphPassKind : uint8 {
    Raster,
    Compute,
    AsyncCompute,
    Copy
};

// pass 표시다(비트 조합).
enum class GraphPassFlags : uint8 {
    None = 0,
    // 결과를 그래프 밖에서 관찰하는 pass다(readback·present·디버그 출력, UE NeverCull). 미사용 제거
    // 대상이 아니며 그 pass가 읽는 자원의 생산자도 살린다.
    HasSideEffects = 1u << 0
};

constexpr GraphPassFlags operator|(GraphPassFlags left, GraphPassFlags right) noexcept {
    return static_cast<GraphPassFlags>(static_cast<uint8>(left) | static_cast<uint8>(right));
}

constexpr bool HasGraphPassFlag(GraphPassFlags value, GraphPassFlags flag) noexcept {
    return (static_cast<uint8>(value) & static_cast<uint8>(flag)) != 0;
}

// 접근 하나의 표시다.
enum class GraphAccessFlags : uint8 {
    None = 0,
    // UAV 쓰기끼리 순서가 필요 없다(겹쳐도 되는 쓰기, UE
    // ERDGUnorderedAccessViewFlags::SkipBarrier). 연속한 두 UAV 접근이 모두 이 표시를 가지면 그
    // 사이 UAV barrier를 생략한다.
    NoUavBarrier = 1u << 0
};

// 그래프 실행 방식이다.
enum class GraphMode : uint8 {
    // pass를 모두 모은 뒤 Execute가 culling·수명·전이를 계산해 선언 순서대로 실행한다.
    Deferred,
    // 디버그용이다(UE 즉시 실행 모드). AddPass가 그 pass를 곧바로 할당·전이·실행한다. 미사용 제거와
    // pass 사이 읽기 상태 병합(앞을 내다봐야 한다)은 하지 않는다. 장치 호출 열은 전이를 뺀 나머지가
    // Deferred와 같고, 각 pass가 보는 자원 상태도 같다.
    Immediate
};

// attachment 시작 동작이다(graphics::LoadAction에 대응). Load는 이전 내용을 읽으므로 생산 전
// 읽기 검사를 받는다. DontCare는 이전 내용을 버린다(다 덮어 그리는 pass).
enum class GraphLoadAction : uint8 {
    Clear,
    DontCare,
    Load
};

// 그래프 내부 접근 모델의 단위다: pass 하나가 자원 하나의 subresource 범위를 어떤 접근으로 쓰는지
// (UE FRDGPass의 자원 상태 목록 대응). 모든 pass 선언은 결국 이 목록이 된다. 지금은
// GraphPassAccesses가 명시 선언으로 만들고, 셰이더 파라미터 구조체(결정 R4, 9.6절)가 들어오면 그
// 구조체 layout을 훑는 수집기가 같은 목록을 만든다(UE도 내부는 접근 목록이다).
struct GraphResourceAccess {
    GraphResourceKind kind = GraphResourceKind::Texture;
    uint32 resource = GraphIndexNone;
    // 자원 handle의 그래프 일련번호다(다른 그래프 handle 검사).
    uint32 graph = 0;
    // 텍스처만 쓴다(버퍼는 전체 하나).
    graphics::SubresourceRange range;
    graphics::ResourceAccess access = graphics::ResourceAccess::Unknown;
    GraphAccessFlags flags = GraphAccessFlags::None;
    // GraphTextureView 선언이면 그 view 번호다(resource·range는 AddPass가 view에서 채운다).
    uint32 view = GraphIndexNone;
};

// import 설정이다(7.4절).
struct GraphImportOptions {
    // 지금 장치 자원의 접근 상태다. Unknown이면 첫 전이의 before를 Unknown으로 두어 장치가 추적한
    // 상태에서 바꾸게 한다.
    graphics::ResourceAccess initialAccess = graphics::ResourceAccess::Unknown;
    // 그래프가 끝날 때 돌려 둘 상태다. Unknown이면 마지막으로 쓴 상태 그대로 둔다.
    graphics::ResourceAccess finalAccess = graphics::ResourceAccess::Unknown;
    // 텍스처의 mip 수와 slice 수(TextureSliceCount: 배열 층·cube 면)다(subresource 추적 단위
    // mip × slice). 장치가 Device::GetTextureSubresources를 지원하면 import가 대조하고 다르면
    // InvalidArgument다(지원하지 않는 장치는 이 선언을 그대로 믿는다).
    uint32 mipLevels = 1;
    uint32 slices = 1;
};

// 계산한 전이 하나다(진단·시험). pass는 이 전이를 prologue에서 내는(분리 전이면 end하는) pass
// 번호이며 GraphIndexNone이면 그래프 끝(export·import 마지막 상태)이다. attachment면 그 pass의
// attachment subresource 전이라 장치의 BeginPass가 낸다(CommandContext::Transition으로 내지
// 않는다).
//
// beginPass가 GraphIndexNone이 아니면 분리 전이다(RG2): 그 subresource를 마지막으로 쓴 pass
// beginPass의 epilogue에서 begin하고 pass의 prologue에서 end한다(UE와 같이 같은 pipe에서 생산 pass
// epilogue begin, 소비 pass prologue end). 지금은 begin과 end가 같은 장치 기록 안이고 그 사이에
// 다른 pass가 하나 이상 있을 때만 나눈다(Raster pass는 새 기록을 열므로 Compute·Copy pass가
// end한다).
//
// 만든 자원의 첫 전이 before는 Unknown(내용 불필요, 장치가 추적한 상태에서 바꾼다)이고, 같은 그래프
// 안에서 앞 자원이 놓은 장치 객체를 이어 쓰는 자원(GraphLifetime::reusedWithinGraph)은 Discard다
// (이전 내용 무효, backend는 Unknown처럼 추적 상태에서 바꾼다).
struct GraphTransitionRecord {
    uint32 pass = GraphIndexNone;
    GraphResourceKind kind = GraphResourceKind::Texture;
    uint32 resource = GraphIndexNone;
    // 연속한 mip 범위와 slice 범위다(그 직사각형 안의 subresource가 모두 같은 전이다).
    uint32 firstMip = 0;
    uint32 mipCount = 1;
    uint32 firstSlice = 0;
    uint32 sliceCount = 1;
    graphics::ResourceAccess before = graphics::ResourceAccess::Unknown;
    graphics::ResourceAccess after = graphics::ResourceAccess::Unknown;
    bool attachment = false;
    uint32 beginPass = GraphIndexNone;

    bool operator==(const GraphTransitionRecord&) const noexcept = default;
};

// 그래프 자원의 수명이다(참조 수 기준, 7.1절 수명·할당). firstPass에서 할당하고 lastPass 뒤에
// 해제한다. 해제한 장치 객체는 같은 그래프의 뒤 pass에서 처음 쓰는 같은 desc 자원이 이어 쓰고(RG2
// 프레임 안 재사용, Deferred 모드), 그래프 끝에 풀로 돌아간다. 쓰는 pass가 모두 제거됐으면 둘 다
// GraphIndexNone이다. import·export 자원은 해제하지 않는다(released가 false). 업로드를 받는 자원은
// 그래프 시작에 할당한다(그래서 앞 자원의 객체를 이어 쓰지 않는다).
struct GraphLifetime {
    uint32 firstPass = GraphIndexNone;
    uint32 lastPass = GraphIndexNone;
    // 남은(제거되지 않은) pass의 참조 수다.
    uint32 references = 0;
    bool released = false;
    // 같은 그래프 안에서 앞 자원이 놓은 장치 객체를 이어 쓴다(첫 전이 before가 Discard).
    bool reusedWithinGraph = false;
};

// 그래프 진단 계수다.
struct GraphStats {
    uint32 passes = 0;
    uint32 culledPasses = 0;
    uint32 executedPasses = 0;
    // 장치에 낸 전이 수(attachment 제외, 분리 전이 포함)와 attachment 전이 수다.
    uint32 transitions = 0;
    // 그 가운데 begin·end로 나눠 낸 전이 수다.
    uint32 splitTransitions = 0;
    uint32 attachmentTransitions = 0;
    // 연 장치 기록(BeginPass·BeginFrame) 수다.
    uint32 recordings = 0;
    // 풀에서 새로 만든·재사용한 자원 수다(풀에서 받은 장치 객체 기준).
    uint32 createdResources = 0;
    uint32 reusedResources = 0;
    // 같은 그래프 안에서 앞 자원이 놓은 장치 객체를 이어 쓴 자원 수다(풀에 가지 않는다, RG2).
    uint32 reusedWithinGraph = 0;
    // 장치에 낸 업로드 수다(버퍼 업로드 하나, 텍스처 mip 하나가 1).
    uint32 uploads = 0;
    uint32 validationErrors = 0;
    // worker에서 기록 목록으로 기록한 pass 수다(RG3 병렬 기록, 직렬이면 0).
    uint32 parallelPasses = 0;
};

} // namespace rocks::render
