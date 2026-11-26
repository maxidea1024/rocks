#pragma once

#include <Core/Memory/FrameArena.h>
#include <Core/Move.h>
#include <Graphics/Device.h>
#include <RenderBase/ComputeDispatch.h>
#include <RenderBase/FullscreenPass.h>
#include <RenderBase/GpuTrace.h>
#include <RenderBase/PipelineCache.h>
#include <RenderBase/RenderHandle.h>
#include <RenderBase/ShaderParameters.h>
#include <RenderGraph/GraphParameterCollector.h>
#include <RenderGraph/GraphPassAccesses.h>
#include <RenderGraph/GraphResourcePool.h>
#include <RenderGraph/GraphSharedData.h>
#include <RenderGraph/GraphTypes.h>
#include <Task/WorkerScheduler.h>
#include <TraceCapture/TraceCpuScope.h>

// 렌더 그래프다(Docs/Design/RenderBase 7·8절 RG1·RG2, UE FRDGBuilder 대응).
namespace rocks::render {

class RenderFrameContext;
class RenderGraph;
class GraphExecutionAccess;

namespace detail {
struct GraphState;
} // namespace detail

// pass 기록 함수가 받는 문맥이다. 기록 함수 호출 동안만 유효하며 저장하지 않는다.
class ROCKS_RENDERGRAPH_API GraphPassContext {
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

    // 이 pass가 TextureView로 선언한 view의 장치 view다(읽기는 SetTexture, 쓰기는
    // SetWritableTexture로 묶는다). 선언하지 않은 view는 null이다.
    graphics::TextureHandle View(GraphTextureView view) const noexcept;

    // 이 pass가 BufferView로 선언한 버퍼 view의 장치 view다(읽기는 SetBufferView, 쓰기는
    // SetWritableBufferView로 묶는다). 선언하지 않은 view는 null이다.
    graphics::BufferViewHandle View(GraphBufferView view) const noexcept;

    // 그래프 상수(RenderGraph::CreateConstants)의 장치 상수 버퍼다. 다른 그래프의 handle이면
    // null이다. 어느 pass에서나 읽을 수 있다(접근 선언 없음, Docs/Design/RenderBase 9.11절 8항).
    graphics::ConstantBufferHandle Constants(GraphConstantBuffer constants) const noexcept;

    // 그래프 상수를 바인딩 지도의 단계 자리마다 묶는다(ApplyConstantBuffer). 실패: 다른 그래프의
    // handle·루트 상수 표 InvalidArgument.
    //
    // 사용 예:
    //   auto view = graph.CreateConstants(viewBlock, "View");      // 선언(소유 스레드)
    //   ... AddPass(..., [=](GraphPassContext& pass) noexcept { pass.SetConstants(viewMap,
    //   view.Value()); ... });
    Result<void> SetConstants(const ShaderBindingMap& map,
                              GraphConstantBuffer constants) const noexcept;

    // 파라미터 구조체 값(parameters — map의 생성 표 구조체)의 자원 멤버를 지도의 자리마다 묶는다
    // (Docs/Design/ShaderSystem 16.2절 SSD65, UE SetShaderParameters 대응): 그래프 SRV 텍스처는 이
    // pass의 장치 텍스처·view를 SetTexture, 짝 샘플러는 SamplerOverride(given이 아니면 지움 —
    // 텍스처 desc 샘플러)를 SetSampler, 그래프 버퍼 SRV는 SetBufferView, UAV는
    // SetWritableTexture·SetWritableBufferView, 장치 멤버는 값 그대로다(검증은 소유 스레드의
    // ParameterBatch 몫 — pass 함수는 병렬 기록에서 불릴 수 있다). 자리 없는 멤버·접근만 선언·렌더
    // 타깃은 묶지 않고, 공유 샘플러는 호출자가 SetSampler로 준다. 실패: 생성 표 지도가 아니거나
    // parameters null InvalidArgument, SetSampler의 실패는 그 오류다(앞 자리는 이미 묶였을 수
    // 있다).
    //
    // 사용 예:
    //   graph.AddPass(BlurName, GraphPassKind::Compute, GraphPassFlags::None, parameters, &blurMap,
    //                 [=](GraphPassContext& pass) noexcept {
    //                     pass.Commands().SetPipeline(blurPipeline);
    //                     (void)pass.SetParameters(blurMap, parameters);
    //                     pass.Commands().Dispatch(groups, 1);
    //                 });
    Result<void> SetParameters(const ShaderResourceMap& map, const void* parameters) const noexcept;

    // view의 기준 mip이다(graphics::TextureViewBaseMip: 범위 view를 지원하는 장치는 0, OpenGL ES는
    // view의 첫 mip). 셰이더는 이 값을 그리기별 상수로 받아 명시 LOD로 읽는다(7.5절). 선언하지 않은
    // view는 0이다.
    uint32 ViewBaseMip(GraphTextureView view) const noexcept;

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
    // 있으면 compute queue 기록(RG4)의 pass GPU 구간과 GPU 프레임을 이 장부에 쓴다
    // (GpuTraceSettings::queue = AsyncCompute인 장부). 없으면 compute 기록에는 marker만 남는다.
    // 그래프 구간(GraphScope)의 GPU 구간은 늘 trace(graphics) 장부다.
    GpuTrace* computeTrace = nullptr;
    // 프레임 번호다(풀 축출 나이·GPU 프레임 번호).
    uint64 frame = 0;
    GraphMode mode = GraphMode::Deferred;
    // 분리 전이(begin·end)를 쓴다(Deferred만, GraphTransitionRecord::beginPass). 끄면 모든 전이를
    // 소비 pass prologue에서 한 번에 낸다(분리 켬·끔 대조 시험·디버그).
    bool splitTransitions = true;
    // 병렬 기록(RG3, RenderBase 8절 "RG3 병렬 기록 설계")이다. scheduler와 scratch가 있고
    // parallelRecording이 켜져 있으면 Deferred 실행이 pass 기록 함수를 worker에서 기록 목록에
    // 기록하고 소유 스레드가 선언 순서로 재생한다(장치 호출 열은 직렬과 같다). scratch는
    // ParallelFor helper 작업을 할당하는 스레드 안전 Allocator다. 즉시 실행 모드는 늘 직렬이다.
    // 병렬 기록 중 pass 기록 함수는 장치 생성·제출을 부르지 않고(소유 스레드 전용), 장치 호출의
    // 결과로 다른 명령을 고르지 않는다(값을 돌려주는 호출은 기록 때 성공, 재생 때 실패하면 그
    // pass의 실행 오류). WriteTimestamp·BeginTransitions·EndTransitions는 쓸 수 없다.
    WorkerScheduler* scheduler = nullptr;
    Allocator* scratch = nullptr;
    bool parallelRecording = true;
    // async compute(RG4, RenderBase 8절 "RG4 async compute 설계")다. 즉시 실행 모드는 늘 끈다.
    // 켜지면 AsyncCompute pass를 compute queue 기록에 내고, fork(그 pass가 기대는 마지막 graphics
    // pass 뒤)·join(그 결과를 처음 쓰는 graphics pass 앞)에서 Device::QueueWait로 두 queue를
    // 맞춘다. 그래프의 마지막 제출은 늘 graphics다(남은 compute 제출을 그래프 끝에서 기다린다).
    GraphAsyncCompute asyncCompute = GraphAsyncCompute::Auto;
    // transient aliasing(RG5, RenderBase 8절 "GX12·RG5 transient aliasing 설계")이다. 장치에
    // transientHeaps가 있고 Deferred면 수명이 겹치지 않는 그래프 자원이 transient heap 메모리를
    // 나눠 쓴다(desc가 달라도). 끄거나 지원하지 않으면 풀(RG2)이다. UE r.RDG.TransientAllocator와
    // 같은 자리다.
    bool transientAliasing = true;
    // async compute pass가 쓰는 자원도 aliasing한다(기본 끔, 1차). 켜면 그 자원은 fork·join으로
    // 넓힌 수명으로 할당한다.
    bool asyncComputeAliasing = false;
};

// compute pass 하나의 dispatch다(RenderGraph::AddComputePass, Docs/Design/RenderBase 4.4절 "GX3·GX4
// compute 연결 설계" (4)). indirectArguments가 null이면 groups로 직접 dispatch하고(빈 그룹 수면
// 아무것도 기록하지 않는다), 아니면 그 버퍼 [indirectOffset]의 DispatchIndirectArguments로 간접
// dispatch한다.
struct GraphComputeDispatch {
    graphics::PipelineHandle pipeline;
    ComputeGroupCount groups;
    GraphBuffer indirectArguments;
    uint64 indirectOffset = 0;
};

// 렌더 lane 프레임 작업에서 그래프 설정을 만든다: 장치·handle 표·그 slot의 프레임 arena·프레임
// 번호를 문맥에서 가져온다(5절 렌더 lane·프레임 slot·FrameArena 연결).
ROCKS_RENDERGRAPH_API RenderGraphDesc
MakeRenderGraphDesc(RenderFrameContext& frame, GraphResourcePool& pool, GpuTrace* trace = nullptr,
                    GraphMode mode = GraphMode::Deferred) noexcept;

// 렌더 그래프다. 한 프레임(또는 offscreen 작업 하나)의 pass를 모아 선언 순서대로 실행한다(결정 R5:
// 재배치 없음). 범위: graphics queue와 async compute queue(RG4), aliasing·render pass 병합 끔,
// 병렬 기록(RG3), 같은 그래프 안 풀 재사용, 그래프 안 업로드, begin·end 분리 전이(RG2).
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
//                     pass.Commands().SetTexture(graphics::ShaderStage::Pixel, sceneSlot,
//                                                pass.Texture(scene.Value()));
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
//   pass의 attachment subresource 전이는 장치 BeginPass가 낸다.
// - 실행: Raster pass마다 장치 기록(BeginPass·BeginFrame … EndFrame)을 하나 연다. Compute·Copy
//   pass는 열린 기록에 이어 쓰고, 열린 기록이 없으면 attachment 없는 BeginPass로 연다. pass마다 CPU
//   구간과(trace가 있으면) GpuScope 구간을 둔다.
// - 검증(항상): 생산 전 읽기, 한 pass 안 같은 subresource의 상충 접근, Raster pass의 UAV, pass
// 종류에
//   맞지 않는 접근, Retire된 자원 import, 생산 전 export, 다른 그래프의 handle. 실패한 호출은
//   InvalidArgument(import는 lease 오류)를 돌려주고 LastError에 영어 문구를 남기며, 그 뒤 Execute는
//   InvalidArgument다(그래프 전체를 버린다).
//
// 스레드: 장치 소유 스레드(렌더 lane) 전용이다. 수명: 렌더 lane 프레임 작업 안에서 만들고 부순다.
// 파괴는 pass 실행기를 부수고 남은 lease를 놓는다(Execute하지 않았으면 풀 자원·export handle도
// 정리한다).
class ROCKS_RENDERGRAPH_API RenderGraph {
public:
    explicit RenderGraph(const RenderGraphDesc& desc) noexcept;
    ~RenderGraph() noexcept;

    RenderGraph(const RenderGraph&) = delete;
    RenderGraph& operator=(const RenderGraph&) = delete;

    // --- 자원 ---

    // 그래프 수명 텍스처를 선언한다. 장치 자원은 첫 사용 pass 직전에 받는다(같은 그래프 안 재사용
    // 자리 또는 풀). desc는 graphics::ValidateTextureDesc 규칙을 따르며(모든 차원·mip 사슬·
    // sampleCount) usage는 다섯 조합 가운데 하나다(graphics::TextureUsage 비트 묶음):
    // - RenderTarget: GPU가 색 attachment로 채운다. 형식은 비압축 PixelFormat이다.
    // - DepthStencil: 깊이 attachment(GraphPassAccesses::DepthTarget)다.
    // - Storage: compute UAV(TextureViewDesc::access ReadWrite)로 채운다.
    // - RenderTarget | Storage: 색 attachment와 compute UAV 둘 다로 채운다.
    // - Sampled: 내용을 UploadTexture로만 받는다(모든 mip, pass는 읽기만). 압축 형식도 된다.
    // GPU가 채우는 셋은 처음 내용이 정해지지 않는다(생산 전 읽기는 검증 오류다).
    // name은 정적 수명 문자열(진단)이다. 실패: 잘못된 desc InvalidArgument, arena 할당 실패는 그
    // 오류다. 장치가 지원하지 않는 형식은 풀 할당 때 장치 오류(Unsupported)로 Execute가 실패한다.
    Result<GraphTexture> CreateTexture(const graphics::TextureDesc& desc,
                                       const char* name) noexcept;

    // 그래프 텍스처(또는 import 텍스처)의 view를 선언한다(7.5절 mip view 그래프 자원). desc는
    // graphics::TextureViewDesc 규칙(mip·slice 범위, 개수 0 = 끝까지, access Read·ReadWrite, 형식·
    // plane·swizzle)이며 범위는 이 텍스처에 맞춰 푼다. pass는 GraphPassAccesses::TextureView로 그
    // 범위의 subresource만 접근하므로, 한 pass가 같은 텍스처의 mip k 읽기 view와 mip k + 1 쓰기
    // view를 함께 가질 수 있다(범위가 겹치면 상충 접근 검증 오류). 장치 view는 그 view를 처음 쓰는
    // pass 직전에 만들고 그래프 끝에 Retire한다. 실패: 다른 그래프 handle·범위 밖·쓰기 view 규칙
    // 위반(Storage 아님·mip 둘 이상)·형식 불일치 InvalidArgument(검증 오류), arena 할당 실패는 그
    // 오류다. import 텍스처의 용도·형식은 장치 view 생성이 확인한다.
    //
    // 사용 예(HZB):
    //   graphics::TextureViewDesc read;
    //   read.firstMip = k;
    //   read.mipCount = 1;
    //   auto mipK = graph.CreateTextureView(hzb, read);
    Result<GraphTextureView> CreateTextureView(GraphTexture texture,
                                               const graphics::TextureViewDesc& desc) noexcept;

    // 그래프 수명 버퍼를 선언한다. 풀에서 새로 만든 버퍼는 0으로 채워져 있고, 다시 쓰는 버퍼의
    // 내용은 정해지지 않는다(업로드하거나 pass가 쓴다).
    Result<GraphBuffer> CreateBuffer(const graphics::BufferDesc& desc, const char* name) noexcept;

    // 그래프 버퍼(또는 import 버퍼)의 view를 선언한다(UE CreateSRV·CreateUAV의 버퍼 판). desc는
    // graphics::BufferViewDesc 규칙(종류·접근·형식·원소 범위)이다. pass는
    // GraphPassAccesses::BufferView로 선언하고 GraphPassContext::View로 장치 view를 받는다. 장치
    // view는 그 view를 처음 쓰는 pass 직전에 만들고 그래프 끝에 Retire한다. 실패: 다른 그래프
    // handle·만든 버퍼의 view 규칙 위반(graphics::ResolveBufferView) InvalidArgument(검증 오류),
    // arena 할당 실패는 그 오류다. import 버퍼의 규칙·장치 한도는 장치 view 생성이 확인한다(실행
    // 오류).
    //
    // 사용 예(compute가 쓰고 다음 pass가 읽는 구조화 버퍼):
    //   graphics::BufferViewDesc write{.kind = graphics::BufferViewKind::Structured,
    //                                  .access = graphics::ViewAccess::ReadWrite};
    //   auto itemsWrite = graph.CreateBufferView(items, write);
    Result<GraphBufferView> CreateBufferView(GraphBuffer buffer,
                                             const graphics::BufferViewDesc& desc) noexcept;

    // --- 그래프 상수(Docs/Design/RenderBase 9.11절 8항, UE CreateUniformBuffer 대응) ---

    // 그래프 수명 상수 버퍼를 선언한다. bytes는 이 호출 안에서 프레임 arena에 복사한다. 장치
    // SingleFrame 상수 버퍼는 실행 시작(Immediate는 이 호출)에 만들고 그래프 끝에 Retire한다.
    // pass는 GraphPassContext::Constants·SetConstants로 묶는다. name은 정적 수명 문자열(진단)이다.
    // 실패: 크기 규칙 위반(graphics::IsValidConstantBufferBytes) InvalidArgument(검증 오류),
    // Execute 뒤 Busy, arena 할당 실패는 그 오류, 장치 생성 실패는 그 오류(그래프 오류 — Execute가
    // 돌려준다).
    Result<GraphConstantBuffer> CreateConstants(Span<const uint8> bytes, const char* name) noexcept;
    // 상수 블록의 지금 값으로 선언한다(블록의 장치 버퍼는 쓰지 않는다).
    Result<GraphConstantBuffer> CreateConstants(const ConstantBlock& block,
                                                const char* name) noexcept;

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
    // 지원하지 않는 형태(AsyncCompute) Unsupported, 장치·풀 실패는 그 오류다.
    template <class Execute>
    Result<GraphPass> AddPass(TraceScopeName& name, GraphPassKind kind, GraphPassFlags flags,
                              const GraphPassAccesses& accesses, Execute&& execute) noexcept;

    // 파라미터 구조체(ROCKS_SHADER_PARAMETERS 생성 구조체)를 프레임 arena에 기본값으로 만든다(UE
    // AllocParameters, ShaderSystem 16.2절 SSD65). 값은 그래프 수명이고 pass에 넘긴 뒤 바꾸지
    // 않는다 (RenderBase 9.6절). 소멸자를 부르지 않으므로 T는 소멸자가 trivial해야 한다. 할당
    // 실패면 null이다.
    template <class T>
    T* AllocateParameters() noexcept;

    // 파라미터 구조체로 pass를 넣는다(ShaderSystem 16.2절 SSD65). 접근은 CollectGraphPassAccesses가
    // parameters의 그래프 멤버(used가 있으면 셰이더가 쓰는 것만)에서 만들고, 그 밖은 위 AddPass와
    // 같다. parameters는 AllocateParameters로 만든 값처럼 그래프보다 오래 살아야 한다(pass 함수가
    // 실행 때 읽는다). 실패: 수집기 실패는 검증 오류(InvalidArgument — 그 뒤 Execute도 실패), 그
    // 밖은 위 AddPass와 같다.
    template <class T, class Execute>
    Result<GraphPass> AddPass(TraceScopeName& name, GraphPassKind kind, GraphPassFlags flags,
                              const T* parameters, const ShaderResourceMap* used,
                              Execute&& execute) noexcept;

    // 위 AddPass의 형식 지운 경로다(파라미터 구조체 수집기가 만든 접근 목록도 이리로 들어온다).
    // executor는 arena에 만든 실행기이며 성공·실패와 무관하게 그래프가 소유한다.
    Result<GraphPass> AddPassWithAccesses(TraceScopeName& name, GraphPassKind kind,
                                          GraphPassFlags flags,
                                          Span<const GraphResourceAccess> accesses,
                                          const GraphRenderTarget& target,
                                          GraphPassExecutor* executor) noexcept;

    // compute pass를 넣는다(UE FComputeShaderUtils::AddPass, Docs/Design/RenderBase 4.4절 "GX3·GX4
    // compute 연결 설계" (4)). 기록 순서는 SetPipeline → bind(pass) → Dispatch·DispatchIndirect다.
    // bind는 void(GraphPassContext&) noexcept 람다이며 view·상수를 묶는다. 간접 dispatch면 인자
    // 버퍼를 이 pass의 IndirectArgs 접근으로 accesses에 더한다(이미 같은 선언이 있으면 더하지
    // 않는다). kind는 Compute·AsyncCompute다(AsyncCompute의 실행 pipe는 그래프가 정한다, RG4).
    // 실패(검증 오류, 그 뒤 Execute는 InvalidArgument): kind가 Compute·AsyncCompute가 아님,
    // pipeline null, 그룹 수가 장치 한도 밖(ValidateGroupCount), 인자 버퍼가 다른 그래프의 것,
    // 만든 인자 버퍼의 ValidateDispatchArguments 위반, import 인자 버퍼의 offset이 4 byte 배수가
    // 아님. 그 밖은 AddPass와 같다.
    //
    // 사용 예(그룹 8 × 8, Storage 텍스처에 쓴다):
    //   GraphPassAccesses accesses(graph);
    //   accesses.TextureView(targetWrite, graphics::ResourceAccess::UavCompute);
    //   GraphComputeDispatch dispatch;
    //   dispatch.pipeline = pipeline;
    //   dispatch.groups = GroupCountFor(w, h, ComputeGroupSize2D, ComputeGroupSize2D);
    //   graph.AddComputePass(FillName, GraphPassKind::Compute, GraphPassFlags::None, accesses,
    //                        dispatch, [=](GraphPassContext& pass) noexcept {
    //                            pass.Commands().SetWritableTexture(0, pass.View(targetWrite));
    //                        });
    template <class Bind>
    Result<GraphPass> AddComputePass(TraceScopeName& name, GraphPassKind kind, GraphPassFlags flags,
                                     GraphPassAccesses& accesses,
                                     const GraphComputeDispatch& dispatch, Bind&& bind) noexcept;

    // 위의 캐시 판이다: 이 자리(소유 스레드)에서 cache.FindOrCreate(request)로 파이프라인을 얻어
    // dispatch.pipeline을 채운다(기록 함수는 worker에서 돌 수 있어 장치 생성을 하지 않는다, RG3).
    // 파이프라인 실패는 그 오류를 돌려주고 그래프 오류로 남긴다(그 뒤 Execute가 그 오류를
    // 돌려준다).
    template <class Bind>
    Result<GraphPass> AddComputePass(TraceScopeName& name, GraphPassKind kind, GraphPassFlags flags,
                                     GraphPassAccesses& accesses, PipelineCache& cache,
                                     const ComputePipelineRequest& request,
                                     GraphComputeDispatch dispatch, Bind&& bind) noexcept;

    // 전체화면 pass를 넣는다(UE FPixelShaderUtils::AddFullscreenPass, Docs/Design/RenderBase 11.4절
    // 3항). Raster pass이고 기록 순서는 SetPipeline → bind(pass) → Draw(3)(정점 버퍼 없음 — VS는
    // 엔진 셰이더 FullscreenTriangleVertex, RenderBase/FullscreenPass.h)다. 렌더 타깃은 accesses의
    // attachment 선언이다. bind는 void(GraphPassContext&) noexcept 람다이며 텍스처·상수를 묶는다.
    // 실패(검증 오류, 그 뒤 Execute는 InvalidArgument): pipeline null, accesses에 렌더 타깃이 없음.
    // 그 밖은 AddPass와 같다.
    //
    // 사용 예(HDR 장면 색을 출력에 복사한다):
    //   GraphPassAccesses accesses(graph);
    //   accesses.Texture(sceneColor, graphics::ResourceAccess::SrvGraphics)
    //       .RenderTarget(output, GraphLoadAction::DontCare);
    //   graph.AddFullscreenPass(CopyName, GraphPassFlags::None, accesses, cache, copyRequest,
    //                           [=](GraphPassContext& pass) noexcept {
    //                               pass.Commands().SetTexture(graphics::ShaderStage::Pixel,
    //                                                          sourceSlot,
    //                                                          pass.Texture(sceneColor));
    //                           });
    template <class Bind>
    Result<GraphPass> AddFullscreenPass(TraceScopeName& name, GraphPassFlags flags,
                                        const GraphPassAccesses& accesses,
                                        graphics::PipelineHandle pipeline, Bind&& bind) noexcept;

    // 위의 캐시 판이다: 이 자리(소유 스레드)에서 cache.FindOrCreate(request)로 파이프라인을 얻는다.
    // 파이프라인 실패는 그 오류를 돌려주고 그래프 오류로 남긴다.
    template <class Bind>
    Result<GraphPass> AddFullscreenPass(TraceScopeName& name, GraphPassFlags flags,
                                        const GraphPassAccesses& accesses, PipelineCache& cache,
                                        const GraphicsPipelineRequest& request,
                                        Bind&& bind) noexcept;

    // 그래프를 실행한다(Deferred: culling·수명·전이 계산 → 선언 순서 실행, Immediate: 마무리).
    // 마지막 전이·제출(EndFrame), 풀 반환·export handle 연결·lease 반환·풀 축출까지 한다. 한 번만
    // 부른다. 실패: 앞선 검증 오류 InvalidArgument, 다시 부르면 Busy, 장치 실패는 그 오류(그래도
    // 정리는 끝까지 한다).
    Result<void> Execute() noexcept;

    // --- 진단(Execute 뒤, 시험·디버그) ---

    // pass가 미사용 제거되었는지다(Deferred는 Execute 뒤에 확정).
    bool IsCulled(GraphPass pass) const noexcept;
    // pass가 실행된(Deferred는 Execute 뒤 확정) pipe다. compute queue를 쓰지 않으면 늘 Graphics다.
    GraphPassPipe PassPipe(GraphPass pass) const noexcept;
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

    // --- 그래프 구간(GraphScope) ---

    // 이름 구간을 연다. 뒤에 넣는 pass는 PopScope까지 이 구간(중첩이면 가장 안쪽)에 속한다. 보통
    // GraphScope 스택 객체로 쓴다. index가 있으면 실행 때 구간을 연 직후 trace 표지로 남긴다(view
    // 번호 등). 실패(arena 할당)는 그래프 오류로 남긴다.
    void PushScope(TraceScopeName& name) noexcept;
    void PushScope(TraceScopeName& name, uint32 index) noexcept;
    // 가장 안쪽 구간을 닫는다. 열린 구간이 없으면 검증 오류다.
    void PopScope() noexcept;

    // 그래프 공유 값이다(7.5절, UE FRDGBlackboard 대응). 값은 그래프가 파괴될 때 소멸한다.
    GraphSharedData& SharedData() noexcept {
        return sharedData_;
    }

private:
    friend class GraphPassContext;
    friend class GraphExecutionAccess;
    friend class GraphSharedData;
    friend class GraphViewTableStorage;

    // 실행기를 arena에 만든다. 실패하면 null이다.
    template <class Execute>
    GraphPassExecutor* MakeExecutor(Execute&& execute) noexcept;

    // AddComputePass의 검사다(실패는 검증 오류로 남긴다). 간접이면 accesses에 IndirectArgs 접근을
    // 더한다.
    Result<void> CheckComputeDispatch(TraceScopeName& name, GraphPassKind kind,
                                      const GraphComputeDispatch& dispatch,
                                      GraphPassAccesses& accesses) noexcept;
    // 캐시 판의 파이프라인 실패를 그래프 오류로 남기고 code를 돌려준다.
    ErrorCode RecordComputePipelineFailure(TraceScopeName& name, ErrorCode code) noexcept;
    // 구조체 판 AddPass의 수집이다(수집기 실패는 검증 오류로 남긴다).
    Result<void> CollectParameterAccesses(TraceScopeName& name, GraphPassKind kind,
                                          const shader::ShaderParameterLayout& layout,
                                          const void* parameters, const ShaderResourceMap* used,
                                          GraphPassAccesses& out) noexcept;
    // AddFullscreenPass의 검사다(실패는 검증 오류로 남긴다).
    Result<void> CheckFullscreenPass(TraceScopeName& name, const GraphPassAccesses& accesses,
                                     graphics::PipelineHandle pipeline) noexcept;
    // 전체화면 캐시 판의 파이프라인 실패를 그래프 오류로 남기고 code를 돌려준다.
    ErrorCode RecordFullscreenPipelineFailure(TraceScopeName& name, ErrorCode code) noexcept;

    FrameArena& arena_;
    detail::GraphState* state_ = nullptr;
    GraphSharedData sharedData_{*this};
};

// 그래프 구간이다(UE RDG_EVENT_SCOPE·RDG_EVENT_SCOPE_STAT 대응, RenderBase 7.5절·8절 RG1 남은 것
// 설계 2). 생성 때 PushScope, 소멸 때 PopScope다. 실행 때 구간 안의 pass 묶음이 CPU trace 구간,
// GPU 구간(RenderGraphDesc::trace), debug marker로 함께 남는다. 제거된 pass만 있는 구간은 남지
// 않는다. 장치 기록이 Raster pass마다 열리므로 marker는 기록마다 다시 push한다(캡처 도구에서
// 구간이 기록마다 나뉘어 보인다). 이름은 정적 TraceScopeName이다.
//
// 사용 예:
//   static TraceScopeName ViewName("View");
//   static TraceScopeName ShadowsName("Shadows");
//   GraphScope view(graph, ViewName, viewIndex);   // 번호는 trace 표지로 남는다
//   {
//       GraphScope shadows(graph, ShadowsName);
//       graph.AddPass(...);
//   }
//
// 스레드·수명: 그래프와 같은 스레드에서 스택 객체로 쓰고 그래프보다 먼저 소멸한다.
class GraphScope {
public:
    GraphScope(RenderGraph& graph, TraceScopeName& name) noexcept : graph_(graph) {
        graph_.PushScope(name);
    }

    GraphScope(RenderGraph& graph, TraceScopeName& name, uint32 index) noexcept : graph_(graph) {
        graph_.PushScope(name, index);
    }

    ~GraphScope() noexcept {
        graph_.PopScope();
    }

    GraphScope(const GraphScope&) = delete;
    GraphScope& operator=(const GraphScope&) = delete;

private:
    RenderGraph& graph_;
};

template <class T>
T* RenderGraph::AllocateParameters() noexcept {
    static_assert(IsTriviallyDestructibleV<T>,
                  "graph parameter structs live in the frame arena and are never destroyed");
    auto memory = arena_.TryAllocate(sizeof(T), alignof(T));
    if (!memory) {
        return nullptr;
    }

    return ConstructAt<T>(memory.Value());
}

template <class T, class Execute>
Result<GraphPass> RenderGraph::AddPass(TraceScopeName& name, GraphPassKind kind,
                                       GraphPassFlags flags, const T* parameters,
                                       const ShaderResourceMap* used, Execute&& execute) noexcept {
    GraphPassAccesses accesses(arena_);
    auto collected = CollectParameterAccesses(name, kind, shader::ShaderParameterLayoutOf<T>(),
                                              parameters, used, accesses);
    if (!collected) {
        return Result<GraphPass>::Failure(collected.Error());
    }

    return AddPass(name, kind, flags, accesses, Forward<Execute>(execute));
}

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

template <class Bind>
Result<GraphPass> RenderGraph::AddComputePass(TraceScopeName& name, GraphPassKind kind,
                                              GraphPassFlags flags, GraphPassAccesses& accesses,
                                              const GraphComputeDispatch& dispatch,
                                              Bind&& bind) noexcept {
    auto checked = CheckComputeDispatch(name, kind, dispatch, accesses);
    if (!checked) {
        return Result<GraphPass>::Failure(checked.Error());
    }

    return AddPass(name, kind, flags, accesses,
                   [dispatch, bound = Forward<Bind>(bind)](GraphPassContext& pass) noexcept {
                       graphics::CommandContext& commands = pass.Commands();
                       auto apply = [&bound, &pass](graphics::CommandContext&) noexcept {
                           bound(pass);
                       };
                       if (dispatch.indirectArguments.IsNull()) {
                           DispatchCompute(commands, dispatch.pipeline, dispatch.groups, apply);
                       } else {
                           DispatchComputeIndirect(commands, dispatch.pipeline,
                                                   pass.Buffer(dispatch.indirectArguments),
                                                   dispatch.indirectOffset, apply);
                       }
                   });
}

template <class Bind>
Result<GraphPass> RenderGraph::AddComputePass(TraceScopeName& name, GraphPassKind kind,
                                              GraphPassFlags flags, GraphPassAccesses& accesses,
                                              PipelineCache& cache,
                                              const ComputePipelineRequest& request,
                                              GraphComputeDispatch dispatch, Bind&& bind) noexcept {
    auto pipeline = cache.FindOrCreate(request);
    if (!pipeline) {
        return Result<GraphPass>::Failure(RecordComputePipelineFailure(name, pipeline.Error()));
    }

    dispatch.pipeline = pipeline.Value();
    return AddComputePass(name, kind, flags, accesses, dispatch, Forward<Bind>(bind));
}

template <class Bind>
Result<GraphPass> RenderGraph::AddFullscreenPass(TraceScopeName& name, GraphPassFlags flags,
                                                 const GraphPassAccesses& accesses,
                                                 graphics::PipelineHandle pipeline,
                                                 Bind&& bind) noexcept {
    auto checked = CheckFullscreenPass(name, accesses, pipeline);
    if (!checked) {
        return Result<GraphPass>::Failure(checked.Error());
    }

    return AddPass(name, GraphPassKind::Raster, flags, accesses,
                   [pipeline, bound = Forward<Bind>(bind)](GraphPassContext& pass) noexcept {
                       graphics::CommandContext& commands = pass.Commands();
                       commands.SetPipeline(pipeline);
                       bound(pass);
                       DrawFullscreenTriangle(commands);
                   });
}

template <class Bind>
Result<GraphPass>
RenderGraph::AddFullscreenPass(TraceScopeName& name, GraphPassFlags flags,
                               const GraphPassAccesses& accesses, PipelineCache& cache,
                               const GraphicsPipelineRequest& request, Bind&& bind) noexcept {
    auto pipeline = cache.FindOrCreate(request);
    if (!pipeline) {
        return Result<GraphPass>::Failure(RecordFullscreenPipelineFailure(name, pipeline.Error()));
    }

    return AddFullscreenPass(name, flags, accesses, pipeline.Value(), Forward<Bind>(bind));
}

} // namespace rocks::render
