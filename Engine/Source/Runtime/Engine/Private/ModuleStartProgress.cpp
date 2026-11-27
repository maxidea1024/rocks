#include <Core/Text/StringBuilder.h>
#include <Engine/ModuleStartProgress.h>
#include <Localization/Text.h>
#include <Platform/Memory.h>

namespace rocks {

namespace {

// 모듈 문구 칸이다(모듈 이름이 길면 code point 경계에서 잘린다 — 진행 문구 한 줄).
constexpr usize ModuleStartMessageBytes = 192;
constexpr StringView ModuleStartMessagePrefix = "Starting module ";

} // namespace

ModuleStartProgress::ModuleStartProgress(ModuleLoader& loader, Allocator& allocator) noexcept
    : loader_(loader), allocator_(allocator) {
    ModuleStartObserver observer;
    observer.phaseBegin = &PhaseBegin;
    observer.moduleStarting = &ModuleStarting;
    observer.phaseEnd = &PhaseEnd;
    observer.context = this;
    previous_ = loader_.SetStartObserver(observer);
}

ModuleStartProgress::~ModuleStartProgress() noexcept {
    CloseScope();
    (void)loader_.SetStartObserver(previous_);
}

void ModuleStartProgress::CloseScope() noexcept {
    if (scope_ != nullptr) {
        scope_->~ProgressScope();
        scope_ = nullptr;
    }
}

void ModuleStartProgress::PhaseBegin(void* context, usize moduleCount) noexcept {
    auto& self = *static_cast<ModuleStartProgress*>(context);
    self.CloseScope();
    self.scope_ = InitializeAt<ProgressScope>(self.scopeStorage_, static_cast<double>(moduleCount));
}

void ModuleStartProgress::ModuleStarting(void* context, StringView moduleName) noexcept {
    auto& self = *static_cast<ModuleStartProgress*>(context);
    if (self.scope_ == nullptr) {
        return;
    }

    // 문구는 보고가 쓰일 때만 만든다(바인딩 없는 실행은 할당이 없다).
    if (!self.scope_->IsReporting()) {
        self.scope_->EnterStep(1.0);
        return;
    }

    char storage[ModuleStartMessageBytes];
    FixedStringBuilder message{Span<char>(storage)};
    (void)message.TryAppend(ModuleStartMessagePrefix);
    (void)message.TryAppend(
        moduleName.LeftBytesAtCodePoint(message.Capacity() - message.View().ByteCount()));
    auto text = Text::Invariant(self.allocator_, message.View());
    if (text) {
        self.scope_->EnterStep(1.0, text.Value());
    } else {
        self.scope_->EnterStep(1.0);
    }
}

void ModuleStartProgress::PhaseEnd(void* context) noexcept {
    static_cast<ModuleStartProgress*>(context)->CloseScope();
}

} // namespace rocks
