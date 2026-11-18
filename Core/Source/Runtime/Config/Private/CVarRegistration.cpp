#include "ConfigState.h"

#include <Platform/Atomics.h>
#include <Platform/Clock.h>
#include <Platform/Thread.h>

// CVar 핸들 등록과 번호다(Docs/Design/CVarSystem.ko.md 5절 — CVR1). 핸들은 독립 콘솔 설정과 같은
// 저장소(ConsoleSettingState)에 들어가 대기 값·층·출처·콘솔 경로를 그대로 쓴다. 번호는 프로세스
// 전체에서 한 번 정해진다(조각 snapshot의 칸 번호 — CVR2).
namespace rocks::config {

namespace detail {

uint32 ConsoleFlagsOfDefinition(const cvar::detail::VarDefinition& definition) noexcept {
    uint32 flags = 0;
    if (definition.kind == cvar::HandleKind::Boot) {
        flags |= static_cast<uint32>(ConsoleSettingFlags::StartupOnly);
    }

    if (cvar::HasFlag(definition.flags, cvar::VarFlags::Cheat)) {
        flags |= static_cast<uint32>(ConsoleSettingFlags::DevelopmentOnly);
    }

    if (cvar::HasFlag(definition.flags, cvar::VarFlags::UserSetting)) {
        flags |= static_cast<uint32>(ConsoleSettingFlags::UserConfig);
    }

    if (cvar::HasFlag(definition.flags, cvar::VarFlags::ConsoleReadOnly)) {
        flags |= static_cast<uint32>(ConsoleSettingFlags::ConsoleReadOnly);
    }

    return flags;
}

} // namespace detail

namespace {

using cvar::HandleKind;
using cvar::VarFlags;
using cvar::detail::VarDefinition;
using detail::ConfigState;
using detail::ConsoleSettingState;

// 다음에 줄 핸들 번호다(프로세스 전체 — 상수 초기화, 전역 생성자 없음). 여러 문맥이 다른 스레드에서
// 동시에 등록해도 공유하는 가변 상태는 이 카운터와 핸들의 번호 칸뿐이다(10.1절).
uint32 NextHandleIndex = 0;

// 핸들에 번호가 없으면 하나를 준다(첫 Register — CAS 한 번). 두 문맥이 같은 핸들을 동시에 처음
// 등록하면 진 쪽이 받은 번호는 버려진다(칸 하나가 빈다 — 번호의 뜻은 바뀌지 않는다).
void AssignIndex(const VarDefinition& definition) noexcept {
    uint32* slot = &definition.index;
    if (platform::AtomicLoad<platform::MemoryOrder::Relaxed>(slot) != cvar::Index::None) {
        return;
    }

    const uint32 fresh =
        platform::AtomicFetchAdd<platform::MemoryOrder::Relaxed>(&NextHandleIndex, 1u);
    if (fresh == cvar::Index::None) {
        platform::Fatal("CVar handle index space exhausted");
    }

    uint32 expected = cvar::Index::None;
    static_cast<void>(platform::AtomicCompareExchangeStrong<platform::MemoryOrder::Relaxed,
                                                            platform::MemoryOrder::Relaxed>(
        slot, expected, fresh));
}

// 이름의 첫 마디가 등록된 영역인지다.
bool HasKnownRegion(const ConfigState& state, StringView name) noexcept {
    StringView region;
    StringView rest;
    if (!name.Split(".", region, rest, StringComparison::CaseSensitive)) {
        return false;
    }

    for (const String& candidate : state.regions) {
        if (candidate.View() == region) {
            return true;
        }
    }

    return false;
}

// 같은 이름의 기존 설정과 정의가 같은 타입인지다(enum은 열거자 스키마까지 같아야 한다).
// 설정 struct 필드의 콘솔 설정은 type 칸을 쓰지 않으므로 값 종류(kind)로 비교한다. enum은 등록된
// 스키마 ID가 같으면 같은 enum이고, ID가 없는 enum(ADL 이름 표)은 스키마 주소로 비교한다.
bool SameType(const ConsoleSettingState& setting, const VarDefinition& definition) noexcept {
    if (setting.kind != ConsoleValueKind(definition.type)) {
        return false;
    }

    if (setting.kind != ValueKind::Enum) {
        return true;
    }

    const EnumSchema& schema = definition.enumSchema();
    return schema.id != TypeId{} ? setting.enumSchema->id == schema.id
                                 : setting.enumSchema == &schema;
}

// 정의 둘이 같은 타입인지다.
bool SameType(const VarDefinition& left, const VarDefinition& right) noexcept {
    return left.type == right.type &&
           (left.type != ConsoleValueType::Enum || left.enumSchema == right.enumSchema);
}

// 기본값을 setting.defaultValue에 쓴다. Name은 문맥의 NamePool에 intern해야 하므로 값 표기로
// 읽는다.
Result<void> FillDefault(ConfigState& state, const VarDefinition& definition,
                         ConsoleSettingState& setting) noexcept {
    FieldValue& value = setting.defaultValue;
    value.kind = setting.kind;
    switch (definition.type) {
    case ConsoleValueType::Bool:
        value.boolean = definition.defaultValue.boolean;
        return Result<void>::Success();
    case ConsoleValueType::Int32:
    case ConsoleValueType::Int64:
        value.signedInteger = definition.defaultValue.integer;
        return Result<void>::Success();
    case ConsoleValueType::Float:
        value.float32 = static_cast<float>(definition.defaultValue.real);
        return Result<void>::Success();
    case ConsoleValueType::Double:
        value.float64 = definition.defaultValue.real;
        return Result<void>::Success();
    case ConsoleValueType::String:
        return value.string.TryAssign(definition.defaultValue.text);
    case ConsoleValueType::Enum:
        value.signedInteger = definition.defaultValue.integer;
        value.type = setting.enumSchema->id;
        return Result<void>::Success();
    case ConsoleValueType::Name:
        break;
    }

    ConfigDiagnosticCode code = ConfigDiagnosticCode::InvalidSetting;
    FieldValue parsed(state.allocator);
    auto checked = detail::CheckConsoleValue(state, setting, ConfigLayer::CodeDefault,
                                             definition.defaultValue.text, parsed, code);
    if (!checked) {
        return Result<void>::Failure(checked.Error());
    }

    // Name 기본값의 문법(출력 가능한 ASCII·길이)은 컴파일 때 검사했으므로 여기서 거부되는 경우는
    // 문맥 Registry에 NamePool이 없을 때다(pool 자체의 오류는 위 Failure로 온다).
    if (!checked.Value().IsEmpty()) {
        return Result<void>::Failure(ErrorCode::Unsupported);
    }

    return detail::CopyFieldValue(parsed, value);
}

// 정의 하나의 콘솔 설정 상태(이름·도움말·기본값)를 만든다. 대기 값은 넘기지 않는다(진단을 남기므로
// 모든 검사가 끝난 뒤 RegisterHandles가 넘긴다).
Result<void> MakeSetting(ConfigState& state, const VarDefinition& definition,
                         UniquePtr<ConsoleSettingState>& out) noexcept {
    auto made = UniquePtr<ConsoleSettingState>::TryMake(state.allocator, state.allocator);
    if (!made) {
        return Result<void>::Failure(made.Error());
    }

    ConsoleSettingState& setting = *made.Value();
    setting.definition = &definition;
    setting.type = definition.type;
    setting.kind = ConsoleValueKind(definition.type);
    setting.flags = detail::ConsoleFlagsOfDefinition(definition);
    if (definition.type == ConsoleValueType::Enum) {
        setting.enumSchema = &definition.enumSchema();
    }

    Result<void> filled = setting.name.TryAssign(definition.name);
    if (filled) {
        filled = setting.help.TryAssign(definition.help);
    }

    if (filled) {
        filled = FillDefault(state, definition, setting);
    }

    if (!filled) {
        return filled;
    }

    out = Move(made.Value());
    return Result<void>::Success();
}

} // namespace

Result<void> ConfigContext::RegisterHandles(Span<const VarDefinition* const> definitions) noexcept {
    ConfigState* state = state_.Get();
    if (state == nullptr) {
        return Result<void>::Failure(ErrorCode::InvalidArgument);
    }

    if (platform::CurrentThreadId() != state->owner) {
        return Result<void>::Failure(ErrorCode::WrongThread);
    }

    // 1단계: 모두 검사하고 설정 상태(기본값 포함)를 만든다. 여기까지는 문맥을 바꾸지
    // 않는다(하나라도 거부하면 아무것도 등록하지 않는다). added는 새로 등록할 정의다.
    Array<const VarDefinition*> added(*allocator_);
    // 핸들은 정적 저장 기간이어야 한다(번호·주소가 정체성이고 문맥보다 오래 살아야 한다). 이
    // 스레드의 스택 범위를 한 번 구해 모든 정의에 쓴다.
    [[maybe_unused]] platform::ThreadStackRange stack;
    if constexpr (!IsReleaseBuild) {
        stack = platform::CurrentThreadStackRange();
    }

    for (const VarDefinition* definition : definitions) {
        if constexpr (IsReleaseBuild) {
            if (cvar::HasFlag(definition->flags, VarFlags::Cheat)) {
                continue;
            }
        } else if (stack.Contains(definition)) {
            return Result<void>::Failure(ErrorCode::InvalidArgument);
        }

        if (!HasKnownRegion(*state, definition->name)) {
            return Result<void>::Failure(ErrorCode::InvalidArgument);
        }

        const usize existing = state->FindSetting(definition->name);
        if (existing != IndexNone) {
            const ConsoleSettingState& setting = *state->settings[existing];
            if (setting.definition == definition) {
                continue;
            }

            return Result<void>::Failure(SameType(setting, *definition) ? ErrorCode::AlreadyExists
                                                                        : ErrorCode::TypeMismatch);
        }

        if (state->FindCommand(definition->name) != IndexNone) {
            return Result<void>::Failure(ErrorCode::AlreadyExists);
        }

        bool repeated = false;
        for (const VarDefinition* other : added) {
            if (other == definition) {
                repeated = true;
            } else if (other->name == definition->name) {
                return Result<void>::Failure(SameType(*other, *definition)
                                                 ? ErrorCode::AlreadyExists
                                                 : ErrorCode::TypeMismatch);
            }
        }

        if (!repeated) {
            if (auto pushed = added.TryPushBack(definition); !pushed) {
                return pushed;
            }
        }
    }

    if (added.IsEmpty()) {
        return Result<void>::Success();
    }

    Array<UniquePtr<ConsoleSettingState>> made(*allocator_);
    if (auto reserved = made.TryReserve(added.Size()); !reserved) {
        return reserved;
    }

    for (const VarDefinition* definition : added) {
        UniquePtr<ConsoleSettingState> setting;
        if (auto created = MakeSetting(*state, *definition, setting); !created) {
            return created;
        }

        if (auto pushed = made.TryPushBack(Move(setting)); !pushed) {
            return pushed;
        }
    }

    if (auto reserved = state->settings.TryReserve(state->settings.Size() + made.Size());
        !reserved) {
        return reserved;
    }

    // 2단계: 이미 들어온 층 입력(대기 값)을 넘긴다. 거부한 값은 진단을 남기는데, 넘기다
    // 실패하면(할당 실패) 이번 호출이 남긴 진단을 되돌려 실패한 등록이 진단을 남기지 않게 한다.
    const ConfigDiagnosticsMark mark = state->diagnostics.Mark();
    for (UniquePtr<ConsoleSettingState>& setting : made) {
        if (auto adopted = detail::AdoptPendingValues(*state, *setting); !adopted) {
            state->diagnostics.Rewind(mark);
            return adopted;
        }
    }

    // 3단계: 번호를 주고(첫 등록 — 프로세스 전체) 새 칸만 더한 snapshot을 바로 게시한다(VD17 — 기존
    // 칸은 그대로다. 게시가 실패하면 등록하지 않는다 — 받은 번호는 핸들에 남는다).
    Array<detail::SlotWrite> writes(*allocator_);
    if (auto reserved = writes.TryReserve(made.Size()); !reserved) {
        state->diagnostics.Rewind(mark);
        return reserved;
    }

    for (UniquePtr<ConsoleSettingState>& setting : made) {
        AssignIndex(*setting->definition);
        detail::SlotWrite write;
        write.index =
            platform::AtomicLoad<platform::MemoryOrder::Relaxed>(&setting->definition->index);
        write.value = &setting->value;
        static_cast<void>(writes.TryPushBack(write)); // 예약했다
    }

    if (auto published = detail::PublishSnapshot(*state, nullptr, writes.View()); !published) {
        state->diagnostics.Rewind(mark);
        return published;
    }

    // 4단계: 넣는다(자리를 확보했으므로 실패하지 않는다).
    for (UniquePtr<ConsoleSettingState>& setting : made) {
        if (auto pushed = state->settings.TryPushBack(Move(setting)); !pushed) {
            platform::Fatal("CVar registration lost its reserved capacity");
        }
    }

    return Result<void>::Success();
}

} // namespace rocks::config
