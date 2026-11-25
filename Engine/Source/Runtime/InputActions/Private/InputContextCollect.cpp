#include <Core/Math/MathUtility.h>
#include <InputActions/InputAction.generated.h>
#include <InputActions/InputAction.h>
#include <InputActions/InputActionData.generated.h>
#include <InputActions/InputActionSettings.generated.h>
#include <InputActions/InputBinding.generated.h>
#include <InputActions/InputContext.generated.h>
#include <InputActions/InputContext.h>
#include <InputActions/InputContextCollect.h>
#include <InputActions/InputContextData.generated.h>
#include <InputActions/InputModifier.generated.h>
#include <InputActions/InputProfileBindings.generated.h>
#include <InputActions/InputTrigger.generated.h>
#include <InputActions/PlayerMappingSettings.generated.h>
#include <Object/ObjectRegistry.h>
#include <Object/ValueCopy.h>

// 컨텍스트 모으기·데이터 검증이다(Docs/Design/InputActions 2.7절).
namespace rocks::input {

namespace {

// 검증 위치를 쌓아 가며 첫 거부를 기록한다.
struct DataCheck {
    InputDataDiagnostic location;
    InputDataDiagnostic* out = nullptr;

    Result<void> Reject(InputDataIssue issue) const noexcept {
        if (out != nullptr) {
            *out = location;
            out->issue = issue;
        }

        return Result<void>::Failure(ErrorCode::InvalidArgument);
    }
};

bool DataFinite(float value) noexcept {
    return math::IsFinite(value);
}

bool DataFinite(const Vec3d& value) noexcept {
    return math::IsFinite(value.x) && math::IsFinite(value.y) && math::IsFinite(value.z);
}

Result<void> CheckTrigger(const InputTrigger& trigger, DataCheck& check) noexcept {
    if (static_cast<uint32>(trigger.type) >= InputTriggerTypeCount) {
        return check.Reject(InputDataIssue::InvalidEnum);
    }

    const float values[] = {trigger.actuationThreshold, trigger.holdTime,
                            trigger.holdAndReleaseTime, trigger.tapReleaseTime,
                            trigger.repeatDelay,        trigger.pulseInterval};
    for (const float value : values) {
        if (!DataFinite(value)) {
            return check.Reject(InputDataIssue::NonFiniteParameter);
        }
    }

    const bool chord = trigger.type == InputTriggerType::ChordAction ||
                       trigger.type == InputTriggerType::ChordBlocker;
    if (chord && trigger.chordActionId == Guid{}) {
        return check.Reject(InputDataIssue::ChordTargetMissing);
    }

    return Result<void>::Success();
}

Result<void> CheckModifier(const InputModifier& modifier, DataCheck& check) noexcept {
    if (static_cast<uint32>(modifier.kind) >= InputModifierKindCount ||
        static_cast<uint32>(modifier.deadZoneType) >= DeadZoneTypeCount ||
        static_cast<uint32>(modifier.swizzle) >= SwizzleOrderCount ||
        static_cast<uint32>(modifier.smoothMethod) >= SmoothDeltaMethodCount) {
        return check.Reject(InputDataIssue::InvalidEnum);
    }

    const float values[] = {modifier.lowerThreshold, modifier.upperThreshold, modifier.fovScale,
                            modifier.smoothSpeed, modifier.easingExponent};
    for (const float value : values) {
        if (!DataFinite(value)) {
            return check.Reject(InputDataIssue::NonFiniteParameter);
        }
    }

    if (!DataFinite(modifier.scalar) || !DataFinite(modifier.curveExponent)) {
        return check.Reject(InputDataIssue::NonFiniteParameter);
    }

    if (modifier.kind == InputModifierKind::DeadZone &&
        !(modifier.upperThreshold > modifier.lowerThreshold)) {
        return check.Reject(InputDataIssue::DeadZoneRange);
    }

    return Result<void>::Success();
}

Result<void> CheckElements(Span<const InputTrigger> triggers, Span<const InputModifier> modifiers,
                           DataCheck& check) noexcept {
    for (usize index = 0; index < triggers.Size(); ++index) {
        check.location.elementKind = InputDataElement::Trigger;
        check.location.element = static_cast<uint32>(index);
        if (auto checked = CheckTrigger(triggers[index], check); !checked) {
            return checked;
        }
    }

    for (usize index = 0; index < modifiers.Size(); ++index) {
        check.location.elementKind = InputDataElement::Modifier;
        check.location.element = static_cast<uint32>(index);
        if (auto checked = CheckModifier(modifiers[index], check); !checked) {
            return checked;
        }
    }

    check.location.elementKind = InputDataElement::None;
    check.location.element = NoInputDataIndex;
    return Result<void>::Success();
}

Result<void> CheckSettings(const InputActionSettings& settings, DataCheck& check) noexcept {
    if (static_cast<uint32>(settings.valueType) >= InputValueTypeCount ||
        static_cast<uint32>(settings.accumulation) >= ActionAccumulationCount) {
        return check.Reject(InputDataIssue::InvalidEnum);
    }

    if (settings.mappingSettings.HasValue() && settings.mappingSettings.Value().name.IsEmpty()) {
        return check.Reject(InputDataIssue::MappingNameMissing);
    }

    if (auto checked = CheckElements(settings.triggers.View(), settings.modifiers.View(), check);
        !checked) {
        return checked;
    }

    // Boolean 액션의 액션 수준 Scalar는 효과가 없다(UE는 바깥이 액션일 때만 거부한다).
    if (settings.valueType == InputValueType::Boolean) {
        for (usize index = 0; index < settings.modifiers.Size(); ++index) {
            if (settings.modifiers[index].kind == InputModifierKind::Scalar) {
                check.location.elementKind = InputDataElement::Modifier;
                check.location.element = static_cast<uint32>(index);
                return check.Reject(InputDataIssue::ScalarOnBooleanAction);
            }
        }
    }

    return Result<void>::Success();
}

bool HasAction(const InputContextBundle& bundle, const Guid& id) noexcept {
    for (const InputActionData& action : bundle.actions) {
        if (action.id == id) {
            return true;
        }
    }

    return false;
}

Result<void> CheckBindings(const InputContextBundle& bundle, Span<const InputBinding> bindings,
                           DataCheck& check) noexcept {
    for (usize index = 0; index < bindings.Size(); ++index) {
        const InputBinding& binding = bindings[index];
        check.location.binding = static_cast<uint32>(index);
        if (!HasAction(bundle, binding.action)) {
            return check.Reject(InputDataIssue::UnknownAction);
        }

        if (static_cast<uint32>(binding.settingBehavior) >= MappingSettingBehaviorCount) {
            return check.Reject(InputDataIssue::InvalidEnum);
        }

        if (binding.mappingSettings.HasValue() && binding.mappingSettings.Value().name.IsEmpty()) {
            return check.Reject(InputDataIssue::MappingNameMissing);
        }

        if (auto checked = CheckElements(binding.triggers.View(), binding.modifiers.View(), check);
            !checked) {
            return checked;
        }
    }

    check.location.binding = NoInputDataIndex;
    return Result<void>::Success();
}

} // namespace

Result<void> ValidateInputElements(Span<const InputTrigger> triggers,
                                   Span<const InputModifier> modifiers,
                                   InputDataDiagnostic* diagnostic) noexcept {
    DataCheck check;
    check.out = diagnostic;
    if (auto checked = CheckElements(triggers, modifiers, check); !checked) {
        return checked;
    }

    if (diagnostic != nullptr) {
        *diagnostic = InputDataDiagnostic{};
    }

    return Result<void>::Success();
}

Result<void> ValidateInputActionSettings(const InputActionSettings& settings,
                                         InputDataDiagnostic* diagnostic) noexcept {
    DataCheck check;
    check.out = diagnostic;
    if (auto checked = CheckSettings(settings, check); !checked) {
        return checked;
    }

    if (diagnostic != nullptr) {
        *diagnostic = InputDataDiagnostic{};
    }

    return Result<void>::Success();
}

Result<void> ValidateInputContextBundle(const InputContextBundle& bundle,
                                        InputDataDiagnostic* diagnostic) noexcept {
    DataCheck check;
    check.out = diagnostic;
    if (bundle.id == Guid{}) {
        return check.Reject(InputDataIssue::ContextIdMissing);
    }

    if (static_cast<uint32>(bundle.data.registrationTracking) >= ContextRegistrationTrackingCount) {
        return check.Reject(InputDataIssue::InvalidEnum);
    }

    for (usize index = 0; index < bundle.actions.Size(); ++index) {
        const InputActionData& action = bundle.actions[index];
        check.location.action = static_cast<uint32>(index);
        if (action.id == Guid{}) {
            return check.Reject(InputDataIssue::ActionIdMissing);
        }

        for (usize earlier = 0; earlier < index; ++earlier) {
            if (bundle.actions[earlier].id == action.id) {
                return check.Reject(InputDataIssue::DuplicateAction);
            }
        }

        if (auto checked = CheckSettings(action.settings, check); !checked) {
            return checked;
        }
    }

    check.location.action = NoInputDataIndex;
    if (auto checked = CheckBindings(bundle, bundle.data.bindings.View(), check); !checked) {
        return checked;
    }

    for (usize index = 0; index < bundle.data.profileOverrides.Size(); ++index) {
        check.location.profile = static_cast<uint32>(index);
        const String& profile = bundle.data.profileOverrides[index].profile;
        if (profile.IsEmpty()) {
            return check.Reject(InputDataIssue::ProfileNameMissing);
        }

        for (usize earlier = 0; earlier < index; ++earlier) {
            if (bundle.data.profileOverrides[earlier].profile.View() == profile.View()) {
                return check.Reject(InputDataIssue::DuplicateProfile);
            }
        }

        if (auto checked =
                CheckBindings(bundle, bundle.data.profileOverrides[index].bindings.View(), check);
            !checked) {
            return checked;
        }
    }

    if (diagnostic != nullptr) {
        *diagnostic = InputDataDiagnostic{};
    }

    return Result<void>::Success();
}

Result<void> CollectInputContext(ObjectRegistry& registry, const ObjectAccessContext& access,
                                 ObjectHandle context, InputContextBundle& bundle) noexcept {
    auto borrowed = registry.Resolve<InputContext>(access, context);
    if (!borrowed) {
        return Result<void>::Failure(borrowed.Error());
    }

    auto pointer = borrowed.Value().Get();
    if (!pointer) {
        return Result<void>::Failure(pointer.Error());
    }

    const InputContext& source = *pointer.Value();
    Allocator& allocator = bundle.actions.GetAllocator();
    // 사본을 따로 다 만든 뒤 옮긴다(실패면 bundle은 그대로다).
    InputContextBundle staged(allocator);
    staged.id = source.id;
    if (auto copied = detail::TryCopyValue(staged.data, source.data, allocator); !copied) {
        return copied;
    }

    if (auto reserved = staged.actions.TryReserve(source.actions.Size()); !reserved) {
        return reserved;
    }

    const Span<const ObjectHandle> handles = source.actions.View();
    for (usize index = 0; index < handles.Size(); ++index) {
        const ObjectHandle handle = handles[index];
        if (handle.IsNull()) {
            return Result<void>::Failure(ErrorCode::InvalidArgument);
        }

        // 같은 객체가 여러 번 있으면 처음 것만 복사한다(ObjectRefArray는 중복을 담을 수 있다).
        bool seen = false;
        for (usize earlier = 0; earlier < index && !seen; ++earlier) {
            seen = handles[earlier] == handle;
        }

        if (seen) {
            continue;
        }

        auto action = registry.Resolve<InputAction>(access, handle);
        if (!action) {
            return Result<void>::Failure(action.Error());
        }

        auto actionPointer = action.Value().Get();
        if (!actionPointer) {
            return Result<void>::Failure(actionPointer.Error());
        }

        InputActionData data(allocator);
        data.id = actionPointer.Value()->id;
        if (auto copied =
                detail::TryCopyValue(data.settings, actionPointer.Value()->settings, allocator);
            !copied) {
            return copied;
        }

        if (auto pushed = staged.actions.TryPushBack(Move(data)); !pushed) {
            return pushed;
        }
    }

    bundle = Move(staged);
    return Result<void>::Success();
}

} // namespace rocks::input
