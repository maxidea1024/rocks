#include "ProgressInternal.h"
#include "ProgressText.h"

#include <Core/Text/StringBuilder.h>
#include <Platform/PlatformMath.h>
#include <Progress/ConsoleProgressReporter.h>
#include <Progress/ProgressHub.h>

namespace rocks {

namespace {

// 한 줄 버퍼의 처음 크기다(넘으면 allocator로 늘어난다).
constexpr usize ConsoleLineInlineBytes = 256;
constexpr double ConsolePercentScale = 100.0;

const char* ConsoleOutcomeWord(ProgressOutcome outcome) noexcept {
    switch (outcome) {
    case ProgressOutcome::Succeeded:
        return "done";
    case ProgressOutcome::Failed:
        return "failed";
    case ProgressOutcome::Cancelled:
        return "cancelled";
    case ProgressOutcome::Abandoned:
        return "abandoned";
    }

    return "ended";
}

void ConsoleLogLine(void* context, bool warning, StringView line) noexcept {
    (void)context;
    if (warning) {
        Log::Warning(detail::LogProgress, "{Line}", line);
    } else {
        Log::Info(detail::LogProgress, "{Line}", line);
    }
}

} // namespace

ConsoleProgressReporter::ConsoleProgressReporter(Allocator& allocator,
                                                 const ConsoleProgressReporterDesc& desc) noexcept
    : allocator_(allocator), desc_(desc), snapshot_(allocator), title_(allocator),
      message_(allocator) {
    if (desc_.write == nullptr) {
        desc_.write = &ConsoleLogLine;
    }
}

void ConsoleProgressReporter::Write(bool warning, StringView line) noexcept {
    desc_.write(desc_.context, warning, line);
}

Result<void> ConsoleProgressReporter::Pump(uint64 nowNanoseconds) noexcept {
    if (auto taken = ProgressHub::Get().Snapshot(snapshot_, cursor_); !taken) {
        return taken;
    }

    // 시작·끝 사건은 바로 낸다(Hidden 작업도).
    for (const ProgressEvent& event : snapshot_.events) {
        if (!detail::ProgressSourceInto(allocator_, event.title, title_)) {
            continue;
        }

        StringBuilder<ConsoleLineInlineBytes> line(allocator_);
        (void)line.TryAppend(title_.View());
        if (event.kind == ProgressEventKind::Begin) {
            (void)line.TryAppend(": started");
            Write(false, line.View());
            continue;
        }

        (void)line.TryAppend(": ");
        (void)line.TryAppend(ConsoleOutcomeWord(event.outcome));
        if (event.detail.IsValid() &&
            detail::ProgressSourceInto(allocator_, event.detail, message_) && !message_.IsEmpty()) {
            (void)line.TryAppend(" (");
            (void)line.TryAppend(message_.View());
            (void)line.TryAppend(")");
        }

        Write(event.outcome != ProgressOutcome::Succeeded, line.View());
    }

    if (snapshot_.lostEvents != 0) {
        StringBuilder<ConsoleLineInlineBytes> line(allocator_);
        (void)line.TryAppendFormat("{} progress events were dropped", snapshot_.lostEvents);
        Write(true, line.View());
    }

    // 진행 줄: 첫 Pump는 시계만 맞추고, 그 뒤 간격마다 최상위 작업별 한 줄.
    if (!started_) {
        started_ = true;
        lastLineNanoseconds_ = nowNanoseconds;
        return Result<void>::Success();
    }

    if (nowNanoseconds - lastLineNanoseconds_ < desc_.intervalNanoseconds) {
        return Result<void>::Success();
    }

    lastLineNanoseconds_ = nowNanoseconds;
    for (const ProgressTaskView& view : snapshot_.tasks) {
        if (view.parent == NoProgressTask && view.presentation != ProgressPresentation::Hidden) {
            WriteProgressLine(view);
        }
    }

    return Result<void>::Success();
}

void ConsoleProgressReporter::WriteProgressLine(const ProgressTaskView& view) noexcept {
    if (!detail::ProgressSourceInto(allocator_, view.title, title_)) {
        return;
    }

    StringBuilder<ConsoleLineInlineBytes> line(allocator_);
    (void)line.TryAppend(title_.View());
    (void)line.TryAppend(": ");
    const auto percent =
        static_cast<uint64>(platform::PlatformMath::Floor(view.fraction * ConsolePercentScale));
    switch (view.shape) {
    case ProgressShape::Count: {
        // 계수 줄의 퍼센트는 그 줄의 완료/총량이다(하위 작업을 섞은 보이는 비율 D를 쓰면 같은 줄의
        // 수와 어긋난다).
        const uint64 counted = view.total != 0
                                   ? static_cast<uint64>(platform::PlatformMath::Floor(
                                         static_cast<double>(view.done) * ConsolePercentScale /
                                         static_cast<double>(view.total)))
                                   : 0;
        detail::ProgressAppendGrouped(line, view.done);
        (void)line.TryAppend(" / ");
        detail::ProgressAppendGrouped(line, view.total);
        (void)line.TryAppendFormat(" ({}%)", counted);
        break;
    }
    case ProgressShape::Fraction:
        (void)line.TryAppendFormat("{}%", percent);
        break;
    case ProgressShape::Indeterminate:
        (void)line.TryAppend("in progress");
        break;
    }

    // 한 줄 문구는 구간 문구 → 작업 문구다(제목은 이미 앞에 있다).
    const Text& message = view.DisplayMessage();
    if (&message != &view.title && detail::ProgressSourceInto(allocator_, message, message_) &&
        !message_.IsEmpty()) {
        (void)line.TryAppend(" - ");
        (void)line.TryAppend(message_.View());
    }

    Write(false, line.View());
}

} // namespace rocks
