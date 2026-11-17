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

// 그래프에 넣은 pass handle이다. index는 선언 순서(0부터)이며 진단(culling·수명·전이)에 쓴다.
struct GraphPass {
    uint32 index = GraphIndexNone;

    bool IsNull() const noexcept {
        return index == GraphIndexNone;
    }

    bool operator==(const GraphPass&) const noexcept = default;
};

// pass 종류다(UE ERDGPassFlags의 Raster·Compute·AsyncCompute·Copy). 종류가 허용하는 접근이 다르다:
// - Raster: 렌더 타깃 하나(GraphPassAccesses::RenderTarget·SwapChainTarget)가 있어야 한다. 셰이더
//   읽기는 SrvGraphics·Constant·VertexOrIndex·IndirectArgs. UAV는 검증 오류다(셰이더 시스템 7.2절
//   4항: UAV는 compute만).
// - Compute: SRV·UAV·Constant·IndirectArgs. 렌더 타깃 없음.
// - Copy: CopySource·CopyDestination만(그래픽 pipe).
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

// 렌더 타깃 시작 동작이다(GX4 attachment load). RG1 장치 계약(BeginOffscreen)은 clear만 하므로
// Load는 Unsupported다. DontCare는 clear로 낮춘다(내용이 정해지지 않아도 되므로 clear도 맞다).
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
};

// import 설정이다(7.4절).
struct GraphImportOptions {
    // 지금 장치 자원의 접근 상태다. Unknown이면 첫 전이의 before를 Unknown으로 두어 장치가 추적한
    // 상태에서 바꾸게 한다.
    graphics::ResourceAccess initialAccess = graphics::ResourceAccess::Unknown;
    // 그래프가 끝날 때 돌려 둘 상태다. Unknown이면 마지막으로 쓴 상태 그대로 둔다.
    graphics::ResourceAccess finalAccess = graphics::ResourceAccess::Unknown;
    // 텍스처의 mip 수다(subresource 추적 단위). 장치에 자원 desc 조회가 생기면(GX2) 그쪽에서 읽고
    // 이 칸은 뺀다.
    uint32 mipLevels = 1;
};

// 계산한 전이 하나다(진단·시험). pass는 이 전이를 prologue에서 내는 pass 번호이며
// GraphIndexNone이면 그래프 끝(export·import 마지막 상태)이다. attachment면 그 pass의 렌더 타깃
// 전이라 장치의 BeginOffscreen이 낸다(CommandContext::Transition으로 내지 않는다).
struct GraphTransitionRecord {
    uint32 pass = GraphIndexNone;
    GraphResourceKind kind = GraphResourceKind::Texture;
    uint32 resource = GraphIndexNone;
    // 연속한 mip 범위다(slice는 지금 하나뿐이다).
    uint32 firstMip = 0;
    uint32 mipCount = 1;
    graphics::ResourceAccess before = graphics::ResourceAccess::Unknown;
    graphics::ResourceAccess after = graphics::ResourceAccess::Unknown;
    bool attachment = false;

    bool operator==(const GraphTransitionRecord&) const noexcept = default;
};

// 그래프 자원의 수명이다(참조 수 기준, 7.1절 수명·할당). firstPass에서 할당하고 lastPass 뒤에
// 해제한다(RG1은 해제한 자원을 그래프 끝에 풀로 돌려준다. 프레임 안 재사용은 RG2). 쓰는 pass가 모두
// 제거됐으면 둘 다 GraphIndexNone이다. import·export 자원은 해제하지 않는다(released가 false).
struct GraphLifetime {
    uint32 firstPass = GraphIndexNone;
    uint32 lastPass = GraphIndexNone;
    // 남은(제거되지 않은) pass의 참조 수다.
    uint32 references = 0;
    bool released = false;
};

// 그래프 진단 계수다.
struct GraphStats {
    uint32 passes = 0;
    uint32 culledPasses = 0;
    uint32 executedPasses = 0;
    // 장치에 낸 전이 수(attachment 제외)와 attachment 전이 수다.
    uint32 transitions = 0;
    uint32 attachmentTransitions = 0;
    // 연 장치 기록(BeginOffscreen·BeginFrame) 수다.
    uint32 recordings = 0;
    // 풀에서 새로 만든·재사용한 자원 수다.
    uint32 createdResources = 0;
    uint32 reusedResources = 0;
    uint32 validationErrors = 0;
};

} // namespace rocks::render
