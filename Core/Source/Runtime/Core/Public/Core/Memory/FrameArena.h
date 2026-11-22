#pragma once

#include <Base/Checked.h>
#include <Core/Containers/Span.h>
#include <Core/Memory/Allocator.h>
#include <Core/Move.h>
#include <Core/Traits.h>
#include <Platform/Sync.h>

namespace rocks {

class FrameArena;
class FrameArenaWriter;

struct FrameArenaConfig {
    // 표준 블록 하나의 크기(byte, 블록 머리 포함)다. MinBlockBytes보다 작으면 MinBlockBytes로 본다.
    usize blockBytes = 64 * 1024;
    // Reset 뒤 다음 수명에 다시 쓰려고 남겨 두는 빈 표준 블록의 최대 수다. 넘는 블록과 대형 블록은
    // Reset 때 backing에 돌려준다. 0이면 하나도 남기지 않는다.
    usize maxRetainedBlocks = 64;

    static constexpr usize MinBlockBytes = 4096;
};

struct FrameArenaStats {
    // 이번 수명에 쓰고 있는 표준 블록·대형 블록 수와, 다음 수명용으로 남긴 빈 표준 블록 수다.
    usize usedBlocks = 0;
    usize largeBlocks = 0;
    usize retainedBlocks = 0;
    // 이번 수명에 할당한 byte 합과 등록된 소멸자 수다. byte 합은 TryAllocate에 넘긴 크기의 합이라
    // TryNew·TryNewArray가 객체 앞에 붙이는 소멸자 기록도 들어가고, 정렬 여유·블록 머리는 빠진다.
    uint64 requestedBytes = 0;
    uint64 destructors = 0;
    // 지금 열려 있는 FrameArenaWriter 수와 지금까지 성공한 Reset 수다.
    uint64 openWriters = 0;
    uint64 resets = 0;
};

namespace detail {

// 소멸자 등록 기록이다. 객체(또는 배열) 바로 앞에 같은 할당으로 둔다.
struct FrameArenaDestructor {
    void (*destroy)(void* objects, usize count) noexcept = nullptr;
    void* objects = nullptr;
    usize count = 0;
    FrameArenaDestructor* next = nullptr;
};

template <typename T>
void DestroyFrameArenaObjects(void* objects, usize count) noexcept {
    // 배열은 생성 역순으로 부순다.
    if constexpr (!IsTriviallyDestructibleV<T>) {
        T* typed = static_cast<T*>(objects);
        for (usize index = count; index > 0; --index) {
            DestroyAt(typed + index - 1);
        }
    }
}

// FrameArena와 FrameArenaWriter가 함께 쓰는 객체 생성 함수다. Derived는 TryAllocate(bytes,
// alignment)와 Owner()(소멸자를 등록할 FrameArena)를 가진다.
template <typename Derived>
class FrameArenaFactory {
public:
    // T를 args로 생성하고, 자명하지 않은 소멸자면 Reset 때 부르도록 등록한다. 생성자·소멸자는
    // noexcept여야 한다. 실패: 할당 실패는 그 오류(args는 쓰지 않는다), Reset 중이면 Busy.
    template <typename T, typename... Args>
    Result<T*> TryNew(Args&&... args) noexcept {
        static_assert(IsNothrowConstructibleV<T, Args&&...>,
                      "FrameArena objects must be nothrow constructible");
        static_assert(IsNothrowDestructibleV<T>, "FrameArena objects must be nothrow destructible");
        auto placed = Place<T>(1);
        if (!placed) {
            return Result<T*>::Failure(placed.Error());
        }

        T* object = InitializeAt<T>(placed.Value().objects, Forward<Args>(args)...);
        Register(placed.Value());
        return Result<T*>::Success(object);
    }

    // T count개를 값 초기화해 연속으로 만든다. 자명하지 않은 소멸자면 배열 전체를 한 번에
    // 등록한다(Reset 때 원소 역순으로 부순다). count가 0이면 빈 Span이다. 실패: 크기 계산 넘침
    // Overflow, 할당 실패는 그 오류, Reset 중이면 Busy.
    template <typename T>
    Result<Span<T>> TryNewArray(usize count) noexcept {
        static_assert(IsNothrowDefaultConstructibleV<T>,
                      "FrameArena arrays must be nothrow default constructible");
        static_assert(IsNothrowDestructibleV<T>, "FrameArena objects must be nothrow destructible");
        if (count == 0) {
            return Result<Span<T>>::Success(Span<T>());
        }

        auto placed = Place<T>(count);
        if (!placed) {
            return Result<Span<T>>::Failure(placed.Error());
        }

        T* objects = static_cast<T*>(placed.Value().objects);
        for (usize index = 0; index < count; ++index) {
            InitializeAt<T>(objects + index);
        }

        Register(placed.Value());
        return Result<Span<T>>::Success(Span<T>(objects, count));
    }

private:
    struct Placement {
        void* objects = nullptr;
        FrameArenaDestructor* record = nullptr;
    };

    // 소멸자 기록이 필요하면 [기록][여유][객체] 한 덩어리로 잡는다(기록이 객체보다 먼저 사라지지
    // 않는다).
    template <typename T>
    Result<Placement> Place(usize count) noexcept {
        usize objectBytes = 0;
        if (!TryMultiply(sizeof(T), count, objectBytes)) {
            return Result<Placement>::Failure(ErrorCode::Overflow);
        }

        Derived& self = static_cast<Derived&>(*this);
        if constexpr (IsTriviallyDestructibleV<T>) {
            auto memory = self.TryAllocate(objectBytes, alignof(T));
            if (!memory) {
                return Result<Placement>::Failure(memory.Error());
            }

            return Result<Placement>::Success(Placement{memory.Value(), nullptr});
        } else {
            constexpr usize recordAlignment = alignof(FrameArenaDestructor);
            constexpr usize alignment = alignof(T) > recordAlignment ? alignof(T) : recordAlignment;
            constexpr usize objectOffset =
                (sizeof(FrameArenaDestructor) + alignof(T) - 1) / alignof(T) * alignof(T);
            usize total = 0;
            if (!TryAdd(objectOffset, objectBytes, total)) {
                return Result<Placement>::Failure(ErrorCode::Overflow);
            }

            auto memory = self.TryAllocate(total, alignment);
            if (!memory) {
                return Result<Placement>::Failure(memory.Error());
            }

            auto* base = static_cast<unsigned char*>(memory.Value());
            auto* record = ConstructAt<FrameArenaDestructor>(base);
            record->destroy = &DestroyFrameArenaObjects<T>;
            record->objects = base + objectOffset;
            record->count = count;
            return Result<Placement>::Success(Placement{base + objectOffset, record});
        }
    }

    void Register(const Placement& placed) noexcept {
        if (placed.record != nullptr) {
            static_cast<Derived&>(*this).Owner().PushDestructor(placed.record);
        }
    }
};

} // namespace detail

// 그래프·프레임 수명 객체를 담는 다중 스레드 bump arena다(UE FMemStack·FRDGAllocator 대응). 한
// 수명(프레임·렌더 그래프 하나) 동안 객체를 만들고, 수명이 끝나면 Reset이 등록된 소멸자를 등록
// 역순으로 부른 뒤 메모리를 한 번에 비운다. 개별 해제는 없다.
//
// 사용 예:
//   FrameArena arena(system);
//   auto pass = arena.TryNew<RenderPass>(name);          // 소멸자가 있으면 Reset 때 부른다
//   auto draws = arena.TryNewArray<DrawCommand>(count);  // 값 초기화한 연속 배열
//   // worker마다 writer 하나로 잠금 없이 할당한다
//   ParallelFor(scheduler, scratch, count, [&](usize begin, usize end) noexcept {
//       FrameArenaWriter writer(arena);
//       ... writer.TryNew<Node>(...) ...
//   });
//   arena.Reset();   // 수명 끝: 소멸자(등록 역순) → 블록 비움(일부는 다음 수명용으로 남김)
//
// 기존 할당기와의 차이(그래서 Allocator 인터페이스를 구현하지 않는다):
// - ArenaAllocator·StackAllocator는 개별 할당의 반환을 기록하고, 반환되지 않은 할당이 남아 있으면
//   Reset을 Busy로 거부한다. 객체 수명을 모르므로 살아 있는 객체의 메모리를 비우지 않기 위해서다.
// - FrameArena는 객체 수명을 스스로 가진다. 만든 객체의 수명은 정의상 Reset에서 끝나고(소멸자도
//   arena가 부른다) 개별 반환이 없으므로, Reset은 살아 있는 할당을 이유로 거부하지 않는다. 거부하는
//   것은 아직 할당 중인 writer가 열려 있을 때(Busy)뿐이다. Reset 뒤 이전 수명의 포인터를 쓰는 것은
//   호출자의 수명 위반이다(Asan 구성은 비운 블록을 poison해 드러낸다).
// - 여러 스레드가 동시에 할당한다. 스레드마다 FrameArenaWriter를 두면 자기 블록 안에서는 잠금 없이
//   할당하고 새 블록을 얻을 때만 arena 잠금을 쓴다. arena의 TryAllocate·TryNew는 공유 블록 하나를
//   잠금으로 나눠 쓴다(적은 양·드문 할당용).
//
// 계약:
// - 정렬: alignment는 0이 아닌 2의 거듭제곱이다(아니면 InvalidArgument). 0 byte는 성공한 nullptr다.
//   주소는 요청 정렬을 지킨다.
// - 대형 할당: 정렬 여유를 더한 크기가 표준 블록 payload의 절반을 넘으면 그 할당만의 대형 블록을
//   backing에서 따로 받는다. 대형 블록은 Reset 때 backing에 바로 돌려준다.
// - 메모리 반환: Reset은 표준 블록을 최대 maxRetainedBlocks개까지 비워 다음 수명에 다시
//   쓰고 나머지는 backing에 돌려준다. 그래서 수명마다 쓰는 양이 비슷하면 backing 호출이 없다.
//   Trim은 남긴 빈 블록을 모두 돌려준다. 파괴는 Reset 뒤 Trim과 같다.
// - 소멸자: TryNew·TryNewArray로 만든 자명하지 않은 소멸자 객체만 등록한다. Reset은 등록 역순으로
//   부른다(여러 스레드가 등록하면 등록이 게시된 순서의 역순이다). 소멸자 안에서 같은 arena에
//   할당하면 Busy다. TryAllocate로 받은 메모리에 직접 만든 객체는 호출자가 Reset 전에 부순다.
// - 스레드: TryAllocate·TryNew·TryNewArray·Stats·FrameArenaWriter 생성·파괴는 여러
//   스레드에서 동시에 부를 수 있다. Reset·Trim은 다른 어떤 호출과도 겹치지 않아야 한다(호출자가
//   수명 경계를 동기화한다, 예: 그래프 실행 완료 대기 뒤). 열린 writer가 있으면 Busy로 거부한다.
// - 수명: backing은 arena보다 오래 살아야 하고 여러 스레드의 Allocate·Deallocate를 지원해야
//   한다. backing은 arena 잠금 안에서 부르므로 backing 콜백이 같은 arena를 부르면 교착이다
//   (금지). writer가 열린 채 파괴하면 Fatal이다.
class ROCKS_CORE_API FrameArena final : public detail::FrameArenaFactory<FrameArena> {
public:
    explicit FrameArena(Allocator& backing, const FrameArenaConfig& config = {}) noexcept;
    ~FrameArena() noexcept;

    FrameArena(const FrameArena&) = delete;
    FrameArena& operator=(const FrameArena&) = delete;
    FrameArena(FrameArena&&) = delete;
    FrameArena& operator=(FrameArena&&) = delete;

    // 공유 블록에서 bytes를 할당한다(잠금). 실패: 잘못된 정렬 InvalidArgument, 크기 넘침 Overflow,
    // backing 실패는 그 오류, Reset 중이면 Busy. 실패해도 이미 받은 할당은 그대로다.
    Result<void*> TryAllocate(usize bytes, usize alignment) noexcept;

    // 수명을 끝낸다. 등록된 소멸자를 등록 역순으로 부르고, 대형 블록과 남길 수를 넘는 표준 블록을
    // backing에 돌려주고, 나머지 표준 블록을 비워 둔다. 이전 수명의 모든 주소는 무효다. 실패: 열린
    // writer가 있거나 Reset 안(소멸자)에서 다시 부르면 Busy(아무것도 바꾸지 않는다).
    Result<void> Reset() noexcept;

    // 남겨 둔 빈 표준 블록을 모두 backing에 돌려준다. 지금 수명의 블록은 건드리지 않는다. 실패:
    // Reset 중이면 Busy.
    Result<void> Trim() noexcept;

    FrameArenaStats Stats() const noexcept;

private:
    struct Block;
    friend class FrameArenaWriter;
    template <typename>
    friend class detail::FrameArenaFactory;

    FrameArena& Owner() noexcept {
        return *this;
    }

    // 표준 블록 하나를 얻는다(남긴 블록 재사용, 없으면 backing). 성공하면 used 목록에 넣는다.
    // gate_ 안에서 부른다(backing 호출도 잠금 안이다).
    Result<Block*> AcquireBlock() noexcept;
    // cursor 범위에서 할당한다. 공간이 모자라면 false다.
    static bool TryBump(unsigned char*& next, unsigned char* end, usize bytes, usize alignment,
                        void*& result) noexcept;
    // 대형 블록 하나에 할당한다.
    Result<void*> AllocateLarge(usize bytes, usize alignment) noexcept;
    // 표준 블록 cursor 하나(writer 또는 공유)에서 할당하고, 모자라면 새 블록으로 바꾼다.
    Result<void*> AllocateFrom(unsigned char*& next, unsigned char*& end, usize bytes,
                               usize alignment) noexcept;
    void PushDestructor(detail::FrameArenaDestructor* record) noexcept;
    void ReleaseBlock(Block* block) noexcept;
    void OpenWriter() noexcept;
    void CloseWriter() noexcept;

    Allocator& backing_;
    usize blockBytes_;
    usize maxRetainedBlocks_;
    // 블록 목록·공유 cursor·수명 상태를 보호한다. writer의 블록 안 할당은 이 잠금을 쓰지 않는다.
    mutable platform::Mutex gate_;
    Block* usedBlocks_ = nullptr;
    Block* largeBlocks_ = nullptr;
    Block* freeBlocks_ = nullptr;
    usize usedCount_ = 0;
    usize largeCount_ = 0;
    usize freeCount_ = 0;
    unsigned char* sharedNext_ = nullptr;
    unsigned char* sharedEnd_ = nullptr;
    // 원자적으로 갱신한다(writer 잠금 없는 경로·여러 스레드의 등록).
    detail::FrameArenaDestructor* destructors_ = nullptr;
    uint64 destructorCount_ = 0;
    uint64 requestedBytes_ = 0;
    uint64 openWriters_ = 0;
    uint64 resets_ = 0;
    bool resetting_ = false;
};

// 한 스레드가 FrameArena에서 잠금 없이 할당하는 cursor다(UE FRDGAllocator의 스레드별 문맥 대응).
// 자기 표준 블록이 찰 때만 arena 잠금으로 새 블록을 얻는다. 쓰다 남은 블록 공간은 Reset까지 비어
// 있다.
//
// - 한 번에 한 스레드만 쓴다(writer 자체는 동기화하지 않는다). 스레드마다·작업마다 하나씩
//   스택에 둔다.
// - arena보다 먼저, 그리고 arena Reset 전에 파괴한다(열린 writer가 있으면 Reset은 Busy, arena
//   파괴는 Fatal). 만든 객체의 수명은 writer가 아니라 arena 수명이다(writer를 파괴해도 객체는
//   Reset까지 산다).
// - 할당 함수의 오류 규칙은 FrameArena와 같다.
class ROCKS_CORE_API FrameArenaWriter final : public detail::FrameArenaFactory<FrameArenaWriter> {
public:
    explicit FrameArenaWriter(FrameArena& arena) noexcept;
    ~FrameArenaWriter() noexcept;

    FrameArenaWriter(const FrameArenaWriter&) = delete;
    FrameArenaWriter& operator=(const FrameArenaWriter&) = delete;

    Result<void*> TryAllocate(usize bytes, usize alignment) noexcept;

private:
    friend class detail::FrameArenaFactory<FrameArenaWriter>;

    FrameArena& Owner() noexcept {
        return arena_;
    }

    FrameArena& arena_;
    unsigned char* next_ = nullptr;
    unsigned char* end_ = nullptr;
};

} // namespace rocks
