#include "ConfigState.h"

#include <Platform/Atomics.h>
#include <Platform/Clock.h>
#include <Platform/Thread.h>

// CVar 핸들 등록과 번호다(Docs/Design/CVarSystem.ko.md 5절 — CVR1). 핸들은 독립 콘솔 설정과 같은
// 저장소(ConsoleSettingState)에 들어가 대기 값·층·출처·콘솔 경로를 그대로 쓴다. 번호는 프로세스
// 전체에서 한 번 정해진다(조각 snapshot의 칸 번호 — CVR2).
namespace rocks::config {

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

// 정의의 성질을 콘솔 설정 플래그로 옮긴다. 부팅 변수는 StartupOnly(CompleteStartup 뒤 고정 — 9절),
// Cheat는 DevelopmentOnly(VD5 — 출시 규칙 시험에서 숨김·고정)다. 모든 CVar는 snapshot에 든다(VD6).
uint32 ConsoleFlagsOf(const VarDefinition& definition) noexcept {
    uint32 flags = static_cast<uint32>(ConsoleSettingFlags::RenderRead);
    if (definition.kind == HandleKind::Boot) {
        flags |= static_cast<uint32>(ConsoleSettingFlags::StartupOnly);
    }

    if (cvar::HasFlag(definition.flags, VarFlags::Cheat)) {
        flags |= static_cast<uint32>(ConsoleSettingFlags::DevelopmentOnly);
    }

    if (cvar::HasFlag(definition.flags, VarFlags::UserSetting)) {
        flags |= static_cast<uint32>(ConsoleSettingFlags::UserConfig);
    }

    if (cvar::HasFlag(definition.flags, VarFlags::ConsoleReadOnly)) {
        flags |= static_cast<uint32>(ConsoleSettingFlags::ConsoleReadOnly);
    }

    return flags;
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

    // 기본값 텍스트는 컴파일 때 검사했으므로 거부는 Name pool 쪽 사정(pool 없음 등)이다.
    if (!checked.Value().IsEmpty()) {
        return Result<void>::Failure(ErrorCode::InvalidArgument);
    }

    return detail::CopyFieldValue(parsed, value);
}

// 정의 하나의 콘솔 설정 상태를 만들고 대기 값을 넘긴다.
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
    setting.flags = ConsoleFlagsOf(definition);
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

    if (filled) {
        filled = detail::AdoptPendingValues(state, setting);
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

    // 1단계: 모두 검사한다(하나라도 거부하면 아무것도 등록하지 않는다). added는 새로 등록할 정의다.
    Array<const VarDefinition*> added(*allocator_);
    for (const VarDefinition* definition : definitions) {
        if constexpr (IsReleaseBuild) {
            if (cvar::HasFlag(definition->flags, VarFlags::Cheat)) {
                continue;
            }
        } else {
            // 핸들은 정적 저장 기간이어야 한다(번호·주소가 정체성이고 문맥보다 오래 살아야 한다).
            if (platform::CurrentThreadStackRange().Contains(definition)) {
                return Result<void>::Failure(ErrorCode::InvalidArgument);
            }
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

    // 2단계: 상태를 모두 만든 뒤 한꺼번에 넣는다(넣는 자리를 먼저 확보한다).
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

    for (UniquePtr<ConsoleSettingState>& setting : made) {
        AssignIndex(*setting->definition);
        if (auto pushed = state->settings.TryPushBack(Move(setting)); !pushed) {
            return pushed;
        }
    }

    // snapshot·콜백 판정이 새 설정을 보도록 다음 Sync가 다시 합친다(즉시 게시는 CVR2 — 5절).
    state->dirty = true;
    return Result<void>::Success();
}

} // namespace rocks::config
