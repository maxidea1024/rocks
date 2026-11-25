#include <Base/InitializerList.h>
#include <Core/Memory/SystemAllocator.h>
#include <InputActions/InputAction.generated.h>
#include <InputActions/InputAction.h>
#include <InputActions/InputActionsModule.h>
#include <InputActions/InputContext.generated.h>
#include <InputActions/InputContext.h>
#include <InputActions/InputContextCollect.h>
#include <Object/ModuleRegistration.h>
#include <Object/ObjectRegistry.h>
#include <Object/TypeRegistry.h>
#include <TestSupport/NFailAllocator.h>
#include <TestSupport/Test.h>
#include <math.h>
#include <stdio.h>

// 입력 액션 데이터 시험이다(Docs/Design/InputActions 2.3–2.7절 — IA2a): 기본값 = UE, 검증 규칙마다
// 양성·음성과 진단 위치, 컨텍스트 객체 모으기(깊은 복사·실패 시 bundle 보존).
using namespace rocks;
using namespace rocks::input;
namespace input_data_tests_file {

namespace {

constexpr uint64 TestIdHigh = 0x49414441;
constexpr uint64 JumpId = 1;
constexpr uint64 MoveId = 2;
constexpr uint64 ContextId = 100;

Guid Id(uint64 low) noexcept {
    return Guid{TestIdHigh, low};
}

bool AddBinding(Array<InputBinding>& bindings, Allocator& allocator, uint64 action,
                const char* key) noexcept {
    InputBinding binding(allocator);
    binding.action = Id(action);
    if (!binding.key.TryAssign(key)) {
        return false;
    }

    return bindings.TryPushBack(Move(binding)).IsOk();
}

bool AddAction(InputContextBundle& bundle, Allocator& allocator, uint64 id,
               InputValueType type) noexcept {
    InputActionData action(allocator);
    action.id = Id(id);
    action.settings.valueType = type;
    return bundle.actions.TryPushBack(Move(action)).IsOk();
}

// 올바른 bundle: Jump(Boolean, Pressed 트리거)·Move(2D, DeadZone)와 바인딩 셋, 프로필 하나.
bool MakeBundle(InputContextBundle& bundle, Allocator& allocator) noexcept {
    bundle.id = Id(ContextId);
    if (!AddAction(bundle, allocator, JumpId, InputValueType::Boolean) ||
        !AddAction(bundle, allocator, MoveId, InputValueType::Axis2D)) {
        return false;
    }

    InputTrigger pressed;
    pressed.type = InputTriggerType::Pressed;
    InputModifier dead(allocator);
    if (!bundle.actions[0].settings.triggers.TryPushBack(pressed) ||
        !bundle.actions[1].settings.modifiers.TryPushBack(Move(dead))) {
        return false;
    }

    if (!AddBinding(bundle.data.bindings, allocator, JumpId, "SpaceBar") ||
        !AddBinding(bundle.data.bindings, allocator, MoveId, "Gamepad_Left2D") ||
        !AddBinding(bundle.data.bindings, allocator, JumpId, "Gamepad_FaceButton_Bottom")) {
        return false;
    }

    InputTrigger hold;
    hold.type = InputTriggerType::Hold;
    InputModifier negate(allocator);
    negate.kind = InputModifierKind::Negate;
    if (!bundle.data.bindings[2].triggers.TryPushBack(hold) ||
        !bundle.data.bindings[1].modifiers.TryPushBack(Move(negate))) {
        return false;
    }

    InputProfileBindings southpaw(allocator);
    if (!southpaw.profile.TryAssign("Southpaw") ||
        !AddBinding(southpaw.bindings, allocator, MoveId, "Gamepad_Right2D")) {
        return false;
    }

    return bundle.data.profileOverrides.TryPushBack(Move(southpaw)).IsOk();
}

// bundle을 바꾸는 함수를 받아 검증 결과가 기대 진단인지 본다.
struct Expected {
    InputDataIssue issue;
    uint32 action = NoInputDataIndex;
    uint32 profile = NoInputDataIndex;
    uint32 binding = NoInputDataIndex;
    InputDataElement elementKind = InputDataElement::None;
    uint32 element = NoInputDataIndex;
};

template <class Mutate>
bool RejectsWith(Mutate mutate, const Expected& expected) noexcept {
    SystemAllocator allocator;
    InputContextBundle bundle(allocator);
    if (!MakeBundle(bundle, allocator)) {
        return false;
    }

    mutate(bundle, allocator);
    InputDataDiagnostic diagnostic;
    auto validated = ValidateInputContextBundle(bundle, &diagnostic);
    const bool same =
        !validated && validated.Error() == ErrorCode::InvalidArgument &&
        diagnostic.issue == expected.issue && diagnostic.action == expected.action &&
        diagnostic.profile == expected.profile && diagnostic.binding == expected.binding &&
        diagnostic.elementKind == expected.elementKind && diagnostic.element == expected.element;
    if (!same) {
        printf("  issue %u action %u profile %u binding %u kind %u element %u\n",
               static_cast<unsigned>(diagnostic.issue), diagnostic.action, diagnostic.profile,
               diagnostic.binding, static_cast<unsigned>(diagnostic.elementKind),
               diagnostic.element);
    }

    return same;
}

template <class Mutate>
bool Accepts(Mutate mutate) noexcept {
    SystemAllocator allocator;
    InputContextBundle bundle(allocator);
    if (!MakeBundle(bundle, allocator)) {
        return false;
    }

    mutate(bundle, allocator);
    InputDataDiagnostic diagnostic;
    diagnostic.issue = InputDataIssue::DuplicateAction;
    return ValidateInputContextBundle(bundle, &diagnostic).IsOk() &&
           diagnostic.issue == InputDataIssue::None;
}

const float NotANumber = static_cast<float>(nan(""));
const float Infinity = static_cast<float>(HUGE_VAL);

// 트리거의 float 매개변수 수·모디파이어의 비유한 검사 칸 수·모디파이어 enum 칸 수다.
constexpr uint32 TriggerFloatFields = 6;
constexpr uint32 ModifierFiniteFields = 7;
constexpr uint32 ModifierEnumFields = 4;
// 할당 실패 반복의 상한(모으기 한 번의 할당 수보다 넉넉하다)이다.
constexpr uint64 MaxCollectAttempts = 256;

} // namespace

// 2.3·2.4절: 필드 기본값은 UE와 같다(InputAction.h:92-153, EnhancedActionKeyMapping.h,
// InputMappingContext.h, InputTriggers.h).
TEST(InputData_DefaultsMatchUnreal) {
    SystemAllocator allocator;
    const InputActionSettings settings(allocator);
    CHECK(settings.valueType == InputValueType::Boolean && !settings.triggerWhenPaused);
    CHECK(settings.consumeInput && !settings.reserveAllMappings);
    CHECK(settings.accumulation == ActionAccumulation::TakeHighestAbsoluteValue);
    CHECK(settings.triggers.IsEmpty() && settings.modifiers.IsEmpty() &&
          !settings.mappingSettings.HasValue());
    const InputBinding binding(allocator);
    CHECK(binding.settingBehavior == MappingSettingBehavior::InheritFromAction);
    CHECK(binding.action == Guid{} && binding.key.IsEmpty());
    const InputContextData data(allocator);
    CHECK(data.registrationTracking == ContextRegistrationTracking::Untracked);
    const InputTrigger trigger;
    CHECK(trigger.holdTime == 1.0f && trigger.holdAndReleaseTime == 0.5f);
    CHECK(trigger.tapReleaseTime == 0.2f && trigger.repeatDelay == 0.5f && trigger.tapCount == 2);
    CHECK(trigger.pulseInterval == 1.0f && trigger.triggerOnStart && trigger.triggerLimit == 0);
    CHECK(trigger.actuationThreshold == 0.5f && trigger.chordAction == NoChordAction);
    CHECK(!trigger.affectedByTimeDilation && !trigger.oneShot && trigger.chordActionId == Guid{});
    const InputModifier modifier(allocator);
    CHECK(modifier.lowerThreshold == 0.2f && modifier.upperThreshold == 1.0f);
    CHECK(modifier.deadZoneType == DeadZoneType::Radial && modifier.swizzle == SwizzleOrder::YXZ);
    CHECK(modifier.smoothSpeed == 0.5f && modifier.easingExponent == 2.0f);
    CHECK(modifier.negateX && modifier.negateY && modifier.negateZ && modifier.fovScale == 1.0f);
    CHECK(modifier.scalar == Vec3d(1, 1, 1) && modifier.curveExponent == Vec3d(1, 1, 1));
    CHECK(modifier.smoothMethod == SmoothDeltaMethod::Lerp &&
          modifier.kind == InputModifierKind::DeadZone);
}

// 2.7절: 올바른 bundle은 통과한다. 허용하는 것: chord 대상이 목록에 없음, DeadZone이 아닌
// 모디파이어의 lower ≥ upper, 액션 없는 빈 bundle, 빈 프로필.
TEST(InputData_ValidateAccepts) {
    CHECK(Accepts([](InputContextBundle&, Allocator&) noexcept {}));
    CHECK(Accepts([](InputContextBundle& bundle, Allocator&) noexcept {
        InputTrigger chord;
        chord.type = InputTriggerType::ChordAction;
        chord.chordActionId = Id(77);
        (void)bundle.data.bindings[0].triggers.TryPushBack(chord);
    }));
    CHECK(Accepts([](InputContextBundle& bundle, Allocator&) noexcept {
        bundle.data.bindings[1].modifiers[0].lowerThreshold = 2.0f;
    }));
    CHECK(Accepts([](InputContextBundle& bundle, Allocator&) noexcept {
        bundle.data.bindings.Clear();
        bundle.data.profileOverrides.Clear();
        bundle.actions.Clear();
    }));
    CHECK(Accepts([](InputContextBundle& bundle, Allocator&) noexcept {
        bundle.data.profileOverrides[0].bindings.Clear();
    }));
    // 키 이름은 검증하지 않는다(빈 키·모르는 이름은 평가기가 다시 만들 때 건너뛴다 — UE와 같다).
    CHECK(Accepts([](InputContextBundle& bundle, Allocator&) noexcept {
        (void)bundle.data.bindings[0].key.TryAssign("spacebar");
        bundle.data.bindings[1].key.Clear();
        (void)bundle.data.bindings[2].key.TryAssign("Gamepad_LeftTrigger");
    }));
    // chord 대상이 목록에 있음, 대상 있는 ChordBlocker(UE처럼 막지 않는다).
    CHECK(Accepts([](InputContextBundle& bundle, Allocator&) noexcept {
        InputTrigger chord;
        chord.type = InputTriggerType::ChordAction;
        chord.chordActionId = Id(JumpId);
        InputTrigger blocker;
        blocker.type = InputTriggerType::ChordBlocker;
        blocker.chordActionId = Id(MoveId);
        (void)bundle.data.bindings[1].triggers.TryPushBack(chord);
        (void)bundle.data.bindings[1].triggers.TryPushBack(blocker);
    }));
    // 이름 있는 매핑 설정, Boolean이 아닌 액션의 Scalar, 바인딩 수준 Scalar.
    CHECK(Accepts([](InputContextBundle& bundle, Allocator& allocator) noexcept {
        PlayerMappingSettings jump(allocator);
        (void)jump.name.TryAssign("Jump");
        (void)bundle.actions[0].settings.mappingSettings.Emplace(Move(jump));
        InputModifier scalar(allocator);
        scalar.kind = InputModifierKind::Scalar;
        (void)bundle.actions[1].settings.modifiers.TryPushBack(Move(scalar));
        InputModifier bindingScalar(allocator);
        bindingScalar.kind = InputModifierKind::Scalar;
        (void)bundle.data.bindings[0].modifiers.TryPushBack(Move(bindingScalar));
    }));
}

// 2.7절: 규칙마다 하나씩 어기면 정해진 진단과 위치로 InvalidArgument다.
TEST(InputData_ValidateRejectsEachRule) {
    using M = InputContextBundle&;
    CHECK(RejectsWith([](M bundle, Allocator&) noexcept { bundle.id = Guid{}; },
                      {InputDataIssue::ContextIdMissing}));
    CHECK(RejectsWith(
        [](M bundle, Allocator&) noexcept {
            bundle.data.registrationTracking = static_cast<ContextRegistrationTracking>(2);
        },
        {InputDataIssue::InvalidEnum}));
    CHECK(RejectsWith([](M bundle, Allocator&) noexcept { bundle.actions[1].id = Guid{}; },
                      {InputDataIssue::ActionIdMissing, 1}));
    CHECK(RejectsWith([](M bundle, Allocator&) noexcept { bundle.actions[1].id = Id(JumpId); },
                      {InputDataIssue::DuplicateAction, 1}));
    CHECK(RejectsWith(
        [](M bundle, Allocator&) noexcept {
            bundle.actions[0].settings.valueType = static_cast<InputValueType>(4);
        },
        {InputDataIssue::InvalidEnum, 0}));
    CHECK(RejectsWith(
        [](M bundle, Allocator&) noexcept {
            bundle.actions[1].settings.accumulation = static_cast<ActionAccumulation>(2);
        },
        {InputDataIssue::InvalidEnum, 1}));
    // 액션 설정 안의 트리거·모디파이어.
    CHECK(RejectsWith(
        [](M bundle, Allocator&) noexcept {
            bundle.actions[0].settings.triggers[0].type = static_cast<InputTriggerType>(10);
        },
        {InputDataIssue::InvalidEnum, 0, NoInputDataIndex, NoInputDataIndex,
         InputDataElement::Trigger, 0}));
    CHECK(RejectsWith(
        [](M bundle, Allocator&) noexcept {
            bundle.actions[1].settings.modifiers[0].upperThreshold = 0.1f;
        },
        {InputDataIssue::DeadZoneRange, 1, NoInputDataIndex, NoInputDataIndex,
         InputDataElement::Modifier, 0}));
    CHECK(RejectsWith(
        [](M bundle, Allocator&) noexcept {
            bundle.actions[1].settings.modifiers[0].upperThreshold =
                bundle.actions[1].settings.modifiers[0].lowerThreshold;
        },
        {InputDataIssue::DeadZoneRange, 1, NoInputDataIndex, NoInputDataIndex,
         InputDataElement::Modifier, 0}));
    // 바인딩.
    CHECK(RejectsWith([](M bundle, Allocator&) noexcept { bundle.data.bindings[2].action = Id(9); },
                      {InputDataIssue::UnknownAction, NoInputDataIndex, NoInputDataIndex, 2}));
    // 매핑 설정 이름이 빔(액션·바인딩 — UE IsDataValid).
    CHECK(RejectsWith(
        [](M bundle, Allocator& allocator) noexcept {
            (void)bundle.actions[1].settings.mappingSettings.Emplace(
                PlayerMappingSettings(allocator));
        },
        {InputDataIssue::MappingNameMissing, 1}));
    CHECK(RejectsWith(
        [](M bundle, Allocator& allocator) noexcept {
            (void)bundle.data.bindings[2].mappingSettings.Emplace(PlayerMappingSettings(allocator));
        },
        {InputDataIssue::MappingNameMissing, NoInputDataIndex, NoInputDataIndex, 2}));
    // Boolean 액션의 액션 수준 Scalar(UE InputModifiers.cpp:82-95).
    CHECK(RejectsWith(
        [](M bundle, Allocator& allocator) noexcept {
            InputModifier negate(allocator);
            negate.kind = InputModifierKind::Negate;
            InputModifier scalar(allocator);
            scalar.kind = InputModifierKind::Scalar;
            (void)bundle.actions[0].settings.modifiers.TryPushBack(Move(negate));
            (void)bundle.actions[0].settings.modifiers.TryPushBack(Move(scalar));
        },
        {InputDataIssue::ScalarOnBooleanAction, 0, NoInputDataIndex, NoInputDataIndex,
         InputDataElement::Modifier, 1}));
    CHECK(RejectsWith(
        [](M bundle, Allocator&) noexcept {
            bundle.data.bindings[0].settingBehavior = static_cast<MappingSettingBehavior>(3);
        },
        {InputDataIssue::InvalidEnum, NoInputDataIndex, NoInputDataIndex, 0}));
    // 바인딩 안의 트리거·모디파이어 — 각 비유한 매개변수.
    for (uint32 field = 0; field < TriggerFloatFields * 2; ++field) {
        CHECK(RejectsWith(
            [field](M bundle, Allocator&) noexcept {
                InputTrigger& trigger = bundle.data.bindings[2].triggers[0];
                float* const fields[] = {&trigger.actuationThreshold, &trigger.holdTime,
                                         &trigger.holdAndReleaseTime, &trigger.tapReleaseTime,
                                         &trigger.repeatDelay,        &trigger.pulseInterval};
                // 앞 절반은 NaN, 뒤 절반은 무한이다.
                *fields[field % TriggerFloatFields] =
                    field < TriggerFloatFields ? NotANumber : Infinity;
            },
            {InputDataIssue::NonFiniteParameter, NoInputDataIndex, NoInputDataIndex, 2,
             InputDataElement::Trigger, 0}));
    }

    for (uint32 field = 0; field < ModifierFiniteFields; ++field) {
        CHECK(RejectsWith(
            [field](M bundle, Allocator&) noexcept {
                InputModifier& modifier = bundle.data.bindings[1].modifiers[0];
                switch (field) {
                case 0:
                    modifier.lowerThreshold = NotANumber;
                    break;
                case 1:
                    modifier.upperThreshold = NotANumber;
                    break;
                case 2:
                    modifier.fovScale = NotANumber;
                    break;
                case 3:
                    modifier.smoothSpeed = NotANumber;
                    break;
                case 4:
                    modifier.easingExponent = NotANumber;
                    break;
                case 5:
                    modifier.scalar.y = static_cast<double>(NotANumber);
                    break;
                default:
                    modifier.curveExponent.z = 1.0 / 0.0;
                    break;
                }
            },
            {InputDataIssue::NonFiniteParameter, NoInputDataIndex, NoInputDataIndex, 1,
             InputDataElement::Modifier, 0}));
    }

    // 모디파이어 enum 넷.
    for (uint32 field = 0; field < ModifierEnumFields; ++field) {
        CHECK(RejectsWith(
            [field](M bundle, Allocator&) noexcept {
                InputModifier& modifier = bundle.data.bindings[1].modifiers[0];
                if (field == 0) {
                    modifier.kind = static_cast<InputModifierKind>(InputModifierKindCount);
                } else if (field == 1) {
                    modifier.deadZoneType = static_cast<DeadZoneType>(DeadZoneTypeCount);
                } else if (field == 2) {
                    modifier.swizzle = static_cast<SwizzleOrder>(SwizzleOrderCount);
                } else {
                    modifier.smoothMethod = static_cast<SmoothDeltaMethod>(SmoothDeltaMethodCount);
                }
            },
            {InputDataIssue::InvalidEnum, NoInputDataIndex, NoInputDataIndex, 1,
             InputDataElement::Modifier, 0}));
    }

    // chord 트리거의 대상 id 0(ChordAction·ChordBlocker 둘 다).
    for (const InputTriggerType type :
         {InputTriggerType::ChordAction, InputTriggerType::ChordBlocker}) {
        CHECK(RejectsWith(
            [type](M bundle, Allocator&) noexcept {
                InputTrigger chord;
                chord.type = type;
                (void)bundle.data.bindings[0].triggers.TryPushBack(chord);
            },
            {InputDataIssue::ChordTargetMissing, NoInputDataIndex, NoInputDataIndex, 0,
             InputDataElement::Trigger, 0}));
    }

    // 프로필 override의 바인딩도 같은 규칙이고 위치에 프로필 번호가 붙는다.
    CHECK(RejectsWith(
        [](M bundle, Allocator&) noexcept {
            bundle.data.profileOverrides[0].bindings[0].action = Id(42);
        },
        {InputDataIssue::UnknownAction, NoInputDataIndex, 0, 0}));
    // 프로필 이름이 빔·겹침.
    CHECK(RejectsWith(
        [](M bundle, Allocator&) noexcept { bundle.data.profileOverrides[0].profile.Clear(); },
        {InputDataIssue::ProfileNameMissing, NoInputDataIndex, 0}));
    CHECK(RejectsWith(
        [](M bundle, Allocator& allocator) noexcept {
            InputProfileBindings again(allocator);
            (void)again.profile.TryAssign("Southpaw");
            (void)bundle.data.profileOverrides.TryPushBack(Move(again));
        },
        {InputDataIssue::DuplicateProfile, NoInputDataIndex, 1}));

    // 액션 설정만 보는 검증(주입 액션).
    SystemAllocator allocator;
    InputActionSettings settings(allocator);
    InputDataDiagnostic diagnostic;
    CHECK(ValidateInputActionSettings(settings, &diagnostic) &&
          diagnostic.issue == InputDataIssue::None);
    InputTrigger broken;
    broken.holdTime = NotANumber;
    REQUIRE(settings.triggers.TryPushBack(broken));
    CHECK(ValidateInputActionSettings(settings, &diagnostic).Error() == ErrorCode::InvalidArgument);
    CHECK(diagnostic.issue == InputDataIssue::NonFiniteParameter &&
          diagnostic.elementKind == InputDataElement::Trigger && diagnostic.element == 0);
    CHECK(diagnostic.action == NoInputDataIndex && diagnostic.binding == NoInputDataIndex);
    CHECK(!ValidateInputActionSettings(settings));
}

// 2.7절: 컨텍스트 객체와 actions를 모은다 — 깊은 복사(원본을 바꿔도 사본 그대로), 액션 공유, 접근
// 실패, 할당 실패 때 bundle 보존.
TEST(InputData_CollectContext) {
    SystemAllocator allocator;
    TypeRegistry types(allocator);
    {
        ModuleRegistration registration(types, allocator);
        REQUIRE(RegisterInputActionsTypes(registration));
        {
            ObjectRegistry objects(allocator, types, {TestIdHigh, 1});
            const auto access = objects.OwnerAccess();
            {
                auto jump = objects.NewObject<InputAction>(access, TypeIdOf<InputAction>());
                auto move = objects.NewObject<InputAction>(access, TypeIdOf<InputAction>());
                auto context = objects.NewObject<InputContext>(access, TypeIdOf<InputContext>());
                auto other = objects.NewObject<InputContext>(access, TypeIdOf<InputContext>());
                REQUIRE(jump && move && context && other);
                const ObjectHandle jumpHandle = jump.Value().Handle();
                const ObjectHandle moveHandle = move.Value().Handle();
                const ObjectHandle contextHandle = context.Value().Handle();
                {
                    auto writable = objects.ResolveForWrite<InputAction>(access, jumpHandle);
                    REQUIRE(writable);
                    auto action = writable.Value().Get();
                    REQUIRE(action);
                    action.Value()->id = Id(JumpId);
                    InputTrigger pressed;
                    pressed.type = InputTriggerType::Pressed;
                    REQUIRE(action.Value()->settings.triggers.TryPushBack(pressed));
                }

                {
                    auto writable = objects.ResolveForWrite<InputAction>(access, moveHandle);
                    REQUIRE(writable);
                    auto action = writable.Value().Get();
                    REQUIRE(action);
                    action.Value()->id = Id(MoveId);
                    action.Value()->settings.valueType = InputValueType::Axis2D;
                    InputModifier curve(allocator);
                    curve.kind = InputModifierKind::ResponseCurveUser;
                    REQUIRE(curve.responseX.TryAddKey(0.0f, 0.0f));
                    REQUIRE(curve.responseX.TryAddKey(1.0f, 2.0f));
                    REQUIRE(action.Value()->settings.modifiers.TryPushBack(Move(curve)));
                }

                for (const ObjectHandle owner : {contextHandle, other.Value().Handle()}) {
                    auto writable = objects.ResolveForWrite<InputContext>(access, owner);
                    REQUIRE(writable);
                    auto pointer = writable.Value().Get();
                    REQUIRE(pointer);
                    pointer.Value()->id = Id(owner == contextHandle ? ContextId : ContextId + 1);
                    REQUIRE(
                        AddBinding(pointer.Value()->data.bindings, allocator, JumpId, "SpaceBar"));
                    REQUIRE(
                        AddBinding(pointer.Value()->data.bindings, allocator, MoveId, "Mouse2D"));
                    REQUIRE(
                        objects.AddReference(access, owner, pointer.Value()->actions, jumpHandle));
                    REQUIRE(
                        objects.AddReference(access, owner, pointer.Value()->actions, moveHandle));
                    // 같은 액션 객체를 한 번 더 넣는다(바인딩마다 AddReference하는 편집 코드).
                    REQUIRE(
                        objects.AddReference(access, owner, pointer.Value()->actions, jumpHandle));
                }

                InputContextBundle bundle(allocator);
                REQUIRE(CollectInputContext(objects, access, contextHandle, bundle));
                // 같은 객체 둘은 한 번만 복사한다(검증 DuplicateAction은 다른 객체가 같은 id일
                // 때뿐).
                CHECK(bundle.id == Id(ContextId) && bundle.actions.Size() == 2);
                CHECK(bundle.actions[0].id == Id(JumpId) &&
                      bundle.actions[0].settings.triggers.Size() == 1);
                CHECK(bundle.actions[1].settings.valueType == InputValueType::Axis2D);
                CHECK(bundle.actions[1].settings.modifiers[0].responseX.KeyCount() == 2);
                CHECK(bundle.data.bindings.Size() == 2 &&
                      bundle.data.bindings[1].key.View() == StringView("Mouse2D"));
                CHECK(ValidateInputContextBundle(bundle));

                // 깊은 복사: 원본을 바꿔도 사본은 그대로다.
                {
                    auto writable = objects.ResolveForWrite<InputAction>(access, moveHandle);
                    REQUIRE(writable);
                    auto action = writable.Value().Get();
                    REQUIRE(action);
                    action.Value()->settings.modifiers[0].responseX.Reset();
                    action.Value()->settings.valueType = InputValueType::Axis3D;
                }

                CHECK(bundle.actions[1].settings.modifiers[0].responseX.KeyCount() == 2);
                CHECK(bundle.actions[1].settings.valueType == InputValueType::Axis2D);
                // 다시 모으면 바뀐 값이다(UpdateContext 흐름 — 2.7절).
                InputContextBundle again(allocator);
                REQUIRE(CollectInputContext(objects, access, contextHandle, again));
                CHECK(again.actions[1].settings.valueType == InputValueType::Axis3D);
                // 다른 컨텍스트도 같은 액션 객체를 모은다(액션 공유).
                InputContextBundle shared(allocator);
                REQUIRE(CollectInputContext(objects, access, other.Value().Handle(), shared));
                CHECK(shared.id == Id(ContextId + 1) && shared.actions[0].id == Id(JumpId));

                // 할당 실패: 시도마다 실패시켜 보고, 실패면 OutOfMemory이고 bundle은 그대로다.
                // bundle은 이미 모은 다른 컨텍스트로 채워 두어 "그대로"가 내용까지인지 본다.
                test::NFailAllocator failing(allocator);
                bool succeeded = false;
                uint32 failures = 0;
                for (uint64 attempt = 1; attempt < MaxCollectAttempts && !succeeded; ++attempt) {
                    InputContextBundle target(failing);
                    REQUIRE(CollectInputContext(objects, access, other.Value().Handle(), target));
                    failing.FailOnAttempt(attempt);
                    auto collected = CollectInputContext(objects, access, contextHandle, target);
                    failing.DisableFailure();
                    if (collected) {
                        succeeded = target.id == Id(ContextId) && target.actions.Size() == 2;
                    } else {
                        ++failures;
                        CHECK(collected.Error() == ErrorCode::OutOfMemory);
                        CHECK(target.id == Id(ContextId + 1) && target.actions.Size() == 2);
                        CHECK(target.data.bindings.Size() == 2 &&
                              target.data.bindings[1].key.View() == StringView("Mouse2D"));
                    }
                }

                CHECK(succeeded && failures > 0);

                // 컨텍스트가 아닌 객체·파괴된 액션은 접근 오류를 그대로 돌려주고 bundle은 그대로다.
                InputContextBundle untouched(allocator);
                untouched.id = Id(555);
                auto wrongType = CollectInputContext(objects, access, jumpHandle, untouched);
                CHECK(!wrongType && wrongType.Error() ==
                                        objects.Resolve<InputContext>(access, jumpHandle).Error());
                auto nullHandle = CollectInputContext(objects, access, ObjectHandle{}, untouched);
                CHECK(!nullHandle &&
                      nullHandle.Error() ==
                          objects.Resolve<InputContext>(access, ObjectHandle{}).Error());
                // null actions 원소(Required는 저장 때 검사한다 — 실행 중에는 담길 수 있다).
                auto holder = objects.NewObject<InputContext>(access, TypeIdOf<InputContext>());
                REQUIRE(holder);
                {
                    auto borrowed = objects.Resolve<InputContext>(access, holder.Value().Handle());
                    REQUIRE(borrowed);
                    auto pointer = borrowed.Value().Get();
                    REQUIRE(pointer);
                    REQUIRE(objects.AddReference(access, holder.Value().Handle(),
                                                 pointer.Value()->actions, ObjectHandle{}));
                }

                CHECK(CollectInputContext(objects, access, holder.Value().Handle(), untouched)
                          .Error() == ErrorCode::InvalidArgument);
                REQUIRE(objects.RequestDestroy(access, moveHandle));
                auto destroyed = CollectInputContext(objects, access, contextHandle, untouched);
                CHECK(!destroyed && destroyed.Error() ==
                                        objects.Resolve<InputAction>(access, moveHandle).Error());
                CHECK(untouched.id == Id(555) && untouched.actions.IsEmpty());
            }

            REQUIRE(objects.Shutdown(access));
        }

        REQUIRE(registration.Unregister());
    }

    CHECK(types.Shutdown());
}

} // namespace input_data_tests_file
