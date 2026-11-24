#pragma once

#include <Base/Compiler.h>
#include <Core/Result.h>
#include <Platform/Atomics.h>
#include <Platform/Clock.h>

namespace rocks::graphics {

// 장치 소유 스레드 기록이다(Docs/Design/RenderBase 5.4절 GX10·GX11). backend 장치가 하나씩
// 갖고 소유 스레드 전용 호출의 검사와 소유권 이관(Release → Acquire)에 쓴다.
//
// 사용 예(backend 구현):
//   Result<BufferHandle> CreateBuffer(...) noexcept override {
//       auto owner = ownership_.Check();
//       if (!owner) {
//           return Result<BufferHandle>::Failure(owner.Error());   // WrongThread(Development)
//       }
//       ...
//   }
//
// 계약:
// - 만든 스레드가 처음 소유자다. Release는 소유 스레드만 부르고, 그 뒤 소유자가 없다. Acquire는
//   소유자가 없을 때 부른 스레드를 소유자로 만든다.
// - Check는 Debug·Development 구성에서 소유 스레드가 아니면 WrongThread를 돌려주고 거부 수를
//   올린다. Release 구성(배포)에서는 검사를 생략하고 늘 성공한다(소유권 기록과 Release·Acquire는
//   그대로 동작한다).
// - 모든 함수가 스레드 안전이다.
class DeviceOwnership {
public:
    // 소유자가 없음을 뜻하는 스레드 값이다.
    static constexpr uint64 NoOwner = NumericLimits<uint64>::Max();

    DeviceOwnership() noexcept : owner_(platform::CurrentThreadId()) {
    }

    bool IsOwner() const noexcept {
        return platform::AtomicLoad<platform::MemoryOrder::Acquire>(&owner_) ==
               platform::CurrentThreadId();
    }

    bool HasOwner() const noexcept {
        return platform::AtomicLoad<platform::MemoryOrder::Acquire>(&owner_) != NoOwner;
    }

    Result<void> Check() const noexcept {
        if constexpr (IsReleaseBuild) {
            return Result<void>::Success();
        } else {
            if (IsOwner()) {
                return Result<void>::Success();
            }

            platform::AtomicFetchAdd<platform::MemoryOrder::Relaxed>(&rejected_, uint64{1});
            return Result<void>::Failure(ErrorCode::WrongThread);
        }
    }

    // 소유 스레드가 소유권을 놓는다. 실패: 소유 스레드가 아니면 WrongThread.
    Result<void> Release() noexcept {
        uint64 expected = platform::CurrentThreadId();
        if (!platform::AtomicCompareExchangeStrong<platform::MemoryOrder::AcquireRelease,
                                                   platform::MemoryOrder::Acquire>(
                &owner_, expected, NoOwner)) {
            return Result<void>::Failure(ErrorCode::WrongThread);
        }

        return Result<void>::Success();
    }

    // 소유자가 없는 장치를 이 스레드가 잡는다. 이미 이 스레드가 소유자면 성공이다. 실패: 다른
    // 스레드가 소유 중이면 Busy.
    Result<void> Acquire() noexcept {
        const uint64 self = platform::CurrentThreadId();
        uint64 expected = NoOwner;
        if (platform::AtomicCompareExchangeStrong<platform::MemoryOrder::AcquireRelease,
                                                  platform::MemoryOrder::Acquire>(&owner_, expected,
                                                                                  self) ||
            expected == self) {
            return Result<void>::Success();
        }

        return Result<void>::Failure(ErrorCode::Busy);
    }

    // Check가 거부한 호출 수다(진단).
    uint64 Rejected() const noexcept {
        return platform::AtomicLoad<platform::MemoryOrder::Relaxed>(&rejected_);
    }

private:
    uint64 owner_;
    mutable uint64 rejected_ = 0;
};

} // namespace rocks::graphics
