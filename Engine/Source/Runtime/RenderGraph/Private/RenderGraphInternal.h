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
};

// pass가 subresource 하나에 하는 접근(같은 pass 안의 여러 선언을 합친 것)이다.
struct SubresourceUse {
    uint32 resource = GraphIndexNone;
    uint32 subresource = 0;
    graphics::ResourceAccess access = graphics::ResourceAccess::Unknown;
    GraphAccessFlags flags = GraphAccessFlags::None;
};

// 그래프 자원 하나다(텍스처·버퍼).
struct ResourceRecord {
    GraphResourceKind kind = GraphResourceKind::Texture;
    const char* name = "";
    graphics::TextureDesc textureDesc;
    graphics::BufferDesc bufferDesc;
    uint32 subresources = 1;
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
    // 이 pass가 기대는 생산자 pass다(GraphState::producers의 연결 목록 머리).
    uint32 firstProducer = GraphIndexNone;
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
};

// 오류 문구 버퍼 크기다(영어 진단 문구 하나).
inline constexpr usize GraphErrorMessageBytes = 256;

struct GraphState {
    graphics::Device* device = nullptr;
    RenderHandleTable* handles = nullptr;
    GraphResourcePool* pool = nullptr;
    GpuTrace* trace = nullptr;
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
    // culling DFS 작업 스택이다.
    ArenaArray<uint32> stack;
    // 장치 전이 묶음 임시 공간이다.
    ArenaArray<graphics::ResourceTransition> lowered;

    // 실행 상태다.
    graphics::CommandContext* commands = nullptr;
    bool traceFrameOpen = false;
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

// Deferred 컴파일: export 뿌리 culling, 수명, 전이.
Result<void> CompileGraph(FrameArena& arena, GraphState& state) noexcept;

// pass 하나를 실행한다(자원 할당 → 기록 열기 → prologue 전이 → 기록 함수).
Result<void> ExecutePass(RenderGraph& graph, FrameArena& arena, GraphState& state,
                         uint32 pass) noexcept;

// 그래프 끝: 마지막 전이 → 제출 → 풀 반환·export 연결·lease 반환·축출.
Result<void> FinishGraph(FrameArena& arena, GraphState& state, bool success) noexcept;

} // namespace rocks::render::detail
