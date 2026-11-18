#pragma once

#include <Base/NumericLimits.h>
#include <Base/Types.h>
#include <Platform/Atomics.h>
#include <Platform/Clock.h>
#include <Platform/Fatal.h>

namespace rocks {

// 읽기·변경 접근이 겹치는 계약 위반을 잡는 검출기다(UE 5.8 Misc/MTAccessDetector.h의
// FRWAccessDetector 계열 대응). 잠금이 아니다: 아무것도 기다리지 않고, 겹침을 발견하면
// platform::Fatal로 끝낸다. "읽기 단계에서는 여러 스레드가 읽기만, 변경 단계에서는 한 스레드만
// 변경" 같은 단계 분리 계약을 가진 자료구조 (공간 인덱스, 물리 질의 범위, tick 그래프의 read/write
// 집합)가 그 계약을 지키는지 확인한다.
//
// 규칙
// - 읽기 범위는 여러 개(여러 스레드, 같은 스레드의 중첩)가 동시에 열려도 된다.
// - 변경 범위는 혼자여야 한다. 읽기 범위가 열린 동안의 변경, 변경 중의 읽기, 변경끼리의 겹침은
//   Fatal이다. 겹친 두 쪽 중 적어도 하나가 검출한다(아래 구현). 같은 스레드의 재귀는
//   AccessRecursion으로 고른다.
// - 모든 빌드 구성에서 켜져 있다(계약 위반 검사는 구성 매크로로 빼지 않는다, UE는 DO_CHECK
// 빌드에서만).
//
// 구현: atomic 단어 하나에 열린 읽기 수(아래 32비트)와 열린 변경 수(위 32비트)를 둔다. 진입·종료는
// 모두 같은 단어의 읽고-쓰기 연산(fetch_add·fetch_sub)이고, 한 객체에 대한 읽고-쓰기 연산은 하나의
// 수정 순서를 이루므로 겹친 두 범위 중 나중에 진입한 쪽이 진입할 때, 또는 먼저 진입한 쪽이 종료할
// 때 반드시 상대를 본다(두 단어를 쓰고 읽는 Dekker 방식보다 단순하다). 그래서 진입과 종료 모두
// 검사한다.
//
// 비용: 범위 진입과 종료마다 atomic 읽고-쓰기 한 번씩이다(재귀 정책이 변경 재귀를 허용하면 변경
// 범위에 현재 스레드 ID 조회가 더해진다). 질의·연산마다 범위를 열지 않는다. 범위는 "읽기 묶음"(예:
// 한 프레임의 질의 작업 전체) 또는 변경 함수 하나 단위로 연다.
//
// 범위 객체(ReadAccessScope·WriteAccessScope)는 만든 스레드에서 같은 블록 안에 끝낸다. 저장해
// 두거나 coroutine의 yield·작업 경계를 넘겨 들고 있지 않는다(물리 QueryReadScope와 같은 규칙,
// EngineImplementationPlan 16.3절). 넘기면 다른 스레드의 정상 접근이 위반으로 보이거나 위반을
// 놓친다.
//
// 사용 예(소유 객체 안):
//   class Index {
//   public:
//       Result<void> TryAdd(...) noexcept {
//           WriteAccessScope write(access_);   // 변경 함수 전체
//           ...
//       }
//       ~Index() noexcept { access_.CheckIdle("Index destroyed while an access is open"); }
//   private:
//       AccessOverlapChecker access_;
//   };
//   // 읽기 단계:
//   ReadAccessScope read(index.Access());   // 묶음 단위로 한 번
//   for (...) { index.Query(...); }          // 질의마다 atomic을 건드리지 않는다

// 같은 스레드 안의 재귀 허용 범위다. 읽기 안의 읽기는 모든 정책에서 허용된다(검색 visitor 안의
// 재검색).
enum class AccessRecursion : uint8 {
    // 읽기 중첩만 허용한다. 변경 안의 변경, 변경 안의 읽기는 Fatal이다. 기본값이다(UE
    // FRWAccessDetector).
    ReadsOnly,
    // 같은 스레드의 변경 안 변경도 허용한다(UE FRWRecursiveAccessDetector). 변경 안의 읽기는
    // Fatal이다.
    NestedWrites,
    // 같은 스레드에서 변경 안의 변경과 변경 안의 읽기를 모두 허용한다(UE
    // FRWFullyRecursiveAccessDetector). 다른 스레드와의 겹침은 여전히 Fatal이다.
    NestedWritesAndReads
};

// 위반마다 Fatal에 넘길 메시지다(영어, 정적 수명). 소유 객체가 자기 이름이 든 메시지를 정적 상수로
// 만들어 넘기면 진단에서 어느 객체의 계약인지 바로 보인다.
struct AccessOverlapMessages {
    // 읽기 범위가 열린 동안 변경 범위가 열렸다(진입한 변경 쪽 또는 종료하는 읽기 쪽이 검출).
    const char* writeDuringRead = "Write access began while a read access is open";
    // 변경 범위가 열린 동안 읽기 범위가 열렸다.
    const char* readDuringWrite = "Read access began while a write access is open";
    // 변경 범위끼리 겹쳤다.
    const char* writeDuringWrite = "Write access began while another write access is open";
    // 열린 범위가 있는데 검출기(소유 객체)가 파괴되었다.
    const char* destroyedWhileOpen = "Access-checked object destroyed while an access is open";
    // 열지 않은 범위를 닫았다(검출기 사용 오류).
    const char* unbalancedRelease = "Access released without a matching acquire";
};

inline constexpr AccessOverlapMessages DefaultAccessOverlapMessages{};

// 접근 겹침 검출기다. 소유 객체가 멤버로 두고 변경 함수는 WriteAccessScope, 읽기 단계는
// ReadAccessScope로 감싼다. 복사·이동할 수 없다: 소유 객체를 옮길 때는 원본의 CheckIdle로 열린
// 범위가 없음을 확인하고 대상은 새 검출기를 쓴다(열린 범위가 옛 주소를 들고 있기 때문이다).
class AccessOverlapChecker {
public:
    // messages는 정적 수명이어야 한다.
    explicit AccessOverlapChecker(
        AccessRecursion recursion = AccessRecursion::ReadsOnly,
        const AccessOverlapMessages& messages = DefaultAccessOverlapMessages) noexcept
        : messages_(&messages), recursion_(recursion) {
    }

    AccessOverlapChecker(const AccessOverlapChecker&) = delete;
    AccessOverlapChecker& operator=(const AccessOverlapChecker&) = delete;
    AccessOverlapChecker(AccessOverlapChecker&&) = delete;
    AccessOverlapChecker& operator=(AccessOverlapChecker&&) = delete;

    // 열린 범위가 남아 있으면 Fatal이다(소유 객체가 먼저 CheckIdle로 자기 메시지를 내는 것이 좋다).
    ~AccessOverlapChecker() noexcept {
        CheckIdle(messages_->destroyedWhileOpen);
    }

    // 열린 읽기·변경 범위가 없으면 true다. 다른 스레드가 동시에 범위를 열고 닫는 중이면 그 순간의
    // 값일 뿐이다(소유 객체의 이동·파괴처럼 다른 접근이 없어야 하는 지점에서 확인용으로 쓴다).
    [[nodiscard]] bool IsIdle() const noexcept {
        return platform::AtomicLoad(&state_) == 0;
    }

    // 열린 범위가 있으면 message로 Fatal이다. 소유 객체의 이동 생성·대입·파괴 첫머리에서 부른다.
    void CheckIdle(const char* message) const noexcept {
        if (!IsIdle()) {
            platform::Fatal(message);
        }
    }

    // 읽기 범위를 연다. 열린 변경 범위가 있으면 Fatal이다(같은 스레드의 변경 안 읽기는
    // NestedWritesAndReads에서만 허용).
    void AcquireRead() const noexcept {
        if (recursion_ == AccessRecursion::NestedWritesAndReads && OwnsWrite()) {
            ++readsInsideWrite_;
            return;
        }
        const uint64 previous = platform::AtomicFetchAdd(&state_, ReaderUnit);
        if ((previous & WriterMask) != 0) {
            platform::Fatal(messages_->readDuringWrite);
        }
    }

    // 읽기 범위를 닫는다. 그 사이 변경 범위가 열렸으면 Fatal이다.
    void ReleaseRead() const noexcept {
        if (recursion_ == AccessRecursion::NestedWritesAndReads && readsInsideWrite_ != 0 &&
            OwnsWrite()) {
            --readsInsideWrite_;
            return;
        }
        const uint64 previous = platform::AtomicFetchSub(&state_, ReaderUnit);
        if ((previous & ReaderMask) == 0) {
            platform::Fatal(messages_->unbalancedRelease);
        }
        if ((previous & WriterMask) != 0) {
            platform::Fatal(messages_->writeDuringRead);
        }
    }

    // 변경 범위를 연다. 다른 읽기·변경 범위가 열려 있으면 Fatal이다(같은 스레드의 변경 안 변경은
    // NestedWrites 이상에서 허용).
    void AcquireWrite() noexcept {
        if (recursion_ != AccessRecursion::ReadsOnly && OwnsWrite()) {
            ++writeDepth_;
            return;
        }
        const uint64 previous = platform::AtomicFetchAdd(&state_, WriterUnit);
        if ((previous & WriterMask) != 0) {
            platform::Fatal(messages_->writeDuringWrite);
        }
        if (previous != 0) {
            platform::Fatal(messages_->writeDuringRead);
        }
        if (recursion_ != AccessRecursion::ReadsOnly) {
            platform::AtomicStore<platform::MemoryOrder::Relaxed>(&writerThread_,
                                                                  platform::CurrentThreadId());
            writeDepth_ = 1;
        }
    }

    // 변경 범위를 닫는다. 그 사이 다른 범위가 열렸으면 Fatal이다.
    void ReleaseWrite() noexcept {
        if (recursion_ != AccessRecursion::ReadsOnly) {
            if (writeDepth_ > 1) {
                --writeDepth_;
                return;
            }
            writeDepth_ = 0;
            platform::AtomicStore<platform::MemoryOrder::Relaxed>(&writerThread_, NoThread);
        }
        const uint64 previous = platform::AtomicFetchSub(&state_, WriterUnit);
        if ((previous & WriterMask) == 0) {
            platform::Fatal(messages_->unbalancedRelease);
        }
        if ((previous & WriterMask) != WriterUnit) {
            platform::Fatal(messages_->writeDuringWrite);
        }
        if ((previous & ReaderMask) != 0) {
            platform::Fatal(messages_->readDuringWrite);
        }
    }

    AccessRecursion Recursion() const noexcept {
        return recursion_;
    }

private:
    static constexpr uint64 ReaderUnit = 1;
    static constexpr uint64 ReaderMask = (uint64{1} << 32) - 1;
    static constexpr uint64 WriterUnit = uint64{1} << 32;
    static constexpr uint64 WriterMask = ~ReaderMask;
    // 변경 범위를 가진 스레드가 없다는 표지다.
    static constexpr uint64 NoThread = NumericLimits<uint64>::Max();

    // 현재 스레드가 열린 변경 범위의 주인인지다. 다른 스레드는 이 값을 바꾸지 않으므로(주인만 쓰고
    // 지운다) relaxed 읽기로 자기 ID와 같은지만 보면 된다.
    bool OwnsWrite() const noexcept {
        return platform::AtomicLoad<platform::MemoryOrder::Relaxed>(&writerThread_) ==
               platform::CurrentThreadId();
    }

    mutable uint64 state_ = 0;
    // 재귀 정책이 변경 재귀를 허용할 때만 쓴다. writeDepth_·readsInsideWrite_는 변경 범위 주인
    // 스레드만 읽고 쓴다.
    mutable uint64 writerThread_ = NoThread;
    uint32 writeDepth_ = 0;
    mutable uint32 readsInsideWrite_ = 0;
    const AccessOverlapMessages* messages_;
    AccessRecursion recursion_;
};

// 읽기 범위다. 만든 스레드에서 같은 블록 안에 끝낸다(저장·yield 경계 넘김 금지).
class ReadAccessScope {
public:
    explicit ReadAccessScope(const AccessOverlapChecker& checker) noexcept : checker_(checker) {
        checker_.AcquireRead();
    }

    ~ReadAccessScope() noexcept {
        checker_.ReleaseRead();
    }

    ReadAccessScope(const ReadAccessScope&) = delete;
    ReadAccessScope& operator=(const ReadAccessScope&) = delete;

private:
    const AccessOverlapChecker& checker_;
};

// 변경 범위다. 만든 스레드에서 같은 블록 안에 끝낸다.
class WriteAccessScope {
public:
    explicit WriteAccessScope(AccessOverlapChecker& checker) noexcept : checker_(checker) {
        checker_.AcquireWrite();
    }

    ~WriteAccessScope() noexcept {
        checker_.ReleaseWrite();
    }

    WriteAccessScope(const WriteAccessScope&) = delete;
    WriteAccessScope& operator=(const WriteAccessScope&) = delete;

private:
    AccessOverlapChecker& checker_;
};

} // namespace rocks
