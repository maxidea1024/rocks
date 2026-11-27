#pragma once

#include <Core/Containers/Array.h>
#include <Core/Text/StringView.h>
#include <Platform/Process.h>
#include <Progress/ProgressTaskHandle.h>

namespace rocks {

struct ChildProgressReaderDesc {
    // 자식의 최상위 작업을 붙일 부모 작업이다(필수 — 살아 있어야 한다).
    ProgressTaskId parent = NoProgressTask;
    // 보는 취소다(보통 부모 작업의 Token). 취소되면 자식 stdin에 "cancel" 줄을 한 번 쓴다.
    CancelToken cancel;
    // cancel 줄을 보낸 뒤 자식이 끝나기를 기다리는 시간(밀리초)이다. 넘으면 강제 종료한다.
    uint32 cancelGraceMilliseconds = DefaultChildCancelGraceMilliseconds;
};

// 자식 프로세스의 JSONL 진행(9.3절)을 부모 작업 아래 하위 작업으로 다시 만든다(Docs/Design/
// ProgressReporting 10.1절). 자식 작업 id → 부모 쪽 하위 작업 대응표는 MaxChildProgressTasks까지다.
// 하위 작업은 취소 불가(NotCancellable)로 만들고 부모 작업 token을 물려받는다 — 취소는 부모 쪽에서
// cancel 줄로 자식에게 간다.
//
// 사용 예(자식을 소유한 스레드):
//   platform::ProcessStartInfo info = ...;      // "--progress=jsonl" "--parent-control"
//   info.input = platform::ProcessStreamMode::Pipe;
//   info.output = platform::ProcessStreamMode::Pipe;   // stderr는 상속(10.1절 — 교착 방지)
//   platform::Process child;
//   if (auto started = child.Start(info); !started) { ... }
//   ChildProgressReaderDesc desc;
//   desc.parent = task.Id();
//   desc.cancel = task.Token();
//   ChildProgressReader reader(allocator, desc);
//   auto exit = reader.Run(child);              // 자식이 끝날 때까지
//
// 하위 작업은 begin의 weight·selfWeight로 만들고, progress의 self(자기 진행)가 있을 때만 자기
// 진행을 싣는다 — 부모 쪽 합성이 자식 쪽과 같다(자식의 합성 비율 frac은 표시용). 대응표에 없는
// 부모(상한으로 만들지 않은 작업)의 자식은 만들지 않는다. 깨진 줄은 Warning 한 번 뒤 무시한다. v가
// 다른 줄은 Unsupported로 그 자식의 진행을 끊는다(자식 프로세스는 계속 — 남은 하위 작업은
// Abandoned). 자식이 끝나면 남은 하위 작업을 비정상 종료면 Failed, 정상 종료면 Abandoned로 닫는다.
//
// 스레드: Process를 소유한 한 스레드가 Run(또는 FeedLine·Finish)을 부른다.
class ROCKS_PROGRESS_API ChildProgressReader {
public:
    ChildProgressReader(Allocator& allocator, const ChildProgressReaderDesc& desc) noexcept;
    ~ChildProgressReader() noexcept;
    ChildProgressReader(const ChildProgressReader&) = delete;
    ChildProgressReader& operator=(const ChildProgressReader&) = delete;

    // 자식이 끝날 때까지 stdout을 줄로 읽어 FeedLine하고, 반복 사이 취소를 본다(ChildControl
    // PollMilliseconds마다). 끝나면 Finish로 남은 하위 작업을 닫고 종료 결과를 돌려준다.
    // 오류: 출력이 Pipe가 아니면 InvalidArgument, Process 읽기·기다리기 오류는 그 오류(남은 하위
    // 작업은 Failed로 닫는다).
    Result<platform::ProcessExit> Run(platform::Process& child) noexcept;

    // 자식 출력 byte를 받아 줄로 나눠 FeedLine한다(Run이 쓴다 — 줄 경계는 호출 사이에 이어진다).
    // MaxProgressJsonlLineBytes를 넘는 줄은 다음 줄바꿈까지 버린다. 시험·다른 전송은 직접 부른다.
    void FeedBytes(const char* bytes, usize count) noexcept;

    // 줄 하나를 처리한다(끝 줄바꿈 없이). 오류: 깨진 줄 CorruptData(무시하고 계속해도 된다), 버전
    // 불일치 Unsupported(그 뒤 줄은 모두 무시), 대응표 초과 CapacityExceeded(그 하위 작업은 안
    // 보임).
    Result<void> FeedLine(StringView line) noexcept;

    // 자식이 끝났다: 남은 하위 작업을 exitedNormally면 Abandoned, 아니면 Failed로 닫는다.
    void Finish(bool exitedNormally) noexcept;

    // 지금 살아 있는 하위 작업 수다(시험·진단).
    usize LiveSubtasks() const noexcept;

    // cancel 줄을 보냈는지·강제 종료했는지다(시험·진단).
    bool SentCancel() const noexcept {
        return sentCancel_;
    }

    bool Terminated() const noexcept {
        return terminated_;
    }

private:
    struct Mirror {
        uint64 childId = 0;
        // 자식 쪽 부모 작업 id다(0이면 자식의 최상위).
        uint64 parentChildId = 0;
        ProgressTaskHandle task;
    };

    usize FindMirror(uint64 childId) const noexcept;
    Result<void> HandleBegin(const void* object) noexcept;
    Result<void> HandleProgress(const void* object) noexcept;
    Result<void> HandleEnd(const void* object) noexcept;
    void WarnOnce(uint32 bit, const char* message) noexcept;
    void CheckCancel(platform::Process& child) noexcept;

    Allocator& allocator_;
    ChildProgressReaderDesc desc_;
    Array<Mirror> mirrors_;
    Array<char> pending_;
    uint32 warned_ = 0;
    bool unsupported_ = false;
    // 줄 상한을 넘은 줄을 다음 줄바꿈까지 버리는 중이다.
    bool discarding_ = false;
    bool sentCancel_ = false;
    bool terminated_ = false;
    uint64 cancelSentNanoseconds_ = 0;
};

} // namespace rocks
