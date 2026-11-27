#pragma once

#include <Core/Containers/Array.h>
#include <Core/Text/String.h>
#include <Progress/ProgressSnapshot.h>

namespace rocks {

// 기계 판독 줄 하나를 받는다(끝 줄바꿈 없이, 줄 하나 = JSON 하나).
using ProgressLineWriter = void (*)(void* context, StringView line) noexcept;

struct ProgressJsonlWriterDesc {
    // null이면 표준 출력에 줄바꿈과 함께 쓴다(--progress=jsonl). 시험은 줄을 모은다.
    ProgressLineWriter write = nullptr;
    void* context = nullptr;
    // progress 줄 간격(나노초)이다.
    uint64 intervalNanoseconds = DefaultJsonlProgressIntervalNanoseconds;
};

// 기계 판독 진행 줄(JSONL)을 쓴다(Docs/Design/ProgressReporting 9.3절). 자식 프로세스가 부모에게
// 진행을 보내는 형식이고(10.1절 ChildProgressReader가 읽는다) 자동화가 읽는다.
//   {"v":1,"e":"begin","id":7,"parent":0,"title":"Cooking packages","titleId":"","titleRev":0,
//    "pres":"background","cancel":true,"weight":1.0,"selfWeight":1.0}
//   {"v":1,"e":"progress","id":7,"shape":"count","frac":0.25,"done":1,"total":4,"msg":"…",
//    "msgId":"","msgRev":0,"self":0.25,"cancelRequested":false}
//   {"v":1,"e":"end","id":7,"outcome":"succeeded","detail":"","detailId":"","detailRev":0}
//   {"v":1,"e":"lost","count":12}
// begin·end·lost는 Pump 때 바로, progress는 간격마다 바뀐 작업만 쓴다. 스냅샷에 처음 보이는 작업은
// begin 사건을 놓쳤어도 begin 줄을 먼저 쓰고, 부모 begin은 언제나 자식보다 먼저다(받는 쪽이 모르는
// id를 받지 않게). 사건이 유실됐으면(lost) 이미 begin을 쓴 작업 가운데 스냅샷에서 사라진 것에
// abandoned end를 보충한다. begin에는 합성 가중치(weight·selfWeight), progress에는 자기 보고가 있을
// 때 자기 진행(self)을 실어 받는 쪽이 같은 합성을 한다(10.1절). 문구 칸은
// MaxProgressJsonlTextBytes까지 UTF-8 경계에서 자른다.
//
// 사용 예(자식 명령):
//   ProgressJsonlWriter jsonl(allocator);
//   while (Working()) { DoSomeWork(); (void)jsonl.Pump(platform::MonotonicNanoseconds()); }
//   (void)jsonl.Flush(platform::MonotonicNanoseconds());   // 끝나기 전에 남은 줄
//
// 스레드: 한 스레드가 Pump·Flush를 부른다.
class ROCKS_PROGRESS_API ProgressJsonlWriter {
public:
    explicit ProgressJsonlWriter(Allocator& allocator,
                                 const ProgressJsonlWriterDesc& desc = {}) noexcept;
    ProgressJsonlWriter(const ProgressJsonlWriter&) = delete;
    ProgressJsonlWriter& operator=(const ProgressJsonlWriter&) = delete;

    // 사건 줄을 쓰고, 간격이 지났으면 바뀐 작업의 progress 줄을 쓴다.
    // 오류: ProgressHub::Snapshot의 오류(NotReady·ShuttingDown·할당 실패).
    Result<void> Pump(uint64 nowNanoseconds) noexcept;

    // Pump와 같되 간격과 상관없이 바뀐 작업의 progress 줄을 쓴다(명령이 끝나기 직전·시험).
    Result<void> Flush(uint64 nowNanoseconds) noexcept;

private:
    // 이미 begin 줄을 쓴 작업과 마지막으로 쓴 progress 값이다.
    struct Known {
        explicit Known(Allocator& allocator) noexcept : message(allocator) {
        }

        Known(Known&&) noexcept = default;
        Known& operator=(Known&&) noexcept = default;

        ProgressTaskId id = NoProgressTask;
        bool reported = false;
        ProgressShape shape = ProgressShape::Indeterminate;
        double fraction = 0.0;
        double self = -1.0;
        uint64 done = 0;
        uint64 total = 0;
        bool cancelRequested = false;
        String message;
    };

    Result<void> Run(uint64 nowNanoseconds, bool force) noexcept;
    usize FindKnown(ProgressTaskId id) const noexcept;
    const ProgressTaskView* FindView(ProgressTaskId id) const noexcept;
    // 부모 begin을 먼저 쓴 뒤 그 작업의 begin 줄을 쓴다(이미 썼으면 아무 일도 하지 않는다).
    void BeginFromView(const ProgressTaskView& view) noexcept;
    void WriteBegin(ProgressTaskId id, ProgressTaskId parent, const Text& title,
                    ProgressPresentation presentation, bool cancellable, double weightInParent,
                    double selfWeight) noexcept;
    void WriteEnd(ProgressTaskId id, ProgressOutcome outcome, const Text& detail) noexcept;
    void WriteProgress(const ProgressTaskView& view, Known& known) noexcept;
    void Emit(StringView line) noexcept;

    Allocator& allocator_;
    ProgressJsonlWriterDesc desc_;
    ProgressSnapshot snapshot_;
    uint64 cursor_ = 0;
    uint64 lastProgressNanoseconds_ = 0;
    bool started_ = false;
    Array<Known> known_;
    String text_;
};

} // namespace rocks
