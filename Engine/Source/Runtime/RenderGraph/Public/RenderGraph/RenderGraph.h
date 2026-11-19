#pragma once

#include <Core/Memory/FrameArena.h>
#include <Core/Move.h>
#include <Graphics/Device.h>
#include <RenderBase/GpuTrace.h>
#include <RenderBase/RenderHandle.h>
#include <RenderGraph/GraphPassAccesses.h>
#include <RenderGraph/GraphResourcePool.h>
#include <RenderGraph/GraphTypes.h>
#include <TraceCapture/TraceCpuScope.h>

// 렌더 그래프다(Docs/Design/RenderBase.ko.md 7·8절 RG1·RG2, UE FRDGBuilder 대응).
namespace rocks::render {

class RenderFrameContext;
class RenderGraph;
class GraphExecutionAccess;

namespace detail {
struct GraphState;
} // namespace detail

// pass 기록 함수가 받는 문맥이다. 기록 함수 호출 동안만 유효하며 저장하지 않는다.
class GraphPassContext {
public:
    // 이 pass를 기록하는 장치 기록 객체다. Raster pass는 그 렌더 타깃이 묶인 기록이다.
    graphics::CommandContext& Commands() const noexcept {
        return commands_;
    }

    graphics::Device& Device() const noexcept;

    // 그래프 자원의 장치 handle이다. 이 pass가 접근을 선언하지 않은 자원이나 다른 그래프의
    // handle이면 null handle이다(그 그리기는 장치가 무시하거나 흰 텍스처를 쓴다).
    graphics::TextureHandle Texture(GraphTexture texture) const noexcept;
    graphics::BufferHandle Buffer(GraphBuffer buffer) const noexcept;

    GraphPass Pass() const noexcept {
        return GraphPass{pass_};
    }

private:
    GraphPassContext(RenderGraph& graph, graphics::CommandContext& commands, uint32 pass) noexcept
        : graph_(graph), commands_(commands), pass_(pass) {
    }

    RenderGraph& graph_;
    graphics::CommandContext& commands_;
    uint32 pass_;

    friend class RenderGraph;
    friend class GraphExecutionAccess;
};

// pass 기록 함수의 형식 지운 실행기다. 그래프가 프레임 arena에 만들고, 그래프 파괴 때(렌더
// lane에서) Destroy로 부순다. 보통 AddPass 템플릿이 람다를 감싸 만든다.
class GraphPassExecutor {
public:
    virtual void Execute(GraphPassContext& context) noexcept = 0;
    // 소멸자를 부른다(메모리는 프레임 arena가 비운다).
    virtual void Destroy() noexcept = 0;

protected:
    ~GraphPassExecutor() = default;
};

namespace detail {

template <class Function>
class GraphLambdaExecutor final : public GraphPassExecutor {
public:
    explicit GraphLambdaExecutor(Function&& function) noexcept : function_(Move(function)) {
    }

    void Execute(GraphPassContext& context) noexcept override {
        function_(context);
    }

    void Destroy() noexcept override {
        this->~GraphLambdaExecutor();
    }

private:
    Function function_;
};

} // namespace detail

// 그래프 생성 설정이다. 포인터는 모두 그래프보다 오래 살아야 한다.
struct RenderGraphDesc {
    graphics::Device* device = nullptr;
    // import·export가 쓰는 렌더 handle 표다(lease·export handle 예약).
    RenderHandleTable* handles = nullptr;
    GraphResourcePool* pool = nullptr;
    // 그래프 수명 할당(pass·접근 목록·내부 표)이다. 그래프가 끝난 뒤에 Reset되어야 한다(렌더 lane의
    // 프레임 slot arena는 그 프레임의 GPU 완료 뒤 Reset된다).
    // 필수다(null이면 Fatal). device·handles·pool이 null이면 그래프의 모든 호출이 OutOfMemory다.
    FrameArena* arena = nullptr;
    // 있으면 pass마다 GpuScope(pass 이름) 구간을 두고, 그래프의 장치 기록 전체를 GPU 프레임 하나로
    // 묶는다(GpuTrace::BeginFrame·EndFrame을 그래프가 부른다, 13절).
    GpuTrace* trace = nullptr;
    // 프레임 번호다(풀 축출 나이·GPU 프레임 번호).
    uint64 frame = 0;
    GraphMode mode = GraphMode::Deferred;
    // 분리 전이(begin·end)를 쓴다(Deferred만, GraphTransitionRecord::beginPass). 끄면 모든 전이를
    // 소비 pass prologue에서 한 번에 낸다(분리 켬·끔 대조 시험·디버그).
    bool splitTransitions = true;
};

// 렌더 lane 프레임 작업에서 그래프 설정을 만든다: 장치·handle 표·그 slot의 프레임 arena·프레임
// 번호를 문맥에서 가져온다(5절 렌더 lane·프레임 slot·FrameArena 연결).
RenderGraphDesc MakeRenderGraphDesc(RenderFrameContext& frame, GraphResourcePool& pool,
                                    GpuTrace* trace = nullptr,
                                    GraphMode mode = GraphMode::Deferred) noexcept;

// 렌더 그래프다. 한 프레임(또는 offscreen 작업 하나)의 pass를 모아 선언 순서대로 실행한다(결정 R5:
// 재배치 없음). 범위(RG1·RG2 A부): 단일 graphics queue, aliasing·render pass 병합 끔, 단일 스레드
// 기록, 같은 그래프 안 풀 재사용, 그래프 안 업로드. begin·end 분리 전이는 RG2 B부다.
//
// 사용 예(렌더 lane 프레임 작업):
//   static TraceScopeName SceneName("Scene");
//   static TraceScopeName TonemapName("Tonemap");
//   RenderGraph graph(MakeRenderGraphDesc(context, renderer.pool, &renderer.gpuTrace));
//   auto scene = graph.CreateTexture({.width = w, .height = h,
//                                     .usage = graphics::TextureUsage::RenderTarget},
//                                     "SceneColor");
//   auto output = graph.ImportTexture(packet.output, "Output");
//   GraphPassAccesses sceneAccesses(graph);
//   sceneAccesses.RenderTarget(scene.Value(), GraphLoadAction::Clear, {0, 0, 0, 1});
//   graph.AddPass(SceneName, GraphPassKind::Raster, GraphPassFlags::None, sceneAccesses,
//                 [=](GraphPassContext& pass) noexcept { ... pass.Commands().Draw(3); });
//   GraphPassAccesses tonemapAccesses(graph);
//   tonemapAccesses.Texture(scene.Value(), graphics::ResourceAccess::SrvGraphics)
//       .RenderTarget(output.Value());
//   graph.AddPass(TonemapName, GraphPassKind::Raster, GraphPassFlags::None, tonemapAccesses,
//                 [=](GraphPassContext& pass) noexcept {
//                     pass.Commands().SetTexture(pass.Texture(scene.Value()));
//                     pass.Commands().Draw(3);
//                 });
//   auto history = graph.ExportTexture(scene.Value());   // 다음 프레임에 ImportTexture로 읽는다
//   graph.Execute();
//   ... 다음 프레임에 새 history를 export한 뒤 lane.Retire(previousHistory)  // 풀 반납(결정 R15)
//
// 업로드(그래프 시작에 장치 업로드 경로로 올린다):
//   auto lights = graph.CreateBuffer({.size = bytes, .usage = BufferUsage::ShaderRead}, "Lights");
//   graph.UploadBuffer(lights.Value(), lightBytes);       // 첫 pass보다 먼저 선언한다
//   accesses.Buffer(lights.Value(), graphics::ResourceAccess::SrvGraphics);   // 전이는 그래프가
//   낸다
//
// 동작:
// - 의존: pass는 자원 접근 목록(GraphResourceAccess, subresource 범위·접근 상태)으로 의존을
// 선언한다.
//   쓰기 접근이 그 subresource의 마지막 생산자가 되고, 뒤 pass의 접근은 그 생산자에 기댄다.
// - 미사용 제거(점진 culling): 부수 효과 pass(HasSideEffects·스왑체인 타깃), import 자원에 쓰는
//   pass(외부 출력), export 자원의 마지막 생산자에서 생산자 간선을 거꾸로 따라 살린 pass만
//   실행한다. 앞의 둘은 AddPass 때, export는 Execute 때 살린다.
// - 수명: 남은 pass의 참조 수로 첫 사용 pass에서 장치 객체를 받고 마지막 사용 pass 뒤에 놓는다
//   (GraphLifetime). Deferred는 놓인 객체를 뒤 pass에서 처음 쓰는 같은 desc 자원이 이어 쓴다(같은
//   그래프 안 재사용, 첫 전이 before Discard). 풀 반환은 그래프 끝이다. Immediate는 앞을 내다볼 수
//   없어 같은 그래프 안 재사용을 하지 않는다.
// - 업로드: UploadBuffer·UploadTexture가 선언한 CPU 데이터를 Deferred는 그래프 시작(첫 기록 앞),
//   Immediate는 그 자원의 첫 pass 직전에 장치 업로드 경로로 올린다. 업로드가 생산자이므로 뒤 pass는
//   생산자 pass 없이 읽을 수 있고, 읽기 전이는 장치가 추적한 상태에서 그래프가 낸다(장치 업로드는
//   상태 중립이다, UE도 Unknown → CopyDest → Unknown). 남은 pass가 쓰지 않고 export도 아닌 자원의
//   업로드는 하지 않는다.
// - 전이: subresource마다 상태를 추적하며, 연속한 pass의 접근이 합칠 수 있으면(읽기끼리, 같은 UAV에
//   생략 표시) 한 상태로 합쳐 그 첫 pass의 prologue에서 한 번만 바꾼다. 그래서 읽기 → 읽기 전이는
//   없고 UAV → UAV는 생략 표시가 없을 때만 UAV barrier다. 전이는 그 pass 기록 함수 앞에 낸다.
//   Deferred는 같은 장치 기록 안에서 생산 pass와 소비 pass 사이에 다른 pass가 있으면 생산 pass
//   끝에 begin하고 소비 pass 앞에서 end한다(분리 전이, CommandContext::BeginTransitions). Raster
//   pass의 렌더 타깃 전이는 장치 BeginOffscreen이 낸다(attachment).
// - 실행: Raster pass마다 장치 기록(BeginOffscreen·BeginFrame … EndFrame)을 하나 연다. Compute·Copy
//   pass는 열린 기록에 이어 쓴다(기록 없는 그래프 앞부분의 Compute·Copy pass는 GX4 전까지
//   Unsupported). pass마다 CPU 구간과(trace가 있으면) GpuScope 구간을 둔다.
// - 검증(항상): 생산 전 읽기, 한 pass 안 같은 subresource의 상충 접근, Raster pass의 UAV, pass
// 종류에
//   맞지 않는 접근, Retire된 자원 import, 생산 전 export, 다른 그래프의 handle. 실패한 호출은
//   InvalidArgument(import는 lease 오류)를 돌려주고 LastError에 영어 문구를 남기며, 그 뒤 Execute는
//   InvalidArgument다(그래프 전체를 버린다).
//
// 스레드: 장치 소유 스레드(렌더 lane) 전용이다. 수명: 렌더 lane 프레임 작업 안에서 만들고 부순다.
// 파괴는 pass 실행기를 부수고 남은 lease를 놓는다(Execute하지 않았으면 풀 자원·export handle도
// 정리한다).
class RenderGraph {
public:
    explicit RenderGraph(const RenderGraphDesc& desc) noexcept;
    ~RenderGraph() noexcept;

    RenderGraph(const RenderGraph&) = delete;
    RenderGraph& operator=(const RenderGraph&) = delete;

    // --- 자원 ---

    // 그래프 수명 텍스처를 선언한다. 장치 자원은 첫 사용 pass 직전에 받는다(같은 그래프 안 재사용
    // 자리 또는 풀). usage는 둘 가운데 하나다:
    // - RenderTarget: GPU가 채운다. 형식은 번호표 안의 비압축 PixelFormat이다.
    // - Sampled: 내용을 UploadTexture로만 받는다(모든 mip, pass는 읽기만). 압축 형식도 된다.
    // name은 정적 수명 문자열(진단)이다. 실패: 잘못된 desc InvalidArgument, arena 할당 실패는 그
    // 오류다. 장치가 지원하지 않는 형식은 풀 할당 때 장치 오류(Unsupported)로 Execute가 실패한다.
    Result<GraphTexture> CreateTexture(const graphics::TextureDesc& desc,
                                       const char* name) noexcept;

    // 그래프 수명 버퍼를 선언한다. 풀에서 새로 만든 버퍼는 0으로 채워져 있고, 다시 쓰는 버퍼의
    // 내용은 정해지지 않는다(업로드하거나 pass가 쓴다).
    Result<GraphBuffer> CreateBuffer(const graphics::BufferDesc& desc, const char* name) noexcept;

    // --- 그래프 안 업로드(RG2, UE QueueBufferUpload 대응) ---
    //
    // 만든 자원에 CPU 데이터를 올린다고 선언한다. 데이터는 이 호출 안에서 프레임 arena에 복사한다
    // (호출 뒤 원본을 바꿔도 된다). 업로드는 그 자원의 생산자이고, 자원을 쓰는 어느 pass보다 먼저
    // 선언해야 한다(업로드는 첫 pass 앞에서 실행된다). 업로드를 받는 자원은 그래프 시작에
    // 할당하므로 같은 그래프의 앞 자원 객체를 이어 쓰지 않는다.

    // 버퍼 [offset, offset + data.Size())에 올린다. 같은 버퍼에 여러 번 선언하면 선언 순서대로
    // 올린다. 올리지 않은 구간의 내용은 정해지지 않는다(다시 쓰는 풀 버퍼). 실패: 다른 그래프
    // handle·import 자원·이미 pass가 선언한 자원·빈 data·범위 밖 InvalidArgument, arena 할당 실패는
    // 그 오류다.
    //
    // 사용 예:
    //   graph.UploadBuffer(constants.Value(), Span<const uint8>(bytes, size));
    Result<void> UploadBuffer(GraphBuffer buffer, Span<const uint8> data,
                              uint64 offset = 0) noexcept;

    // Sampled 그래프 텍스처의 mip 하나를 올린다(pixels는 빈틈 없는 행, 첫 행이 위,
    // PixelFormatImageBytes 크기). 모든 mip을 한 번씩 올려야 한다(장치 생성에 mip 사슬 전체가
    // 필요하다: 빠진 mip은 Execute의 검증 오류). 실패: 위 공통 조건, mip 범위 밖·크기 불일치·같은
    // mip 두 번 InvalidArgument, RenderTarget 텍스처 Unsupported(버퍼 → 텍스처 복사 명령이 없다,
    // GX4).
    Result<void> UploadTexture(GraphTexture texture, uint32 mipLevel,
                               Span<const uint8> pixels) noexcept;

    // 렌더 handle의 장치 자원을 그래프에 들인다(7.4절). handle의 lease를 얻어 그래프가 끝날 때까지
    // 잡는다. 실패: Retired·모르는 handle StaleHandle(LastError "retired"), 종류 TypeMismatch,
    // Pending·Failed NotFound. 호출자는 내장 typed fallback을 import하거나 그 pass를 넣지
    // 않는다(그래프는 조용히 대체하지 않는다).
    Result<GraphTexture> ImportTexture(RenderHandle handle, const char* name,
                                       const GraphImportOptions& options = {}) noexcept;
    Result<GraphBuffer> ImportBuffer(RenderHandle handle, const char* name,
                                     const GraphImportOptions& options = {}) noexcept;

    // 자원을 그래프 밖으로 내보낸다(7.4절, history 경로). 그래프가 끝날 때 finalAccess로 바꾸고,
    // 만든 자원이면 그 풀 자원을 새 렌더 handle에 잇는다(돌려준 handle은 Execute가 성공하면 Ready,
    // 실패하면 Failed가 된다). 자원은 풀 소속 그대로이고 풀이 그 handle의 Retire 대행자다(결정
    // R15): handle의 Retire(렌더 lane Retire 또는 ReleaseRetiredResource)는 장치 해제가 아니라
    // 마지막 사용 fence 뒤 풀 반납이며, 같은 desc의 다음 할당이 다시 쓴다. import 자원이면 그
    // handle을 그대로 돌려주고 마지막 상태만 정한다. export 자원의 마지막 생산자는 미사용 제거되지
    // 않는다. 실패: 다른 그래프 handle·이미 export InvalidArgument, handle 예약 실패는 그 오류다.
    Result<RenderHandle> ExportTexture(
        GraphTexture texture,
        graphics::ResourceAccess finalAccess = graphics::ResourceAccess::SrvGraphics) noexcept;
    Result<RenderHandle> ExportBuffer(
        GraphBuffer buffer,
        graphics::ResourceAccess finalAccess = graphics::ResourceAccess::VertexOrIndex) noexcept;

    // --- pass ---

    // pass를 넣는다. execute는 void(GraphPassContext&) noexcept 람다이며 프레임 arena에 옮겨
    // 담는다. Immediate 모드면 이 안에서 실행까지 한다. 실패: 검증 오류 InvalidArgument, 아직
    // 지원하지 않는 형태(AsyncCompute, Load, 기록 없는 Compute·Copy) Unsupported, 장치·풀 실패는 그
    // 오류다.
    template <class Execute>
    Result<GraphPass> AddPass(TraceScopeName& name, GraphPassKind kind, GraphPassFlags flags,
                              const GraphPassAccesses& accesses, Execute&& execute) noexcept;

    // 위 AddPass의 형식 지운 경로다(파라미터 구조체 수집기가 만든 접근 목록도 이리로 들어온다).
    // executor는 arena에 만든 실행기이며 성공·실패와 무관하게 그래프가 소유한다.
    Result<GraphPass> AddPassWithAccesses(TraceScopeName& name, GraphPassKind kind,
                                          GraphPassFlags flags,
                                          Span<const GraphResourceAccess> accesses,
                                          const GraphRenderTarget& target,
                                          GraphPassExecutor* executor) noexcept;

    // 그래프를 실행한다(Deferred: culling·수명·전이 계산 → 선언 순서 실행, Immediate: 마무리).
    // 마지막 전이·제출(EndFrame), 풀 반환·export handle 연결·lease 반환·풀 축출까지 한다. 한 번만
    // 부른다. 실패: 앞선 검증 오류 InvalidArgument, 다시 부르면 Busy, 장치 실패는 그 오류(그래도
    // 정리는 끝까지 한다).
    Result<void> Execute() noexcept;

    // --- 진단(Execute 뒤, 시험·디버그) ---

    // pass가 미사용 제거되었는지다(Deferred는 Execute 뒤에 확정).
    bool IsCulled(GraphPass pass) const noexcept;
    GraphLifetime Lifetime(GraphTexture texture) const noexcept;
    GraphLifetime Lifetime(GraphBuffer buffer) const noexcept;
    // 계산한 전이 열이다(pass 순서, 같은 pass 안은 접근 선언 순서). Immediate는 실행한 순서다.
    Span<const GraphTransitionRecord> Transitions() const noexcept;
    GraphStats Stats() const noexcept;
    // 마지막 검증·실행 오류 문구(영어)다. 없으면 빈 문자열이다.
    StringView LastError() const noexcept;

    FrameArena& Arena() const noexcept {
        return arena_;
    }

private:
    friend class GraphPassContext;
    friend class GraphExecutionAccess;

    // 실행기를 arena에 만든다. 실패하면 null이다.
    template <class Execute>
    GraphPassExecutor* MakeExecutor(Execute&& execute) noexcept;

    FrameArena& arena_;
    detail::GraphState* state_ = nullptr;
};

template <class Execute>
GraphPassExecutor* RenderGraph::MakeExecutor(Execute&& execute) noexcept {
    using Stored = RemoveConstT<RemoveReferenceT<Execute>>;
    using Executor = detail::GraphLambdaExecutor<Stored>;
    static_assert(IsNothrowDestructibleV<Stored>, "pass functions must be nothrow destructible");
    auto memory = arena_.TryAllocate(sizeof(Executor), alignof(Executor));
    if (!memory) {
        return nullptr;
    }

    Stored stored(Forward<Execute>(execute));
    return ConstructAt<Executor>(memory.Value(), Move(stored));
}

template <class Execute>
Result<GraphPass> RenderGraph::AddPass(TraceScopeName& name, GraphPassKind kind,
                                       GraphPassFlags flags, const GraphPassAccesses& accesses,
                                       Execute&& execute) noexcept {
    if (accesses.Error() != ErrorCode::None) {
        return Result<GraphPass>::Failure(accesses.Error());
    }

    GraphPassExecutor* executor = MakeExecutor(Forward<Execute>(execute));
    if (executor == nullptr) {
        return Result<GraphPass>::Failure(ErrorCode::OutOfMemory);
    }

    return AddPassWithAccesses(name, kind, flags, accesses.Accesses(), accesses.Target(), executor);
}

} // namespace rocks::render
