#pragma once

#include <Base/Types.h>
#include <Core/Containers/Span.h>
#include <Core/Result.h>
#include <Crypto/AsyncCrypto.h>

// 비동기 세그먼트 처리의 값을 받는 내부 경로다(Docs/Design/Crypto.ko.md 9.7·11절 결정성). 공개
// TrySubmitSegmentEncrypt·TrySubmitSegmentDecrypt는 OS 난수 salt와 빈 probe로 같은 구현을 부른다.
// 시험은 고정 salt로 동기 API와 byte 일치를 확인하고, probe로 세그먼트 경계의 취소를 결정적으로
// 일으킨다. 바꿔 둘 수 있는 전역 hook이 아니라 호출마다 넘기는 값이다.
namespace rocks::crypto::detail {

// 세그먼트 경계마다 부르는 확인 함수다. true면 그 작업이 취소를 관측한 것과 같다. 작업
// 스레드에서 불리므로 여러 스레드가 동시에 부를 수 있다. 확인 뒤에 작업 자신의 취소 요청도 본다.
struct SegmentCryptoProbe {
    bool (*check)(void* context) noexcept = nullptr;
    void* context = nullptr;
};

// salt(32 byte, 아니면 InvalidArgument)를 받는 암호화 제출이다. 나머지는 TrySubmitSegmentEncrypt와
// 같다.
[[nodiscard]] Result<void>
TrySubmitSegmentEncryptWithSalt(WorkerScheduler& scheduler, SegmentCryptoTask& task,
                                const SegmentStreamKey& key, const SegmentStreamParams& params,
                                Span<const uint8> associatedData, Span<const uint8> plaintext,
                                Span<uint8> ciphertext, const SegmentCryptoOptions& options,
                                Span<const WorkerTaskHandle> dependencies, Span<const uint8> salt,
                                SegmentCryptoProbe probe) noexcept;

// probe를 받는 복호 제출이다. 나머지는 TrySubmitSegmentDecrypt와 같다.
[[nodiscard]] Result<void> TrySubmitSegmentDecryptWithProbe(
    WorkerScheduler& scheduler, SegmentCryptoTask& task, const SegmentStreamKey& key,
    Span<const uint8> associatedData, Span<const uint8> ciphertext, Span<uint8> plaintext,
    const SegmentCryptoOptions& options, Span<const WorkerTaskHandle> dependencies,
    SegmentCryptoProbe probe) noexcept;

} // namespace rocks::crypto::detail
