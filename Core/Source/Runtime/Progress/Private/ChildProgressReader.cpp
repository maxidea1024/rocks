#include "ProgressInternal.h"

#include <Core/FileError.h>
#include <Core/Guid.h>
#include <Core/Move.h>
#include <Core/Text/Json.h>
#include <Platform/Clock.h>
#include <Platform/PlatformMath.h>
#include <Progress/ChildProgressReader.h>
#include <Progress/ProgressHub.h>

namespace rocks {

namespace {

// 한 번에 읽는 자식 출력 크기다.
constexpr usize ChildReadChunkBytes = 4096;
constexpr uint64 ChildNanosecondsPerMillisecond = 1'000'000;
constexpr char ChildCancelLine[] = "cancel\n";
// 제목이 빈 자식 작업의 부모 쪽 제목이다.
constexpr StringView ChildUntitledTask = "Child task";

// 처음 한 번만 남기는 Warning 종류(warned_ 비트)다.
constexpr uint32 ChildWarnBroken = 1u << 0;
constexpr uint32 ChildWarnVersion = 1u << 1;
constexpr uint32 ChildWarnCapacity = 1u << 2;
constexpr uint32 ChildWarnLost = 1u << 3;

// 객체의 문자열 칸이다(없거나 문자열이 아니면 빈 값).
StringView ChildString(const JsonValue& object, StringView key) noexcept {
    const JsonValue value = object.Find(key);
    return value.IsValid() && value.IsString() ? value.GetString() : StringView();
}

// 객체의 부호 없는 정수 칸이다(없거나 맞지 않으면 false).
bool ChildUnsigned(const JsonValue& object, StringView key, uint64& out) noexcept {
    const JsonValue value = object.Find(key);
    if (!value.IsValid() || !value.IsNumber()) {
        return false;
    }

    auto number = value.TryGetUInt64();
    if (!number) {
        return false;
    }

    out = number.Value();
    return true;
}

// 자식이 보낸 문구를 Text로 만든다: 지역화 id가 있으면 Localized(부모가 다시 지역화할 수 있다),
// 없으면 Invariant다.
Result<Text> ChildText(Allocator& allocator, StringView source, StringView id,
                       uint64 revision) noexcept {
    if (!id.IsEmpty() && revision != 0) {
        if (auto parsed = TryParseGuid(id)) {
            if (auto localized = Text::Localized(allocator, parsed.Value(), revision, source)) {
                return localized;
            }
        }
    }

    return Text::Invariant(allocator, source);
}

ProgressPresentation ChildPresentation(StringView name) noexcept {
    if (name == StringView("blocking")) {
        return ProgressPresentation::Blocking;
    }

    return name == StringView("hidden") ? ProgressPresentation::Hidden
                                        : ProgressPresentation::Background;
}

bool ChildOutcome(StringView name, ProgressOutcome& out) noexcept {
    if (name == StringView("succeeded")) {
        out = ProgressOutcome::Succeeded;
    } else if (name == StringView("failed")) {
        out = ProgressOutcome::Failed;
    } else if (name == StringView("cancelled")) {
        out = ProgressOutcome::Cancelled;
    } else if (name == StringView("abandoned")) {
        out = ProgressOutcome::Abandoned;
    } else {
        return false;
    }

    return true;
}

bool ChildShape(StringView name, ProgressShape& out) noexcept {
    if (name == StringView("count")) {
        out = ProgressShape::Count;
    } else if (name == StringView("fraction")) {
        out = ProgressShape::Fraction;
    } else if (name == StringView("indeterminate")) {
        out = ProgressShape::Indeterminate;
    } else {
        return false;
    }

    return true;
}

} // namespace

ChildProgressReader::ChildProgressReader(Allocator& allocator,
                                         const ChildProgressReaderDesc& desc) noexcept
    : allocator_(allocator), desc_(desc), mirrors_(allocator), pending_(allocator) {
}

ChildProgressReader::~ChildProgressReader() noexcept {
    Finish(true);
}

void ChildProgressReader::WarnOnce(uint32 bit, const char* message) noexcept {
    if ((warned_ & bit) != 0) {
        return;
    }

    warned_ |= bit;
    Log::Warning(detail::LogProgress, "{Message}", StringView(message));
}

usize ChildProgressReader::FindMirror(uint64 childId) const noexcept {
    for (usize index = 0; index < mirrors_.Size(); ++index) {
        if (mirrors_[index].childId == childId) {
            return index;
        }
    }

    return mirrors_.Size();
}

usize ChildProgressReader::LiveSubtasks() const noexcept {
    return mirrors_.Size();
}

void ChildProgressReader::Finish(bool exitedNormally) noexcept {
    // 하위 작업(자식의 자식)이 뒤에 있으므로 뒤에서부터 닫는다.
    const ProgressOutcome outcome =
        exitedNormally ? ProgressOutcome::Abandoned : ProgressOutcome::Failed;
    for (usize index = mirrors_.Size(); index != 0; --index) {
        (void)mirrors_[index - 1].task.Finish(outcome);
    }

    mirrors_.Clear();
}

Result<void> ChildProgressReader::HandleBegin(const void* raw) noexcept {
    const JsonValue& object = *static_cast<const JsonValue*>(raw);
    uint64 id = 0;
    uint64 parent = 0;
    uint64 revision = 0;
    if (!ChildUnsigned(object, "id", id) || !ChildUnsigned(object, "parent", parent)) {
        return Result<void>::Failure(ErrorCode::CorruptData);
    }

    (void)ChildUnsigned(object, "titleRev", revision);
    if (FindMirror(id) != mirrors_.Size()) {
        return Result<void>::Success();
    }

    if (mirrors_.Size() >= MaxChildProgressTasks) {
        WarnOnce(ChildWarnCapacity,
                 "Child process has more live progress tasks than MaxChildProgressTasks; the "
                 "rest are not shown");
        return Result<void>::Failure(ErrorCode::CapacityExceeded);
    }

    StringView titleSource = ChildString(object, "title");
    if (titleSource.IsEmpty()) {
        titleSource = ChildUntitledTask;
    }

    auto title = ChildText(allocator_, titleSource, ChildString(object, "titleId"), revision);
    if (!title) {
        return Result<void>::Failure(title.Error());
    }

    // 자식의 하위 작업은 대응한 부모 쪽 하위 작업 아래로, 최상위는 desc.parent 아래로 붙인다. 부모
    // 미러가 없으면(상한으로 만들지 않았거나 이미 끝남) 이 작업도 만들지 않는다(안 보임).
    ProgressTaskDesc task;
    task.title = &title.Value();
    task.parent = desc_.parent;
    if (parent != 0) {
        const usize found = FindMirror(parent);
        if (found == mirrors_.Size()) {
            return Result<void>::Success();
        }

        task.parent = mirrors_[found].task.Id();
    }

    task.presentation = ChildPresentation(ChildString(object, "pres"));
    // 합성 가중치(10.1절): 유한한 양수(selfWeight는 0 이상)만 받고 나머지는 기본값이다.
    const JsonValue weight = object.Find("weight");
    if (weight.IsValid() && weight.IsNumber()) {
        if (auto value = weight.TryGetDouble();
            value && value.Value() > 0.0 && platform::PlatformMath::IsFinite(value.Value())) {
            task.weightInParent = value.Value();
        }
    }

    const JsonValue selfWeight = object.Find("selfWeight");
    if (selfWeight.IsValid() && selfWeight.IsNumber()) {
        if (auto value = selfWeight.TryGetDouble();
            value && value.Value() >= 0.0 && platform::PlatformMath::IsFinite(value.Value())) {
            task.selfWeight = value.Value();
        }
    }

    auto begun = ProgressHub::Get().Begin(task);
    if (!begun) {
        return Result<void>::Failure(begun.Error());
    }

    auto added = mirrors_.TryEmplaceBack();
    if (!added) {
        (void)begun.Value().Finish(ProgressOutcome::Abandoned);
        return Result<void>::Failure(added.Error());
    }

    added.Value()->childId = id;
    added.Value()->parentChildId = parent;
    added.Value()->task = Move(begun.Value());
    return Result<void>::Success();
}

Result<void> ChildProgressReader::HandleProgress(const void* raw) noexcept {
    const JsonValue& object = *static_cast<const JsonValue*>(raw);
    uint64 id = 0;
    if (!ChildUnsigned(object, "id", id)) {
        return Result<void>::Failure(ErrorCode::CorruptData);
    }

    const usize index = FindMirror(id);
    if (index == mirrors_.Size()) {
        return Result<void>::Success();
    }

    ProgressShape shape = ProgressShape::Indeterminate;
    if (!ChildShape(ChildString(object, "shape"), shape)) {
        return Result<void>::Failure(ErrorCode::CorruptData);
    }

    // 자기 진행 self가 있을 때만 싣는다(없으면 자식이 자기 보고를 안 했다 — 부모 쪽도 w₀ = 0으로
    // 미러된 하위 작업만 합성한다). 계수 모양은 done·total이 자기 진행이다.
    double self = 0.0;
    bool hasSelf = false;
    const JsonValue selfValue = object.Find("self");
    if (selfValue.IsValid() && selfValue.IsNumber()) {
        if (auto value = selfValue.TryGetDouble();
            value && platform::PlatformMath::IsFinite(value.Value())) {
            self = value.Value() < 0.0 ? 0.0 : (value.Value() > 1.0 ? 1.0 : value.Value());
            hasSelf = true;
        }
    }

    uint64 done = 0;
    uint64 total = 0;
    (void)ChildUnsigned(object, "done", done);
    (void)ChildUnsigned(object, "total", total);
    ProgressTaskHandle& task = mirrors_[index].task;
    if (shape == ProgressShape::Count) {
        (void)task.SetExternalProgress(ProgressShape::Count, 0.0, done, total);
    } else if (hasSelf) {
        (void)task.SetExternalProgress(ProgressShape::Fraction, self, 0, 0);
    }
    const StringView message = ChildString(object, "msg");
    if (!message.IsEmpty()) {
        uint64 revision = 0;
        (void)ChildUnsigned(object, "msgRev", revision);
        if (auto text = ChildText(allocator_, message, ChildString(object, "msgId"), revision)) {
            (void)task.SetMessage(text.Value());
        }
    }

    return Result<void>::Success();
}

Result<void> ChildProgressReader::HandleEnd(const void* raw) noexcept {
    const JsonValue& object = *static_cast<const JsonValue*>(raw);
    uint64 id = 0;
    ProgressOutcome outcome = ProgressOutcome::Abandoned;
    if (!ChildUnsigned(object, "id", id) ||
        !ChildOutcome(ChildString(object, "outcome"), outcome)) {
        return Result<void>::Failure(ErrorCode::CorruptData);
    }

    const usize index = FindMirror(id);
    if (index == mirrors_.Size()) {
        return Result<void>::Success();
    }

    const StringView detail = ChildString(object, "detail");
    if (detail.IsEmpty()) {
        (void)mirrors_[index].task.Finish(outcome);
    } else {
        uint64 revision = 0;
        (void)ChildUnsigned(object, "detailRev", revision);
        auto text = ChildText(allocator_, detail, ChildString(object, "detailId"), revision);
        if (text) {
            (void)mirrors_[index].task.Finish(outcome, text.Value());
        } else {
            (void)mirrors_[index].task.Finish(outcome);
        }
    }

    // 끝난 작업 아래 작업은 hub가 먼저 Abandoned로 닫았다(5.3절) — 대응표에서도 뺀다. 대응표는
    // begin 순서라 자손이 조상 뒤에 있으므로 한 번 앞으로 훑으며 끝난 id 사슬을 모은다.
    Array<uint64> ended(allocator_);
    (void)ended.TryPushBack(id);
    for (usize walk = index + 1; walk < mirrors_.Size(); ++walk) {
        if (ended.Contains(mirrors_[walk].parentChildId)) {
            (void)ended.TryPushBack(mirrors_[walk].childId);
        }
    }

    for (usize walk = mirrors_.Size(); walk != 0; --walk) {
        if (ended.Contains(mirrors_[walk - 1].childId)) {
            (void)mirrors_.TryRemoveAt(walk - 1);
        }
    }

    return Result<void>::Success();
}

Result<void> ChildProgressReader::FeedLine(StringView line) noexcept {
    if (unsupported_) {
        return Result<void>::Failure(ErrorCode::Unsupported);
    }

    if (line.EndsWith("\r")) {
        line = line.SliceBytes(0, line.ByteCount() - 1);
    }

    if (line.IsEmpty()) {
        return Result<void>::Success();
    }

    JsonDocument document(allocator_);
    if (!document.Parse(line).Succeeded() || !document.Root().IsObject()) {
        WarnOnce(ChildWarnBroken, "A child progress line could not be read; it is ignored");
        return Result<void>::Failure(ErrorCode::CorruptData);
    }

    const JsonValue root = document.Root();
    uint64 version = 0;
    if (!ChildUnsigned(root, "v", version) || version != ProgressJsonlVersion) {
        // 다른 판의 줄은 뜻을 모른다 — 이 자식의 진행을 끊는다(자식 프로세스는 계속).
        unsupported_ = true;
        WarnOnce(ChildWarnVersion,
                 "Child progress uses an unsupported line version; its progress is not shown");
        Finish(true);
        return Result<void>::Failure(ErrorCode::Unsupported);
    }

    const StringView event = ChildString(root, "e");
    Result<void> handled = Result<void>::Success();
    if (event == StringView("begin")) {
        handled = HandleBegin(&root);
    } else if (event == StringView("progress")) {
        handled = HandleProgress(&root);
    } else if (event == StringView("end")) {
        handled = HandleEnd(&root);
    } else if (event == StringView("lost")) {
        WarnOnce(ChildWarnLost, "The child process dropped progress events");
    } else {
        handled = Result<void>::Failure(ErrorCode::CorruptData);
    }

    if (!handled && handled.Error() == ErrorCode::CorruptData) {
        WarnOnce(ChildWarnBroken, "A child progress line could not be read; it is ignored");
    }

    return handled;
}

void ChildProgressReader::CheckCancel(platform::Process& child) noexcept {
    if (!sentCancel_) {
        if (desc_.cancel.IsCancellationRequested()) {
            sentCancel_ = true;
            cancelSentNanoseconds_ = platform::MonotonicNanoseconds();
            (void)child.WriteInput(ChildCancelLine, sizeof(ChildCancelLine) - 1,
                                   ChildControlPollMilliseconds);
        }

        return;
    }

    // 강제 종료는 성공했을 때만 표시한다(실패하면 다음 반복에서 다시 한다).
    if (!terminated_ &&
        platform::MonotonicNanoseconds() - cancelSentNanoseconds_ >=
            uint64{desc_.cancelGraceMilliseconds} * ChildNanosecondsPerMillisecond) {
        terminated_ = static_cast<bool>(child.Terminate());
    }
}

void ChildProgressReader::FeedBytes(const char* bytes, usize count) noexcept {
    for (usize index = 0; index < count; ++index) {
        const char c = bytes[index];
        if (c == '\n') {
            if (!discarding_) {
                (void)FeedLine(StringView(pending_.Data(), pending_.Size()));
            }

            pending_.Clear();
            discarding_ = false;
            continue;
        }

        if (discarding_) {
            continue;
        }

        if (pending_.Size() >= MaxProgressJsonlLineBytes || !pending_.TryPushBack(c)) {
            WarnOnce(ChildWarnBroken, "A child progress line could not be read; it is ignored");
            pending_.Clear();
            discarding_ = true;
        }
    }
}

Result<platform::ProcessExit> ChildProgressReader::Run(platform::Process& child) noexcept {
    char chunk[ChildReadChunkBytes];
    for (;;) {
        const platform::FileTransfer read = child.Read(platform::ProcessStream::Output, chunk,
                                                       sizeof(chunk), ChildControlPollMilliseconds);
        if (read.status.code == platform::FileCode::NoProgress) {
            // 자식이 끝났는데 EOF가 오지 않을 수 있다(자손이 출력 pipe를 물려받아 쥠) — 남은 출력을
            // 비우고 빠져나간다.
            if (child.Wait(0) == platform::WaitResult::Signaled) {
                for (;;) {
                    const platform::FileTransfer rest =
                        child.Read(platform::ProcessStream::Output, chunk, sizeof(chunk), 0);
                    if (!rest.status || rest.bytes == 0) {
                        break;
                    }

                    FeedBytes(chunk, rest.bytes);
                }

                break;
            }

            CheckCancel(child);
            continue;
        }

        if (read.status.code == platform::FileCode::EndOfFile) {
            break;
        }

        if (!read.status) {
            Finish(false);
            return Result<platform::ProcessExit>::Failure(
                read.status.code == platform::FileCode::NotOpen ? ErrorCode::InvalidArgument
                                                                : FileErrorCode(read.status));
        }

        FeedBytes(chunk, read.bytes);
        CheckCancel(child);
    }

    if (!discarding_ && !pending_.IsEmpty()) {
        (void)FeedLine(StringView(pending_.Data(), pending_.Size()));
    }

    pending_.Clear();
    // 출력이 닫혔다 — 끝나기를 기다린다(취소 유예가 지나면 강제 종료).
    platform::WaitResult waited = child.Wait(ChildControlPollMilliseconds);
    while (waited == platform::WaitResult::TimedOut) {
        CheckCancel(child);
        waited = child.Wait(ChildControlPollMilliseconds);
    }

    platform::ProcessExit exit;
    if (waited != platform::WaitResult::Signaled || !child.GetExit(exit)) {
        Finish(false);
        return Result<platform::ProcessExit>::Failure(ErrorCode::DependencyFailed);
    }

    const bool normal =
        exit.kind == platform::ProcessExitKind::Exited &&
        (exit.code == 0 || exit.code == static_cast<uint32>(ProgressCancelledExitCode));
    Finish(normal);
    return Result<platform::ProcessExit>::Success(exit);
}

} // namespace rocks
