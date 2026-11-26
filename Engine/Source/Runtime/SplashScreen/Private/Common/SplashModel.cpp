#include "../SplashPresenter.h"

#include <Platform/Memory.h>
#include <Platform/PlatformMath.h>
#include <SplashScreen/SplashModel.h>

namespace rocks::splash {

namespace {

constexpr double NanosecondsPerSecond = 1'000'000'000.0;

// 백분율 문구 뒤의 사이(공백 둘 — 문구와 숫자를 떼어 읽히게)다.
constexpr StringView PercentSeparator = "%  ";

// 백분율의 최대값이다(끝난 작업).
constexpr uint32 FullPercent = 100;

double ClampFraction(double value) noexcept {
    if (!platform::PlatformMath::IsFinite(value) || value < 0.0) {
        return 0.0;
    }

    return value > 1.0 ? 1.0 : value;
}

// 십진 숫자를 써 넣는다(0–100). 쓴 byte 수다.
uint32 WritePercent(char* out, uint32 percent) noexcept {
    char digits[4];
    uint32 count = 0;
    do {
        digits[count++] = static_cast<char>('0' + percent % 10);
        percent /= 10;
    } while (percent != 0);

    for (uint32 index = 0; index < count; ++index) {
        out[index] = digits[count - 1 - index];
    }

    return count;
}

} // namespace

SplashModel::SplashModel(uint64 nowNanoseconds) noexcept
    : now_(nowNanoseconds), marqueeStart_(nowNanoseconds) {
}

void SplashModel::SetVisible(SplashTextSlot slot, bool visible) noexcept {
    bool& current = visible_[SlotIndex(slot)];
    if (current != visible) {
        current = visible;
        dirty_ = true;
    }
}

bool SplashModel::IsVisible(SplashTextSlot slot) const noexcept {
    return visible_[SlotIndex(slot)];
}

void SplashModel::SetBarVisible(bool visible) noexcept {
    if (barVisible_ != visible) {
        barVisible_ = visible;
        dirty_ = true;
    }
}

void SplashModel::StoreLine(Line& line, StringView utf8) noexcept {
    const StringView fitted = utf8.LeftBytesAtCodePoint(MaxSplashLineBytes);
    if (line.count == fitted.ByteCount() &&
        (fitted.IsEmpty() ||
         platform::MemoryCompare(line.bytes, fitted.Data(), fitted.ByteCount()) == 0)) {
        return;
    }

    if (!fitted.IsEmpty()) {
        platform::MemoryCopy(line.bytes, fitted.Data(), fitted.ByteCount());
    }

    line.count = static_cast<uint32>(fitted.ByteCount());
    dirty_ = true;
}

void SplashModel::SetText(SplashTextSlot slot, StringView utf8) noexcept {
    if (slot == SplashTextSlot::Progress) {
        statusOwnsProgress_ = false;
    }

    StoreLine(text_[SlotIndex(slot)], utf8);
}

StringView SplashModel::Text(SplashTextSlot slot) const noexcept {
    const Line& line = text_[SlotIndex(slot)];
    return StringView(line.bytes, line.count);
}

void SplashModel::RefreshProgressText() noexcept {
    if (!statusOwnsProgress_) {
        return;
    }

    char buffer[MaxSplashLineBytes];
    usize used = 0;
    if (determinate_) {
        uint32 percent = static_cast<uint32>(platform::PlatformMath::Floor(target_ * FullPercent));
        if (!finished_ && percent > SplashMaxUnfinishedPercent) {
            percent = SplashMaxUnfinishedPercent;
        }

        if (finished_) {
            percent = FullPercent;
        }

        used = WritePercent(buffer, percent);
        platform::MemoryCopy(buffer + used, PercentSeparator.Data(), PercentSeparator.ByteCount());
        used += PercentSeparator.ByteCount();
    }

    const StringView message = StringView(statusMessage_.bytes, statusMessage_.count)
                                   .LeftBytesAtCodePoint(MaxSplashLineBytes - used);
    if (!message.IsEmpty()) {
        platform::MemoryCopy(buffer + used, message.Data(), message.ByteCount());
        used += message.ByteCount();
    } else if (determinate_) {
        // 문구가 없으면 숫자만 둔다("42%").
        used -= PercentSeparator.ByteCount() - 1;
    }

    StoreLine(text_[SlotIndex(SplashTextSlot::Progress)], StringView(buffer, used));
}

void SplashModel::ApplyStatus(const SplashStatus& status) noexcept {
    // 원천이 보고하면 진행 칸은 원천의 것이다(SetText로 덮은 글자는 여기서 끝난다).
    bool changed = !statusOwnsProgress_;
    statusOwnsProgress_ = true;
    if (status.messageChanged) {
        const uint32 bytes = status.messageBytes > MaxSplashLineBytes
                                 ? static_cast<uint32>(MaxSplashLineBytes)
                                 : status.messageBytes;
        const StringView message =
            StringView(status.message, bytes).LeftBytesAtCodePoint(MaxSplashLineBytes);
        platform::MemoryCopy(statusMessage_.bytes, message.Data(), message.ByteCount());
        statusMessage_.count = static_cast<uint32>(message.ByteCount());
        changed = true;
    }

    const bool determinate = status.determinate || status.finished;
    // 목표는 뒤로 가지 않는다(중계가 보장하지만 원천이 어겨도 막대는 앞으로만 간다).
    double target = status.finished ? 1.0 : ClampFraction(status.fraction);
    if (determinate_ && target < target_) {
        target = target_;
    }

    if (determinate != determinate_ || target != target_ || status.finished != finished_) {
        if (determinate && !determinate_) {
            // 불확정에서 처음 비율로 바뀌면 그 자리부터 보간한다(0에서 출발).
            value_ = 0.0;
        }

        determinate_ = determinate;
        target_ = target;
        finished_ = status.finished;
        changed = true;
        dirty_ = true;
    }

    if (changed) {
        RefreshProgressText();
    }
}

void SplashModel::Advance(uint64 nowNanoseconds) noexcept {
    const uint64 previous = now_;
    now_ = nowNanoseconds < previous ? previous : nowNanoseconds;
    if (!barVisible_) {
        return;
    }

    if (!determinate_) {
        const uint64 elapsed = (now_ - marqueeStart_) % SplashMarqueePeriodNanoseconds;
        const double phase =
            static_cast<double>(elapsed) / static_cast<double>(SplashMarqueePeriodNanoseconds);
        if (phase != marqueePhase_) {
            marqueePhase_ = phase;
            dirty_ = true;
        }

        return;
    }

    if (value_ == target_) {
        return;
    }

    const double seconds = static_cast<double>(now_ - previous) / NanosecondsPerSecond;
    const double blend = 1.0 - platform::PlatformMath::Exp(-seconds / SplashBarEaseSeconds);
    double next = value_ + (target_ - value_) * blend;
    const double remaining = target_ - next;
    if (remaining <= SplashBarSettleEpsilon && remaining >= -SplashBarSettleEpsilon) {
        next = target_;
    }

    if (next != value_) {
        value_ = next;
        dirty_ = true;
    }
}

uint64 SplashModel::NextWake(uint64 nowNanoseconds, uint64 nextPollNanoseconds) const noexcept {
    const bool animating = barVisible_ && (!determinate_ || value_ != target_);
    if (!animating) {
        return nextPollNanoseconds;
    }

    const uint64 frame = nowNanoseconds + SplashAnimationIntervalNanoseconds;
    return frame < nextPollNanoseconds ? frame : nextPollNanoseconds;
}

SplashBarMode SplashModel::BarMode() const noexcept {
    if (!barVisible_) {
        return SplashBarMode::Hidden;
    }

    return determinate_ ? SplashBarMode::Determinate : SplashBarMode::Indeterminate;
}

double SplashModel::BarValue() const noexcept {
    return value_;
}

double SplashModel::BarTarget() const noexcept {
    return target_;
}

double SplashModel::MarqueePhase() const noexcept {
    return marqueePhase_;
}

bool SplashModel::ConsumeDirty() noexcept {
    const bool dirty = dirty_;
    dirty_ = false;
    return dirty;
}

} // namespace rocks::splash
