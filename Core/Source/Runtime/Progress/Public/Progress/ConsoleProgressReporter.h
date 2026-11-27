#pragma once

#include <Core/Text/String.h>
#include <Progress/ProgressSnapshot.h>

namespace rocks {

// 사람이 읽는 줄 하나를 받는다(warning이면 실패·취소·Abandoned 끝).
using ProgressLogWriter = void (*)(void* context, bool warning, StringView line) noexcept;

struct ConsoleProgressReporterDesc {
    // null이면 Log API(범주 "Progress", Info·Warning)로 낸다 — 로그는 stderr라 stdout JSONL과
    // 섞이지 않는다. 시험은 줄을 모은다.
    ProgressLogWriter write = nullptr;
    void* context = nullptr;
    // 최상위 작업 진행 줄 간격(나노초)이다.
    uint64 intervalNanoseconds = DefaultConsoleProgressIntervalNanoseconds;
};

// 명령줄 진행 보고기다(Docs/Design/ProgressReporting 9.1절, UE 명령줄 FFeedbackContext 대응). 펌프
// 주인(명령의 주 스레드 반복 또는 ProgressPumpThread)이 Pump를 부른다. 시작·끝 사건은 그때 바로,
// 진행 줄은 간격마다 최상위 작업별 한 줄이다:
//   Cooking packages: 1,234 / 5,000 (24%) - Maps/Arena      (계수)
//   Building mesh: 41% - Welding vertices                    (비율)
//   Loading: in progress - Mounting packs                    (불확정)
// Hidden 작업은 시작·끝 줄만 낸다. 문구는 원문(9.2절)이다.
//
// 사용 예:
//   ConsoleProgressReporter console(allocator);
//   while (Working()) { DoSomeWork(); (void)console.Pump(platform::MonotonicNanoseconds()); }
//
// 스레드: 한 스레드가 Pump를 부른다(자기 스냅샷·cursor를 가진 소비자 하나).
class ROCKS_PROGRESS_API ConsoleProgressReporter {
public:
    explicit ConsoleProgressReporter(Allocator& allocator,
                                     const ConsoleProgressReporterDesc& desc = {}) noexcept;
    ConsoleProgressReporter(const ConsoleProgressReporter&) = delete;
    ConsoleProgressReporter& operator=(const ConsoleProgressReporter&) = delete;

    // 스냅샷을 당겨 줄을 낸다. 첫 Pump는 진행 줄 시계를 맞추기만 한다(그 뒤 간격마다).
    // 오류: ProgressHub::Snapshot의 오류(NotReady·ShuttingDown·할당 실패).
    Result<void> Pump(uint64 nowNanoseconds) noexcept;

private:
    void Write(bool warning, StringView line) noexcept;
    void WriteProgressLine(const ProgressTaskView& view) noexcept;

    Allocator& allocator_;
    ConsoleProgressReporterDesc desc_;
    ProgressSnapshot snapshot_;
    uint64 cursor_ = 0;
    uint64 lastLineNanoseconds_ = 0;
    bool started_ = false;
    String title_;
    String message_;
};

} // namespace rocks
