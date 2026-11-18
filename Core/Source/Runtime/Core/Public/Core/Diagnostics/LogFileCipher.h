#pragma once

#include <Base/Types.h>
#include <Core/Containers/Span.h>
#include <Core/Result.h>

namespace rocks {

// 파일 sink(FileLogSink)가 flush 묶음의 byte를 파일에 쓰기 전에 거치는 변환 지점이다
// (Docs/Design/LoggingSystem.ko.md 9.1절). Core는 Crypto보다 아래 계층이라 인터페이스만 두고, 구현
// (crypto::LogFileSealer)은 Crypto 모듈에 있다. 설정하지 않으면 평문이다.
//
// 호출 순서(파일마다): TryWriteHeader 한 번 → 묶음마다 TrySeal(batch, false) → 정상 종료·회전 때
// TrySeal({}, true) 한 번. 회전으로 새 파일을 열면 다시 TryWriteHeader부터다(파일마다 새 키 상태).
//
// 계약:
// - 호출은 FileLogSink가 직렬화한다(한 번에 한 스레드). 출력 스레드(Async)나 로그를 남긴 호출
//   스레드(Sync)에서 불린다. 힙을 쓰지 않아야 한다(파일 sink의 무할당 보장).
// - TryWriteHeader가 실패하면 파일 sink는 그 파일을 만들지 않고 스스로 꺼진다(평문으로 내려가지
//   않는다). 키 준비(캡슐화·키 저장소) 실패가 여기서 드러난다.
// - TrySeal의 출력 길이는 batch.Size() + SealOverhead() 이하다. 출력은 그것만으로 파일 안에서
//   경계를 알 수 있어야 한다(길이 머리를 포함한다).
//
// 사용 예(구현):
//   class MyCipher final : public LogFileCipher {
//       usize HeaderBytes() const noexcept override { return 16; }
//       usize SealOverhead() const noexcept override { return 4 + 17; }
//       Result<usize> TryWriteHeader(Span<uint8> out) noexcept override;
//       Result<usize> TrySeal(Span<const uint8> batch, bool last, Span<uint8> out) noexcept
//       override;
//   };
class LogFileCipher {
public:
    virtual ~LogFileCipher() noexcept = default;

    // 파일 머리의 최대 byte 수다.
    virtual usize HeaderBytes() const noexcept = 0;

    // 묶음 하나를 봉인할 때 늘어나는 최대 byte 수다.
    virtual usize SealOverhead() const noexcept = 0;

    // 새 파일의 머리를 out에 쓰고 쓴 byte 수를 돌려준다. 이 파일의 봉인 상태를 새로 만든다.
    // 실패(키 준비 실패, out 부족)면 아무것도 쓰지 않는다.
    virtual Result<usize> TryWriteHeader(Span<uint8> out) noexcept = 0;

    // batch를 봉인해 out에 쓰고 쓴 byte 수를 돌려준다. last면 이 파일의 끝 표시를 함께 쓴다(batch는
    // 비어도 된다). 그 뒤에는 다음 TryWriteHeader 전까지 부르지 않는다.
    virtual Result<usize> TrySeal(Span<const uint8> batch, bool last, Span<uint8> out) noexcept = 0;
};

// FileLogSink가 flush 묶음을 압축하는 변환 지점이다(Docs/Design/LoggingSystem.ko.md 14.1.7, L23).
// Core는 Compression 아래 계층이라 인터페이스만 두고, 구현(zstd 고정 작업 공간 문맥)은 위
// 계층(LogHost)이 둔다. 묶음 하나가 독립 frame 하나다(이어 붙인 frame은 그대로 올바른 스트림이다).
// cipher가 있으면 압축한 frame을 봉인한다.
//
// 계약: Open 뒤 TryCompress는 할당하지 않는다. 출력은 Bound(batch 크기) 이하다.
class LogBatchCompressor {
public:
    virtual ~LogBatchCompressor() noexcept = default;

    // batchBytes를 압축한 결과의 최악 크기다.
    virtual usize Bound(usize batchBytes) const noexcept = 0;

    // batch를 독립 frame 하나로 압축해 out에 쓰고 쓴 byte 수를 돌려준다.
    virtual Result<usize> TryCompress(Span<const uint8> batch, Span<uint8> out) noexcept = 0;
};

} // namespace rocks
