#pragma once

#include <Core/Diagnostics/Log.h>
#include <Core/Text/StringBuilder.h>
#include <Platform/Memory.h>
#include <RenderGraph/RenderGraph.h>

// 렌더 그래프 내부 표다(RenderGraph.cpp·GraphCompile.cpp·GraphExecute.cpp가 함께 쓴다). 모든 표는
// 그래프 수명이며 프레임 arena에 둔다(7.3절 "그래프 수명 할당은 CP3 프레임 arena").
namespace rocks::render::detail {

// 프레임 arena에 두는 늘어나는 배열이다. 자명하게 복사·소멸하는 원소만 담는다(커질 때 새 자리로
// byte 복사하고 옛 자리는 arena Reset까지 버려 둔다. 버리는 양은 최종 크기보다 작다).
template <class T>
class ArenaArray {
    static_assert(IsTriviallyCopyableV<T> && IsTriviallyDestructibleV<T>,
                  "ArenaArray elements must be trivially copyable and destructible");

public:
    ArenaArray() noexcept = default;

    Result<void> TryPushBack(FrameArena& arena, const T& value) noexcept {
        if (size_ == capacity_) {
            auto grown = Grow(arena, size_ + 1);
            if (!grown) {
                return grown;
            }
        }

        platform::MemoryCopy(data_ + size_, &value, sizeof(T));
        ++size_;
        return Result<void>::Success();
    }

    // count개를 value로 채운 배열을 만든다(이전 내용은 버린다).
    Result<void> TryAssign(FrameArena& arena, usize count, const T& value) noexcept {
        size_ = 0;
        if (count > capacity_) {
            auto grown = Grow(arena, count);
            if (!grown) {
                return grown;
            }
        }

        for (usize index = 0; index < count; ++index) {
            platform::MemoryCopy(data_ + index, &value, sizeof(T));
        }

        size_ = count;
        return Result<void>::Success();
    }

    T& operator[](usize index) noexcept {
        return data_[index];
    }

    const T& operator[](usize index) const noexcept {
        return data_[index];
    }

    usize Size() const noexcept {
        return size_;
    }

    bool IsEmpty() const noexcept {
        return size_ == 0;
    }

    void Clear() noexcept {
        size_ = 0;
    }

    void PopBack() noexcept {
        --size_;
    }

    T& Back() noexcept {
        return data_[size_ - 1];
    }

    Span<T> View() noexcept {
        return Span<T>(data_, size_);
    }

    Span<const T> View() const noexcept {
        return Span<const T>(data_, size_);
    }

    T* begin() noexcept {
        return data_;
    }

    T* end() noexcept {
        return data_ + size_;
    }

    const T* begin() const noexcept {
        return data_;
    }

    const T* end() const noexcept {
        return data_ + size_;
    }

private:
    static constexpr usize MinCapacity = 8;

    Result<void> Grow(FrameArena& arena, usize required) noexcept {
        usize capacity = capacity_ < MinCapacity ? MinCapacity : capacity_ * 2;
        if (capacity < required) {
            capacity = required;
        }

        usize bytes = 0;
        if (!TryMultiply(sizeof(T), capacity, bytes)) {
            return Result<void>::Failure(ErrorCode::Overflow);
        }

        auto memory = arena.TryAllocate(bytes, alignof(T));
        if (!memory) {
            return Result<void>::Failure(memory.Error());
        }

        T* replacement = static_cast<T*>(memory.Value());
        if (size_ != 0) {
            platform::MemoryCopy(replacement, data_, size_ * sizeof(T));
        }

        data_ = replacement;
        capacity_ = capacity;
        return Result<void>::Success();
    }

    T* data_ = nullptr;
    usize size_ = 0;
    usize capacity_ = 0;
};

// subresource 하나의 컴파일·실행 상태다.
struct SubresourceTrack {
    // 지금(또는 열린 병합 구간의) 접근 상태다. 처음에는 import 상태(만든 자원은 Unknown)다.
    graphics::ResourceAccess access = graphics::ResourceAccess::Unknown;
    GraphAccessFlags flags = GraphAccessFlags::None;
    // 지금 병합 구간에 들어올 때의 전이 기록 번호다(GraphState::chains). 구간이 없으면 None이다.
    uint32 chain = GraphIndexNone;
    // 이 subresource의 마지막 생산자 pass다(culling 간선).
    uint32 lastProducer = GraphIndexNone;
    // 전이 추적 중 이 subresource를 마지막으로 쓴(접근한) pass다(분리 전이의 begin 자리).
    uint32 lastUser = GraphIndexNone;
    // 그래프 안 업로드가 내용을 채운다(생산자 pass 없이도 읽을 수 있다).
    bool uploaded = false;
};

// pass가 subresource 하나에 하는 접근(같은 pass 안의 여러 선언을 합친 것)이다.
struct SubresourceUse {
    uint32 resource = GraphIndexNone;
    uint32 subresource = 0;
    graphics::ResourceAccess access = graphics::ResourceAccess::Unknown;
    GraphAccessFlags flags = GraphAccessFlags::None;
    // pass attachment다. 그 전이는 장치 BeginPass가 내므로 그래프는 내지 않는다.
    bool attachment = false;
};

// 그래프 자원 하나다(텍스처·버퍼).
struct ResourceRecord;

// 텍스처 subresource(mip, slice)의 번호다(slice × mipLevels + mip, 장치 subresource 순서).
uint32 SubresourceIndex(const ResourceRecord& resource, uint32 mip, uint32 slice) noexcept;

struct ResourceRecord {
    GraphResourceKind kind = GraphResourceKind::Texture;
    const char* name = "";
    graphics::TextureDesc textureDesc;
    graphics::BufferDesc bufferDesc;
    // subresource 수(mipLevels × slices)다. 텍스처 subresource 번호는 slice × mipLevels + mip이다
    // (SubresourceIndex). 버퍼는 1이다.
    uint32 subresources = 1;
    uint32 mipLevels = 1;
    uint32 slices = 1;
    bool imported = false;
    bool exported = false;
    // 생산된 적이 있다(import는 처음부터 생산된 것으로 본다).
    bool produced = false;
    bool allocated = false;
    graphics::ResourceAccess initialAccess = graphics::ResourceAccess::Unknown;
    // 그래프 끝 상태(export·import 설정)다. Unknown이면 바꾸지 않는다.
    graphics::ResourceAccess finalAccess = graphics::ResourceAccess::Unknown;
    // import 원본 또는 export 대상 handle이다.
    RenderHandle handle;
    RenderResourceLease lease;
    // 장치 handle slot이다(import면 lease의 것, 만든 자원이면 할당 뒤).
    SlotHandle device;
    GraphPoolEntry poolEntry;
    // subresource별 상태(tracks)의 시작 위치다(GraphState::tracks).
    uint32 firstTrack = 0;
    uint32 firstPass = GraphIndexNone;
    uint32 lastPass = GraphIndexNone;
    uint32 references = 0;
    bool released = false;
    // 이 자원이 쓰는 장치 객체 자리다(GraphState::physicals). 같은 그래프 안 재사용이면 앞 자원과
    // 같은 자리다. 아직 정하지 않았으면 None이다(import는 늘 None).
    uint32 physical = GraphIndexNone;
    // 앞 자원이 놓은 장치 객체를 이어 쓴다(첫 전이 before가 Discard).
    bool reusedWithinGraph = false;
    // 어느 pass가 접근을 선언했다(그 뒤 업로드는 순서 위반이다).
    bool accessed = false;
    // 그래프 안 업로드를 받는다(그래프 시작에 할당하고 올린다).
    bool uploadTarget = false;
    // 어느 Raster pass의 렌더 타깃(attachment)이다.
    bool renderTarget = false;
    // Sampled 텍스처 업로드의 초기 데이터(TextureDataBytes, 프레임 arena)다.
    uint8* uploadPixels = nullptr;
    // --- transient aliasing(RG5) ---
    // transient heap에 둔다(컴파일이 정한다). heap은 GraphState::transientHeaps 번호다.
    bool transient = false;
    uint32 transientHeap = GraphIndexNone;
    uint64 transientOffset = 0;
    // 장치가 준 크기와, 할당이 차지한 구간(정렬 앞 빈틈 포함, 놓을 때 이 구간을 돌려준다)이다.
    uint64 transientBytes = 0;
    uint64 regionBegin = 0;
    uint64 regionEnd = 0;
};

// 그래프 안 transient heap 하나(컴파일 계획)다. pool은 실행 때 받은 풀 heap 번호다.
struct TransientHeapPlan {
    graphics::TransientHeapKind kind = graphics::TransientHeapKind::Buffers;
    uint64 capacity = 0;
    uint64 highWater = 0;
    uint32 pool = GraphIndexNone;
};

// transient heap의 빈 구간 하나다(offset 순, 앞 자원이 놓은 구간이면 resource가 그 자원이다).
struct TransientRange {
    uint32 heap = GraphIndexNone;
    uint64 offset = 0;
    uint64 size = 0;
    uint32 resource = GraphIndexNone;
};

// 그래프 자원이 쓰는 장치 객체 자리 하나다(같은 그래프 안 재사용 단위, RG2). 수명이 겹치지 않는
// 같은 desc 자원 여럿이 한 자리를 차례로 쓴다. 풀에서 받는 것은 첫 자원의 첫 pass 직전이고 풀에
// 돌려주는 것은 그래프 끝이다.
struct PhysicalResource {
    // 키(desc)를 대표하는 첫 자원 번호다.
    uint32 owner = GraphIndexNone;
    // 계획 중 지금 자원이 쓰는 중이다(놓이면 같은 키의 뒤 자원이 받을 수 있다).
    bool held = false;
    // 놓였지만 아직 열린 장치 기록의 attachment일 수 있다. 다음 Raster pass(새 기록)부터 받는다.
    bool waitsForRecording = false;
    // 풀에서 받았다(entry·device 유효).
    bool acquired = false;
    // 그래프 끝에 export로 넘겼다(풀에 Release하지 않는다).
    bool exported = false;
    GraphPoolEntry entry;
    SlotHandle device;
};

// 그래프 view 하나다(CreateTextureView·CreateBufferView). 장치 view는 처음 쓰는 pass 직전에 만들고
// 그래프 끝에 Retire한다.
struct ViewRecord {
    GraphResourceKind kind = GraphResourceKind::Texture;
    // 원 자원 번호다(텍스처 view면 텍스처, 버퍼 view면 버퍼).
    uint32 resource = GraphIndexNone;
    graphics::TextureViewDesc desc;
    graphics::BufferViewDesc bufferDesc;
    // 텍스처에 맞춰 푼 범위다(접근 선언의 subresource 범위, 버퍼 view는 쓰지 않는다).
    graphics::ResolvedSubresourceRange range;
    SlotHandle device;

    // 쓰기(UAV) view인지다.
    bool Writable() const noexcept {
        return kind == GraphResourceKind::Texture
                   ? desc.access == graphics::ViewAccess::ReadWrite
                   : bufferDesc.access == graphics::ViewAccess::ReadWrite;
    }
};

class GraphCommandList;

// 그래프 구간 하나다(PushScope). parent는 바깥 구간이다(없으면 None).
struct ScopeRecord {
    TraceScopeName* name = nullptr;
    uint32 parent = GraphIndexNone;
    uint32 index = 0;
    bool hasIndex = false;
};

// 실행 중 열린 구간 하나다(바깥부터 쌓인다).
struct OpenScopeEntry {
    uint32 scope = GraphIndexNone;
    uint64 gpuSpan = 0;
    uint64 cpuSerial = 0;
    // GPU 구간을 graphics 기록에서 열었다(compute 기록에서 연 구간은 GPU 구간이 없다, RG4).
    bool gpuOpened = false;
};

// 버퍼 업로드 하나다(데이터는 프레임 arena에 복사해 둔다).
struct BufferUpload {
    uint32 resource = GraphIndexNone;
    uint64 offset = 0;
    const uint8* data = nullptr;
    usize size = 0;
};

// pass 하나다.
struct PassRecord {
    TraceScopeName* name = nullptr;
    GraphPassKind kind = GraphPassKind::Raster;
    GraphPassFlags flags = GraphPassFlags::None;
    GraphRenderTarget target;
    GraphPassExecutor* executor = nullptr;
    // 이 pass의 subresource 접근이다(GraphState::uses의 [firstUse, firstUse + useCount)).
    uint32 firstUse = 0;
    uint32 useCount = 0;
    // 이 pass가 선언한 view 번호다(GraphState::passViews의 [firstView, firstView + viewCount)).
    uint32 firstView = 0;
    uint32 viewCount = 0;
    // 이 pass가 속한 가장 안쪽 그래프 구간이다(없으면 None).
    uint32 scope = GraphIndexNone;
    // 자원 할당·장치 view 생성을 마쳤다(병렬 기록은 재생 전에 모두 마친다).
    bool prepared = false;
    // 병렬 기록의 기록 목록이다(직렬이면 null, 실행 때 기록 함수 대신 재생한다).
    GraphCommandList* recorded = nullptr;
    // 이 pass가 새 장치 기록을 연다(Raster pass, 또는 앞 Raster pass의 attachment에 접근해 그
    // 기록을 닫는 Compute·Copy pass). Deferred 컴파일이 정한다(분리 전이·재사용 자리의 기록 경계).
    bool opensRecording = false;
    // 이 pass가 기대는 생산자 pass다(GraphState::producers의 연결 목록 머리).
    uint32 firstProducer = GraphIndexNone;
    // --- async compute(RG4, Deferred 컴파일이 정한다) ---
    // compute queue에서 실행한다(AsyncCompute pass이고 그래프가 compute queue를 쓴다).
    bool async = false;
    // async pass: 이 pass가 속한 run(이어진 async pass 묶음 = compute 기록 하나)이 기다릴 graphics
    // fork pass다. GraphIndexNone이면 그래프 시작(prologue 지점)이다.
    uint32 runFork = GraphIndexNone;
    // async pass: run의 첫 pass다(compute 기록을 열고 fork 지점을 기다린다).
    bool startsRun = false;
    // async pass: 결과를 처음 쓰는 graphics pass(join)다. GraphIndexNone이면 그래프 끝이다.
    uint32 joinPass = GraphIndexNone;
    // graphics pass: 어느 run의 fork다(실행 뒤 fork 전이를 내고 기록을 닫아 지점을 남긴다).
    bool forks = false;
    // graphics pass: 어느 async pass의 join이다(새 기록을 열기 전에 compute 지점을 기다린다).
    bool joins = false;
    // 실행 중 정한 지점이다: fork pass는 그 기록의 graphics 제출, async pass는 그 compute 기록의
    // 제출이다.
    graphics::GpuFencePoint submitted;
    // 부수 효과·외부 출력 pass다(culling 뿌리).
    bool root = false;
    bool culled = true;
    bool executed = false;
};

// 생산자 간선 하나다(연결 목록).
struct ProducerLink {
    uint32 pass = GraphIndexNone;
    uint32 next = GraphIndexNone;
};

// 병합 구간에 들어오는 전이 후보 하나다(subresource 단위). 구간이 합쳐지면 after가 넓어진다.
// 마지막에 전이가 필요한 것만 GraphTransitionRecord로 모은다.
struct ChainEntry {
    uint32 pass = GraphIndexNone;
    uint32 resource = GraphIndexNone;
    uint32 subresource = 0;
    graphics::ResourceAccess before = graphics::ResourceAccess::Unknown;
    graphics::ResourceAccess after = graphics::ResourceAccess::Unknown;
    GraphAccessFlags beforeFlags = GraphAccessFlags::None;
    GraphAccessFlags afterFlags = GraphAccessFlags::None;
    bool attachment = false;
    // 앞 구간을 마지막으로 쓴 pass다(없으면 None).
    uint32 previousUser = GraphIndexNone;
    // transient 자원의 첫 전이(aliasing 획득)다.
    bool acquire = false;
};

// 분리 전이 묶음 하나다: beginPass epilogue에서 begin한 전이 가운데 endPass에서 end하는 것들이다.
// 장치가 준 짝을 end까지 들고 있는다.
struct SplitGroup {
    uint32 beginPass = GraphIndexNone;
    uint32 endPass = GraphIndexNone;
    graphics::SplitTransition split;
};

// 그래프 상수 하나다(RenderGraph::CreateConstants). 내용은 프레임 arena에 있고 device는 장치
// SingleFrame 상수 버퍼다(만들기 전·Retire 뒤 null).
struct ConstantRecord {
    uint8* data = nullptr;
    uint32 bytes = 0;
    const char* name = "";
    SlotHandle device;
};

// 오류 문구 버퍼 크기다(영어 진단 문구 하나).
inline constexpr usize GraphErrorMessageBytes = 256;

struct GraphState {
    graphics::Device* device = nullptr;
    RenderHandleTable* handles = nullptr;
    GraphResourcePool* pool = nullptr;
    GpuTrace* trace = nullptr;
    // compute queue 장부와 그 GPU 프레임이 열렸는지다(RG4).
    GpuTrace* computeTrace = nullptr;
    bool computeFrameOpen = false;
    uint64 frame = 0;
    GraphMode mode = GraphMode::Deferred;
    uint32 serial = 0;

    ArenaArray<ResourceRecord> resources;
    ArenaArray<PassRecord> passes;
    ArenaArray<SubresourceUse> uses;
    ArenaArray<SubresourceTrack> tracks;
    ArenaArray<ProducerLink> producers;
    ArenaArray<ChainEntry> chains;
    ArenaArray<GraphTransitionRecord> transitions;
    ArenaArray<PhysicalResource> physicals;
    ArenaArray<BufferUpload> bufferUploads;
    ArenaArray<ViewRecord> views;
    ArenaArray<ConstantRecord> constants;
    ArenaArray<ScopeRecord> scopes;
    // 선언 중 지금 열린 가장 안쪽 구간이다.
    uint32 currentScope = GraphIndexNone;
    // 실행 중 열린 구간 사슬과 지금 장치 기록에 push한 구간 marker 수다.
    ArenaArray<OpenScopeEntry> openScopes;
    uint32 scopeMarkers = 0;
    // pass마다 선언한 view 번호다(PassRecord::firstView).
    ArenaArray<uint32> passViews;
    // culling DFS 작업 스택이다.
    ArenaArray<uint32> stack;
    // 장치 전이 묶음 임시 공간이다.
    ArenaArray<graphics::ResourceTransition> lowered;
    ArenaArray<SplitGroup> splitGroups;
    // 분리 전이를 쓴다(Deferred이고 RenderGraphDesc::splitTransitions).
    bool splitEnabled = false;
    // 병렬 기록 설정이다(RenderGraphDesc).
    WorkerScheduler* scheduler = nullptr;
    Allocator* scratch = nullptr;
    bool parallelRecording = false;

    // async compute를 쓴다(Deferred, RenderGraphDesc::asyncCompute와 장치 능력, RG4).
    bool asyncEnabled = false;
    // transient aliasing을 쓴다(Deferred, RenderGraphDesc::transientAliasing와 장치 능력, RG5)와
    // async compute 자원도 aliasing하는지다.
    bool transientEnabled = false;
    bool asyncComputeAliasing = false;
    ArenaArray<TransientHeapPlan> transientHeaps;
    ArenaArray<TransientRange> transientRanges;
    // 컴파일이 compute queue에 둔 pass가 있다.
    bool hasAsyncPasses = false;

    // 실행 상태다.
    graphics::CommandContext* commands = nullptr;
    // 지금 장치 기록의 queue다.
    graphics::QueueKind recordingQueue = graphics::QueueKind::Graphics;
    // 지금 compute 기록을 연 async pass다(제출 지점을 run의 pass들에 적는다).
    uint32 computeRunStart = GraphIndexNone;
    // 그래프 시작의 graphics 지점이다(prologue fork가 기다린다).
    graphics::GpuFencePoint prologuePoint;
    // 마지막 compute 제출과 graphics queue가 이미 기다린 compute 값이다(그래프 끝 join 판단).
    graphics::GpuFencePoint lastComputePoint;
    uint64 joinedComputeValue = 0;
    // compute queue가 이미 기다린 graphics 값이다(같은 fork를 다시 기다리지 않는다).
    uint64 forkWaitedValue = 0;
    bool traceFrameOpen = false;
    // 지금 장치 기록을 연 Raster pass다(attachment 없는 기록·기록 없음이면 None).
    uint32 recordingPass = GraphIndexNone;
    // Deferred 실행에서 다음에 낼 전이 기록 번호다.
    usize nextTransition = 0;

    GraphStats stats;
    ErrorCode failure = ErrorCode::None;
    char message[GraphErrorMessageBytes] = {};
    usize messageLength = 0;
    bool executed = false;
    bool compiled = false;
};

// --- 접근 규칙(GraphAccessRules.cpp) ---

// 연속한 두 pass의 접근을 한 상태로 합칠 수 있는지다(UE FRDGSubresourceState::IsMergeAllowed). 접근
// 조합이 한 상태로 성립하고, 다음이 UAV면 두 접근 모두 생략 표시가 있어야 하며, 표시가 같아야 한다.
bool CanMergeAcrossPasses(graphics::ResourceAccess previous, GraphAccessFlags previousFlags,
                          graphics::ResourceAccess next, GraphAccessFlags nextFlags) noexcept;

// 한 pass 안의 두 접근을 합칠 수 있는지다(상충 검사). 합친 조합이 한 상태로 성립하면 된다.
bool CanMergeWithinPass(graphics::ResourceAccess first, graphics::ResourceAccess second) noexcept;

// previous에서 next로 가려면 장치 전이가 필요한지다(UE IsTransitionRequired). 접근·표시가 다르면
// 필요하고, 같은 UAV면 두 접근 모두 생략 표시가 있을 때만 생략한다.
bool IsTransitionRequired(graphics::ResourceAccess previous, GraphAccessFlags previousFlags,
                          graphics::ResourceAccess next, GraphAccessFlags nextFlags) noexcept;

// 진단 문구용 접근 이름이다("SrvGraphics|CopySource"). out에 NUL로 끝나게 쓴다.
void FormatAccess(graphics::ResourceAccess access, char* out, usize capacity) noexcept;

// --- 공용 helper(RenderGraph.cpp) ---

// pass가 Raster pass rasterPass의 attachment subresource를 복사 원본·대상으로 쓰는지다(그 기록을
// 닫아야 하는지, 장치 CopyTexture는 패스 attachment를 받지 않는다).
bool UsesAttachmentsOf(const GraphState& state, uint32 pass, uint32 rasterPass) noexcept;

// 그래프 로그 카테고리다.
inline constexpr LogCategory LogRenderGraph{"RenderGraph"};

// 오류 문구를 남기고 로그에 쓴다(Warning, 문구는 영어).
void PublishGraphError(GraphState& state, usize length) noexcept;

// 오류를 기록하고 code를 돌려준다. 그래프의 첫 오류 코드는 Execute가 돌려준다(검증 오류면 그래프
// 전체를 버린다). validation이면 검증 오류 계수를 올린다. format은 Core 서식("{}")이다.
template <class... Args>
ErrorCode RecordGraphError(GraphState& state, ErrorCode code, bool validation, StringView format,
                           const Args&... args) noexcept {
    FixedStringBuilder builder(Span<char>(state.message, GraphErrorMessageBytes - 1));
    (void)builder.TryAppendFormat(format, args...);
    if (state.failure == ErrorCode::None) {
        state.failure = code;
    }

    if (validation) {
        ++state.stats.validationErrors;
    }

    PublishGraphError(state, builder.ByteCount());
    return code;
}

// --- 컴파일·실행(GraphCompile.cpp·GraphExecute.cpp) ---

// pass를 살린다(생산자 간선을 거꾸로 따라 DFS).
Result<void> UncullPass(FrameArena& arena, GraphState& state, uint32 pass) noexcept;

// pass 하나의 subresource 접근을 상태 추적에 넣고 전이 후보를 만든다. allowMerge면 앞 구간과
// 합친다.
Result<void> TrackPassTransitions(FrameArena& arena, GraphState& state, uint32 pass,
                                  bool allowMerge) noexcept;

// 그래프 끝 상태 전이 후보(export·import 마지막 상태)를 만든다.
Result<void> TrackFinalTransitions(FrameArena& arena, GraphState& state) noexcept;

// 전이 후보 [first, end)에서 필요한 것을 골라 같은 pass·자원·전후 상태의 연속 mip을 범위로 묶어
// state.transitions에 붙인다.
Result<void> CollectTransitions(FrameArena& arena, GraphState& state, usize first) noexcept;

// 두 자원이 같은 풀 키(desc)인지다.
bool SameResourceKey(const ResourceRecord& left, const ResourceRecord& right) noexcept;

// Deferred 컴파일: export 뿌리 culling, 수명, async compute fork·join과 수명 연장, 같은 그래프 안
// 재사용 계획, 전이.
Result<void> CompileGraph(FrameArena& arena, GraphState& state) noexcept;

// 두 pass가 같은 pipe에서 실행되는지다(pass 하나가 None이면 true).
bool SamePipe(const GraphState& state, uint32 left, uint32 right) noexcept;

// transient heap을 풀에서 받는다(컴파일 계획의 heap마다, Deferred 실행 앞부분).
Result<void> AcquireTransientHeaps(GraphState& state) noexcept;

// Deferred 실행 앞부분(async compute가 있을 때): 그래프 업로드를 제출하고 prologue fork 전이를
// graphics 기록 하나에서 낸 뒤 그래프 시작 지점을 정한다.
Result<void> ExecutePrologue(FrameArena& arena, GraphState& state) noexcept;

// 자원의 장치 객체를 정한다: 자리가 이미 풀에서 받았으면 이어 쓰고, 아니면 풀에서 받는다(자리가
// 없으면 새로 만든다). 업로드 대상이면 이때 올린다.
Result<void> AllocateResource(FrameArena& arena, GraphState& state, uint32 index) noexcept;

// Deferred 실행 앞부분: 남은 업로드 대상을 할당하고 올린다(UE SubmitBufferUploads와 같은 자리).
Result<void> ExecuteUploads(FrameArena& arena, GraphState& state) noexcept;

// 실행 중 열린 그래프 구간을 scope의 사슬에 맞춘다: 사슬 밖 구간을 닫는다(EnterScopes 전).
// commands가 없으면 GPU 구간·marker 없이 닫는다.
void LeaveScopes(GraphState& state, uint32 scope) noexcept;
// scope 사슬 가운데 아직 열지 않은 구간을 바깥부터 연다(열린 장치 기록이 있어야 한다).
void EnterScopes(FrameArena& arena, GraphState& state, uint32 scope) noexcept;

// pass의 자원 할당과 장치 view 생성이다(ExecutePass가 부르거나 병렬 기록이 미리 부른다).
Result<void> PreparePass(FrameArena& arena, GraphState& state, uint32 pass) noexcept;

// 병렬 기록(Deferred): 남은 pass를 선언 순서로 준비하고 ParallelFor로 기록 함수를 기록 목록에
// 기록한다. 기록할 pass가 둘 미만이면 아무것도 하지 않는다(직렬 실행).
Result<void> RecordPassesInParallel(RenderGraph& graph, FrameArena& arena,
                                    GraphState& state) noexcept;

// pass 하나를 실행한다(자원 할당 → 기록 열기 → prologue 전이(분리 전이 end 포함) → 기록 함수 →
// epilogue 분리 전이 begin).
Result<void> ExecutePass(RenderGraph& graph, FrameArena& arena, GraphState& state,
                         uint32 pass) noexcept;

// 그래프 끝: 마지막 전이 → 제출 → 풀 반환·export 연결·lease 반환·축출.
Result<void> FinishGraph(FrameArena& arena, GraphState& state, bool success) noexcept;

// 아직 만들지 않은 그래프 상수의 장치 버퍼를 만든다(Deferred 실행 시작, GraphConstants.cpp).
Result<void> CreateGraphConstants(GraphState& state) noexcept;
// 그래프 상수의 장치 버퍼를 Retire한다(FinishGraph).
void RetireGraphConstants(GraphState& state) noexcept;

} // namespace rocks::render::detail
