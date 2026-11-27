#include "ProgressInternal.h"
#include "ProgressText.h"

#include <Core/Move.h>
#include <Core/Text/Json.h>
#include <Core/Text/StringBuilder.h>
#include <Platform/StandardStream.h>
#include <Progress/ProgressHub.h>
#include <Progress/ProgressJsonlWriter.h>

namespace rocks {

namespace {

// 줄 버퍼의 처음 크기와 지역화 id 버퍼 크기(Guid 문자열 36자)다.
constexpr usize JsonlLineInlineBytes = 512;
constexpr usize JsonlIdInlineBytes = 64;

const char* JsonlPresentationName(ProgressPresentation presentation) noexcept {
    switch (presentation) {
    case ProgressPresentation::Blocking:
        return "blocking";
    case ProgressPresentation::Background:
        return "background";
    case ProgressPresentation::Hidden:
        return "hidden";
    }

    return "background";
}

const char* JsonlShapeName(ProgressShape shape) noexcept {
    switch (shape) {
    case ProgressShape::Indeterminate:
        return "indeterminate";
    case ProgressShape::Fraction:
        return "fraction";
    case ProgressShape::Count:
        return "count";
    }

    return "indeterminate";
}

const char* JsonlOutcomeName(ProgressOutcome outcome) noexcept {
    switch (outcome) {
    case ProgressOutcome::Succeeded:
        return "succeeded";
    case ProgressOutcome::Failed:
        return "failed";
    case ProgressOutcome::Cancelled:
        return "cancelled";
    case ProgressOutcome::Abandoned:
        return "abandoned";
    }

    return "abandoned";
}

// 줄 하나를 만드는 도우미다(쓰기 실패는 처음 오류를 기억하고 뒤 호출은 그대로 둔다).
struct JsonlLine {
    explicit JsonlLine(Allocator& allocator) noexcept : out(allocator), writer(out, MakeOptions()) {
    }

    static JsonWriterOptions MakeOptions() noexcept {
        JsonWriterOptions options;
        options.compact = true;
        return options;
    }

    void Check(Result<void> result) noexcept {
        failed = failed || !result;
    }

    void Head(StringView event) noexcept {
        Check(writer.BeginObject());
        Check(writer.Key("v"));
        Check(writer.UInt64(ProgressJsonlVersion));
        Check(writer.Key("e"));
        Check(writer.String(event));
    }

    void Unsigned(StringView key, uint64 value) noexcept {
        Check(writer.Key(key));
        Check(writer.UInt64(value));
    }

    void Word(StringView key, StringView value) noexcept {
        Check(writer.Key(key));
        Check(writer.String(value));
    }

    // 문구 칸 셋(원문·지역화 id·판)이다. 원문은 MaxProgressJsonlTextBytes까지 자른다.
    void TextFields(Allocator& allocator, String& scratch, StringView key, StringView idKey,
                    StringView revisionKey, const Text& text) noexcept {
        if (!detail::ProgressSourceInto(allocator, text, scratch)) {
            scratch.Clear();
        }

        Word(key, scratch.View().LeftBytesAtCodePoint(MaxProgressJsonlTextBytes));
        StringBuilder<JsonlIdInlineBytes> id(allocator);
        uint64 revision = 0;
        detail::ProgressPersistentId(text, id, revision);
        Word(idKey, id.View());
        Unsigned(revisionKey, revision);
    }

    StringView Finish() noexcept {
        Check(writer.EndObject());
        Check(writer.Finish());
        return failed ? StringView() : out.View();
    }

    StringBuilder<JsonlLineInlineBytes> out;
    JsonWriter writer;
    bool failed = false;
};

} // namespace

ProgressJsonlWriter::ProgressJsonlWriter(Allocator& allocator,
                                         const ProgressJsonlWriterDesc& desc) noexcept
    : allocator_(allocator), desc_(desc), snapshot_(allocator), known_(allocator),
      text_(allocator) {
}

Result<void> ProgressJsonlWriter::Pump(uint64 nowNanoseconds) noexcept {
    return Run(nowNanoseconds, false);
}

Result<void> ProgressJsonlWriter::Flush(uint64 nowNanoseconds) noexcept {
    return Run(nowNanoseconds, true);
}

void ProgressJsonlWriter::Emit(StringView line) noexcept {
    if (line.IsEmpty()) {
        return;
    }

    // JsonWriter::Finish가 줄바꿈 하나를 붙였다. 표준 출력에는 그대로(한 번에 써서 줄이 섞이지
    // 않게), 받는 함수에는 떼고 넘긴다.
    if (desc_.write == nullptr) {
        (void)platform::WriteStandardStream(platform::StandardStream::Output, line.Data(),
                                            line.ByteCount());
        return;
    }

    const StringView trimmed =
        line.EndsWith("\n") ? line.SliceBytes(0, line.ByteCount() - 1) : line;
    desc_.write(desc_.context, trimmed);
}

usize ProgressJsonlWriter::FindKnown(ProgressTaskId id) const noexcept {
    for (usize index = 0; index < known_.Size(); ++index) {
        if (known_[index].id == id) {
            return index;
        }
    }

    return known_.Size();
}

const ProgressTaskView* ProgressJsonlWriter::FindView(ProgressTaskId id) const noexcept {
    for (const ProgressTaskView& view : snapshot_.tasks) {
        if (view.id == id) {
            return &view;
        }
    }

    return nullptr;
}

void ProgressJsonlWriter::BeginFromView(const ProgressTaskView& view) noexcept {
    if (FindKnown(view.id) != known_.Size()) {
        return;
    }

    // 부모 begin을 먼저(부모는 스냅샷에서 자식보다 앞에 있고 깊이는 작업 트리 깊이다).
    if (view.parent != NoProgressTask && FindKnown(view.parent) == known_.Size()) {
        if (const ProgressTaskView* parent = FindView(view.parent)) {
            BeginFromView(*parent);
        }
    }

    WriteBegin(view.id, view.parent, view.title, view.presentation, view.cancellable,
               view.weightInParent, view.selfWeight);
}

void ProgressJsonlWriter::WriteBegin(ProgressTaskId id, ProgressTaskId parent, const Text& title,
                                     ProgressPresentation presentation, bool cancellable,
                                     double weightInParent, double selfWeight) noexcept {
    if (FindKnown(id) != known_.Size()) {
        return;
    }

    JsonlLine line(allocator_);
    line.Head("begin");
    line.Unsigned("id", id);
    line.Unsigned("parent", parent);
    line.TextFields(allocator_, text_, "title", "titleId", "titleRev", title);
    line.Word("pres", JsonlPresentationName(presentation));
    line.Check(line.writer.Key("cancel"));
    line.Check(line.writer.Bool(cancellable));
    line.Check(line.writer.Key("weight"));
    line.Check(line.writer.Double(weightInParent));
    line.Check(line.writer.Key("selfWeight"));
    line.Check(line.writer.Double(selfWeight));
    const StringView text = line.Finish();
    if (text.IsEmpty()) {
        return;
    }

    Emit(text);
    if (auto added = known_.TryEmplaceBack(allocator_)) {
        added.Value()->id = id;
    }
}

void ProgressJsonlWriter::WriteEnd(ProgressTaskId id, ProgressOutcome outcome,
                                   const Text& detail) noexcept {
    JsonlLine line(allocator_);
    line.Head("end");
    line.Unsigned("id", id);
    line.Word("outcome", JsonlOutcomeName(outcome));
    line.TextFields(allocator_, text_, "detail", "detailId", "detailRev", detail);
    Emit(line.Finish());
    const usize index = FindKnown(id);
    if (index != known_.Size()) {
        (void)known_.TryRemoveAt(index);
    }
}

void ProgressJsonlWriter::WriteProgress(const ProgressTaskView& view, Known& known) noexcept {
    // 한 줄 문구는 구간 문구 → 작업 문구다(제목은 begin 줄에 있다).
    const Text& message = view.DisplayMessage();
    const Text none;
    const Text& shown = &message != &view.title ? message : none;
    if (!detail::ProgressSourceInto(allocator_, shown, text_)) {
        text_.Clear();
    }

    const double self = view.selfReported ? view.selfFraction : -1.0;
    const bool same =
        known.reported && known.shape == view.shape && known.fraction == view.fraction &&
        known.self == self && known.done == view.done && known.total == view.total &&
        known.cancelRequested == view.cancelRequested && known.message.View() == text_.View();
    if (same) {
        return;
    }

    known.reported = true;
    known.shape = view.shape;
    known.fraction = view.fraction;
    known.self = self;
    known.done = view.done;
    known.total = view.total;
    known.cancelRequested = view.cancelRequested;
    (void)known.message.TryAssign(text_.View());

    JsonlLine line(allocator_);
    line.Head("progress");
    line.Unsigned("id", view.id);
    line.Word("shape", JsonlShapeName(view.shape));
    line.Check(line.writer.Key("frac"));
    line.Check(line.writer.Double(view.fraction));
    if (view.shape == ProgressShape::Count) {
        line.Unsigned("done", view.done);
        line.Unsigned("total", view.total);
    }

    line.TextFields(allocator_, text_, "msg", "msgId", "msgRev", shown);
    if (view.selfReported) {
        line.Check(line.writer.Key("self"));
        line.Check(line.writer.Double(view.selfFraction));
    }

    line.Check(line.writer.Key("cancelRequested"));
    line.Check(line.writer.Bool(view.cancelRequested));
    Emit(line.Finish());
}

Result<void> ProgressJsonlWriter::Run(uint64 nowNanoseconds, bool force) noexcept {
    if (auto taken = ProgressHub::Get().Snapshot(snapshot_, cursor_); !taken) {
        return taken;
    }

    for (const ProgressEvent& event : snapshot_.events) {
        if (event.kind == ProgressEventKind::Begin) {
            // 아직 살아 있으면 스냅샷의 가중치·부모 순서로, 이미 끝났으면 사건 값으로 쓴다.
            if (const ProgressTaskView* view = FindView(event.id)) {
                BeginFromView(*view);
            } else {
                if (event.parent != NoProgressTask && FindKnown(event.parent) == known_.Size()) {
                    if (const ProgressTaskView* parent = FindView(event.parent)) {
                        BeginFromView(*parent);
                    }
                }

                WriteBegin(event.id, event.parent, event.title, event.presentation,
                           event.cancel == ProgressCancel::UserCancellable, 1.0, 1.0);
            }
        } else if (FindKnown(event.id) != known_.Size()) {
            WriteEnd(event.id, event.outcome, event.detail);
        }
    }

    if (snapshot_.lostEvents != 0) {
        JsonlLine line(allocator_);
        line.Head("lost");
        line.Unsigned("count", snapshot_.lostEvents);
        Emit(line.Finish());
        // 유실된 end를 보충한다: begin을 쓴 작업 가운데 스냅샷에 없는 것은 끝났다(결과는 모른다).
        const Text none;
        for (usize index = known_.Size(); index != 0; --index) {
            const ProgressTaskId id = known_[index - 1].id;
            if (FindView(id) == nullptr) {
                WriteEnd(id, ProgressOutcome::Abandoned, none);
            }
        }
    }

    // begin 사건을 놓친 살아 있는 작업(이 writer보다 먼저 시작)은 begin 줄을 먼저 쓴다(부모가 앞).
    for (const ProgressTaskView& view : snapshot_.tasks) {
        BeginFromView(view);
    }

    const bool due = force || !started_ ||
                     nowNanoseconds - lastProgressNanoseconds_ >= desc_.intervalNanoseconds;
    if (!due) {
        return Result<void>::Success();
    }

    started_ = true;
    lastProgressNanoseconds_ = nowNanoseconds;
    for (const ProgressTaskView& view : snapshot_.tasks) {
        const usize index = FindKnown(view.id);
        if (index != known_.Size()) {
            WriteProgress(view, known_[index]);
        }
    }

    return Result<void>::Success();
}

} // namespace rocks
