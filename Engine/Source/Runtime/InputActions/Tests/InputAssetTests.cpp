#include "InputAssetScene.h"

#include <Base/InitializerList.h>
#include <InputActions/Haptics/HapticPlayer.h>
#include <InputActions/InputAssetAccess.h>
#include <InputActions/InputContextCollect.h>
#include <Platform/Thread.h>
#include <TestSupport/NFailAllocator.h>

// 입력 에셋 타입 시험이다(Docs/Design/InputActions 2.8절 — IA6a): Asset 기반과 필드, 쿠킹 패키지
// 로드와 lease로 모으기(Hard 참조 액션 함께 로드·하위 객체 액션·source에서 모은 bundle과 같음),
// 실효 id(0이면 AssetId), 도우미 오류(TypeMismatch·무효·회수 lease·다른 스레드·할당 실패마다 — 출력
// 그대로), 진동 효과 사본과 lease 재생.
using namespace rocks;
using namespace rocks::input;
using namespace rocks::input_asset_test;
namespace input_asset_tests_file {

namespace {

constexpr uint64 JumpPackage = 1;
constexpr uint64 MovePackage = 2;
constexpr uint64 ContextPackage = 3;
constexpr uint64 HapticPackage = 4;
constexpr uint64 JumpLow = 10;
constexpr uint64 MoveLow = 11;
constexpr uint64 FireLow = 12;
constexpr uint64 ContextLow = 20;
constexpr uint64 EffectLow = 30;
constexpr uint64 UntouchedLow = 99;
constexpr float EffectSeconds = 0.5f;
constexpr float HoldSeconds = 0.4f;
constexpr float DeadZoneLower = 0.2f;
constexpr float DeadZoneUpper = 0.9f;
constexpr float CurveMiddle = 0.25f;
constexpr double TickSeconds = 0.1;
constexpr uint32 MaxAllocationAttempts = 512;

bool AddTrigger(Array<InputTrigger>& triggers, InputTriggerType type) noexcept {
    InputTrigger trigger;
    trigger.type = type;
    trigger.holdTime = HoldSeconds;
    return triggers.TryPushBack(trigger).IsOk();
}

bool AddModifier(Array<InputModifier>& modifiers, Allocator& allocator,
                 InputModifierKind kind) noexcept {
    InputModifier modifier(allocator);
    modifier.kind = kind;
    modifier.lowerThreshold = DeadZoneLower;
    modifier.upperThreshold = DeadZoneUpper;
    modifier.negateX = false;
    if (kind == InputModifierKind::ResponseCurveUser) {
        if (!modifier.responseX.TryAddKey(0.0f, 0.0f) ||
            !modifier.responseX.TryAddKey(0.5f, CurveMiddle) ||
            !modifier.responseX.TryAddKey(1.0f, 1.0f)) {
            return false;
        }
    }

    return modifiers.TryPushBack(Move(modifier)).IsOk();
}

bool SetMapping(Optional<PlayerMappingSettings>& out, Allocator& allocator,
                const char* name) noexcept {
    PlayerMappingSettings mapping(allocator);
    String profile(allocator);
    if (!mapping.name.TryAssign(name) || !profile.TryAssign("Default") ||
        !mapping.supportedProfiles.TryPushBack(Move(profile))) {
        return false;
    }

    (void)out.Emplace(Move(mapping));
    return true;
}

// 액션 둘(다른 패키지 — 트리거·모디파이어·매핑 설정을 채운다)과 컨텍스트 패키지 안 하위 액션,
// 그것을 쓰는 컨텍스트(바인딩 트리거·모디파이어·매핑 설정·프로필 override·등록 횟수 세기), 진동
// 효과 에셋을 만든다. jumpId·contextId·effectId가 0이면 실효 id는 AssetId다.
bool BuildScene(InputAssetScene& scene, Allocator& allocator, const Guid& jumpId,
                const Guid& contextId, const Guid& effectId,
                const Guid& fireId = Id(FireLow)) noexcept {
    ObjectRegistry& source = scene.Source();
    const ObjectHandle jump = scene.AddAction(JumpPackage, jumpId, InputValueType::Boolean);
    const ObjectHandle move = scene.AddAction(MovePackage, Id(MoveLow), InputValueType::Axis2D);
    const ObjectHandle context = scene.AddContext(ContextPackage, contextId);
    const ObjectHandle fire =
        scene.AddInnerAction(ContextPackage, "Fire", fireId, InputValueType::Boolean);
    if (jump.IsNull() || move.IsNull() || context.IsNull() || fire.IsNull()) {
        return false;
    }

    InputAction* jumpValue = asset_test::Pointer<InputAction>(source, jump);
    InputAction* moveValue = asset_test::Pointer<InputAction>(source, move);
    InputContext* contextValue = asset_test::Pointer<InputContext>(source, context);
    if (jumpValue == nullptr || moveValue == nullptr || contextValue == nullptr) {
        return false;
    }

    jumpValue->settings.consumeInput = false;
    jumpValue->settings.triggerWhenPaused = true;
    moveValue->settings.accumulation = ActionAccumulation::Cumulative;
    if (!AddTrigger(jumpValue->settings.triggers, InputTriggerType::Hold) ||
        !SetMapping(jumpValue->settings.mappingSettings, allocator, "Jump") ||
        !AddModifier(moveValue->settings.modifiers, allocator, InputModifierKind::DeadZone) ||
        !AddModifier(moveValue->settings.modifiers, allocator,
                     InputModifierKind::ResponseCurveUser)) {
        return false;
    }

    // 바인딩은 액션의 실효 id를 적는다(jumpId가 0이면 액션 패키지 AssetId).
    const Guid jumpTarget = jumpId != Guid{} ? jumpId : asset_test::FixedId(JumpPackage).value;
    if (!scene.LinkAction(context, jump) || !scene.LinkAction(context, move) ||
        !scene.LinkAction(context, fire) || !scene.AddBinding(context, jumpTarget, "SpaceBar") ||
        !scene.AddBinding(context, Id(MoveLow), "Gamepad_Left2D") ||
        !scene.AddBinding(context, fireId, "LeftMouseButton")) {
        return false;
    }

    InputContextData& data = contextValue->data;
    data.registrationTracking = ContextRegistrationTracking::CountRegistrations;
    if (!AddTrigger(data.bindings[0].triggers, InputTriggerType::Pressed) ||
        !AddModifier(data.bindings[1].modifiers, allocator, InputModifierKind::Negate) ||
        !SetMapping(data.bindings[1].mappingSettings, allocator, "MoveStick")) {
        return false;
    }

    InputProfileBindings southpaw(allocator);
    InputBinding right(allocator);
    right.action = Id(MoveLow);
    if (!southpaw.profile.TryAssign("Southpaw") || !right.key.TryAssign("Gamepad_Right2D") ||
        !southpaw.bindings.TryPushBack(Move(right)) ||
        !data.profileOverrides.TryPushBack(Move(southpaw))) {
        return false;
    }

    return !scene.AddHaptic(HapticPackage, effectId, EffectSeconds).IsNull();
}

bool SameTriggers(Span<const InputTrigger> left, Span<const InputTrigger> right) noexcept {
    if (left.Size() != right.Size()) {
        return false;
    }

    for (usize index = 0; index < left.Size(); ++index) {
        if (left[index].type != right[index].type ||
            left[index].holdTime != right[index].holdTime ||
            left[index].actuationThreshold != right[index].actuationThreshold) {
            return false;
        }
    }

    return true;
}

bool SameModifiers(Span<const InputModifier> left, Span<const InputModifier> right) noexcept {
    if (left.Size() != right.Size()) {
        return false;
    }

    for (usize index = 0; index < left.Size(); ++index) {
        const InputModifier& a = left[index];
        const InputModifier& b = right[index];
        if (a.kind != b.kind || a.lowerThreshold != b.lowerThreshold ||
            a.upperThreshold != b.upperThreshold || a.negateX != b.negateX ||
            a.responseX.KeyCount() != b.responseX.KeyCount() ||
            a.responseX.Evaluate(0.5f) != b.responseX.Evaluate(0.5f)) {
            return false;
        }
    }

    return true;
}

bool SameMapping(const Optional<PlayerMappingSettings>& left,
                 const Optional<PlayerMappingSettings>& right) noexcept {
    if (left.HasValue() != right.HasValue()) {
        return false;
    }

    if (!left.HasValue()) {
        return true;
    }

    if (left->name.View() != right->name.View() ||
        left->supportedProfiles.Size() != right->supportedProfiles.Size()) {
        return false;
    }

    for (usize index = 0; index < left->supportedProfiles.Size(); ++index) {
        if (left->supportedProfiles[index].View() != right->supportedProfiles[index].View()) {
            return false;
        }
    }

    return true;
}

bool SameBindings(Span<const InputBinding> left, Span<const InputBinding> right) noexcept {
    if (left.Size() != right.Size()) {
        return false;
    }

    for (usize index = 0; index < left.Size(); ++index) {
        const InputBinding& a = left[index];
        const InputBinding& b = right[index];
        if (a.action != b.action || a.key.View() != b.key.View() ||
            !SameTriggers(a.triggers.View(), b.triggers.View()) ||
            !SameModifiers(a.modifiers.View(), b.modifiers.View()) ||
            !SameMapping(a.mappingSettings, b.mappingSettings)) {
            return false;
        }
    }

    return true;
}

const InputActionData* FindAction(const InputContextBundle& bundle, const Guid& id) noexcept {
    for (const InputActionData& action : bundle.actions) {
        if (action.id == id) {
            return &action;
        }
    }

    return nullptr;
}

// 두 bundle이 채운 필드(컨텍스트·액션·바인딩·프로필 override·등록 방식)에서 같은지다. 액션은 id로
// 짝짓는다.
bool SameBundle(const InputContextBundle& left, const InputContextBundle& right) noexcept {
    if (left.id != right.id || left.actions.Size() != right.actions.Size() ||
        left.data.registrationTracking != right.data.registrationTracking ||
        !SameBindings(left.data.bindings.View(), right.data.bindings.View()) ||
        left.data.profileOverrides.Size() != right.data.profileOverrides.Size()) {
        return false;
    }

    for (usize index = 0; index < left.data.profileOverrides.Size(); ++index) {
        const InputProfileBindings& a = left.data.profileOverrides[index];
        const InputProfileBindings& b = right.data.profileOverrides[index];
        if (a.profile.View() != b.profile.View() ||
            !SameBindings(a.bindings.View(), b.bindings.View())) {
            return false;
        }
    }

    for (const InputActionData& action : left.actions) {
        const InputActionData* other = FindAction(right, action.id);
        if (other == nullptr) {
            return false;
        }

        const InputActionSettings& a = action.settings;
        const InputActionSettings& b = other->settings;
        if (a.valueType != b.valueType || a.consumeInput != b.consumeInput ||
            a.triggerWhenPaused != b.triggerWhenPaused || a.accumulation != b.accumulation ||
            !SameTriggers(a.triggers.View(), b.triggers.View()) ||
            !SameModifiers(a.modifiers.View(), b.modifiers.View()) ||
            !SameMapping(a.mappingSettings, b.mappingSettings)) {
            return false;
        }
    }

    return true;
}

// type 스키마의 필드(기반 필드가 앞에 온다) 중 name이 있으면 그 flags를, 없으면 Max를 돌려준다.
// count에 필드 수를 쓴다.
uint32 SchemaFieldFlags(TypeRegistry& types, TypeId type, StringView name, usize& count) noexcept {
    auto lease = types.Acquire(type);
    if (!lease) {
        return NumericLimits<uint32>::Max();
    }

    const TypeSchema& schema = lease.Value().Schema();
    count = schema.fields.Size();
    for (const FieldSchema& field : schema.fields) {
        if (StringView(field.name.Data(), field.name.Size()) == name) {
            return field.flags;
        }
    }

    return NumericLimits<uint32>::Max();
}

struct OtherThreadCollect {
    InputAssetScene* scene = nullptr;
    const AssetLease* lease = nullptr;
    InputContextBundle* bundle = nullptr;
    ErrorCode error = ErrorCode::None;
};

void CollectOnOtherThread(void* context) noexcept {
    auto* call = static_cast<OtherThreadCollect*>(context);
    auto collected = CollectInputContext(*call->scene->manager, *call->lease, *call->bundle);
    call->error = collected ? ErrorCode::None : collected.Error();
}

} // namespace

// 2.8절: 입력 에셋 클래스는 Asset 파생이다. 필드는 기반 Asset의 assetId_(Transient) 하나에 자기
// 필드(InputAction id·settings, InputContext id·actions·data, HapticEffectAsset effect)를 더한
// 것이다. 코드로 만든 객체는 assetId가 null이라 실효 id가 id 그대로다.
TEST(InputAssets_ReflectionShape) {
    asset_test::AssetEnv env;
    InputAssetScene scene(env);
    REQUIRE(scene.Ready());
    const TypeId asset = GeneratedType<Asset>::Id();
    for (const TypeId type :
         {TypeIdOf<InputAction>(), TypeIdOf<InputContext>(), TypeIdOf<HapticEffectAsset>()}) {
        auto derived = env.types.IsA(type, asset);
        CHECK(derived && derived.Value());
    }

    constexpr uint32 Missing = NumericLimits<uint32>::Max();
    usize count = 0;
    CHECK(SchemaFieldFlags(env.types, TypeIdOf<HapticEffectAsset>(), "effect", count) != Missing);
    CHECK(count == 2);
    CHECK(SchemaFieldFlags(env.types, TypeIdOf<InputAction>(), "id", count) != Missing);
    CHECK(SchemaFieldFlags(env.types, TypeIdOf<InputAction>(), "settings", count) != Missing);
    CHECK(count == 3);
    CHECK(SchemaFieldFlags(env.types, TypeIdOf<InputContext>(), "actions", count) != Missing);
    CHECK(SchemaFieldFlags(env.types, TypeIdOf<InputContext>(), "data", count) != Missing);
    CHECK(count == 4);
    const uint32 assetIdFlags = SchemaFieldFlags(env.types, asset, "assetId_", count);
    REQUIRE(assetIdFlags != Missing);
    CHECK(count == 1);
    CHECK((assetIdFlags & static_cast<uint32>(FieldFlags::Transient)) != 0);

    InputAction action(env.allocator);
    action.id = Id(JumpLow);
    CHECK(action.Id() == AssetId{});
    CHECK(action.EffectiveId() == Id(JumpLow));
    action.id = Guid{};
    CHECK(action.EffectiveId() == Guid{});
}

// 쿠킹 패키지: 컨텍스트를 로드하면 Hard 참조 액션 패키지도 함께 로드되고, lease로 모은 bundle은
// source Registry에서 모은 bundle과 같다(트리거·모디파이어 커브·매핑 설정·프로필 override·등록
// 방식·하위 객체 액션까지). 로드한 대표 에셋은 assetId를 받는다.
TEST(InputAssets_CookedContextMatchesSource) {
    asset_test::AssetEnv env;
    InputAssetScene scene(env);
    REQUIRE(scene.Ready());
    REQUIRE(BuildScene(scene, env.allocator, Id(JumpLow), Id(ContextLow), Id(EffectLow)));
    REQUIRE(scene.Start());
    ObjectRegistry& source = scene.Source();
    {
        auto lease = scene.Load(ContextPackage);
        REQUIRE(lease);
        CHECK(scene.IsLoaded(JumpPackage) && scene.IsLoaded(MovePackage));
        InputContextBundle bundle(env.allocator);
        REQUIRE(CollectInputContext(*scene.manager, lease.Value(), bundle));
        CHECK(ValidateInputContextBundle(bundle));
        CHECK(bundle.id == Id(ContextLow));
        REQUIRE(bundle.actions.Size() == 3);
        CHECK(FindAction(bundle, Id(FireLow)) != nullptr);
        REQUIRE(bundle.data.bindings.Size() == 3);
        CHECK(bundle.data.bindings[0].key.View() == "SpaceBar");
        CHECK(bundle.data.registrationTracking == ContextRegistrationTracking::CountRegistrations);

        // source에서 모은 bundle(쿠킹 전 같은 객체)과 같다.
        const ObjectHandle sourceContext = scene.SourceAsset(ContextPackage);
        REQUIRE(!sourceContext.IsNull());
        InputContextBundle expected(env.allocator);
        REQUIRE(CollectInputContext(source, source.OwnerAccess(), sourceContext, expected));
        CHECK(SameBundle(expected, bundle));

        bool identified = false;
        REQUIRE(scene.manager->With(lease.Value(), [&](ObjectHandle object) noexcept {
            ObjectRegistry& registry = scene.manager->Registry();
            InputContext* context = asset_test::Pointer<InputContext>(registry, object);
            identified = context != nullptr && context->Id() == asset_test::FixedId(ContextPackage);
        }));
        CHECK(identified);
    }
}

// 실효 id: id를 0으로 둔 액션·컨텍스트·진동 효과는 패키지 AssetId를 쓴다(바인딩은 액션의 AssetId를
// 적는다). 직접 적은 id는 그대로이고, 모은 bundle은 검증을 통과한다.
TEST(InputAssets_EffectiveIdFallsBackToAssetId) {
    asset_test::AssetEnv env;
    InputAssetScene scene(env);
    REQUIRE(scene.Ready());
    REQUIRE(BuildScene(scene, env.allocator, Guid{}, Guid{}, Guid{}));
    REQUIRE(scene.Start());
    {
        auto context = scene.Load(ContextPackage);
        auto haptic = scene.Load(HapticPackage);
        REQUIRE(context && haptic);
        InputContextBundle bundle(env.allocator);
        REQUIRE(CollectInputContext(*scene.manager, context.Value(), bundle));
        CHECK(bundle.id == asset_test::FixedId(ContextPackage).value);
        CHECK(FindAction(bundle, asset_test::FixedId(JumpPackage).value) != nullptr);
        CHECK(FindAction(bundle, Id(MoveLow)) != nullptr);
        CHECK(ValidateInputContextBundle(bundle));

        HapticEffect effect(env.allocator);
        REQUIRE(CopyHapticEffect(*scene.manager, haptic.Value(), effect));
        CHECK(effect.id == asset_test::FixedId(HapticPackage).value);
    }
}

// 하위 객체 액션은 대표 에셋이 아니라 AssetId가 없다. id를 0으로 두면 실효 id도 0이라 검증이
// ActionIdMissing으로 거부한다(2.8절 — 하위 액션은 id를 직접 적는다).
TEST(InputAssets_InnerActionNeedsExplicitId) {
    asset_test::AssetEnv env;
    InputAssetScene scene(env);
    REQUIRE(scene.Ready());
    REQUIRE(BuildScene(scene, env.allocator, Id(JumpLow), Id(ContextLow), Id(EffectLow), Guid{}));
    REQUIRE(scene.Start());
    {
        auto context = scene.Load(ContextPackage);
        REQUIRE(context);
        InputContextBundle bundle(env.allocator);
        REQUIRE(CollectInputContext(*scene.manager, context.Value(), bundle));
        CHECK(FindAction(bundle, Guid{}) != nullptr);
        InputDataDiagnostic diagnostic;
        CHECK(ValidateInputContextBundle(bundle, &diagnostic).Error() ==
              ErrorCode::InvalidArgument);
        CHECK(diagnostic.issue == InputDataIssue::ActionIdMissing);
    }
}

// 도우미 오류: 대표 에셋 타입이 다르면 TypeMismatch, 빈·회수 lease는 StaleHandle, 다른 스레드는
// WrongThread이고 모두 출력이 그대로다.
TEST(InputAssets_HelperErrorsKeepOutput) {
    asset_test::AssetEnv env;
    InputAssetScene scene(env);
    REQUIRE(scene.Ready());
    REQUIRE(BuildScene(scene, env.allocator, Id(JumpLow), Id(ContextLow), Id(EffectLow)));
    REQUIRE(scene.Start());
    {
        auto action = scene.Load(JumpPackage);
        auto context = scene.Load(ContextPackage);
        REQUIRE(action && context);

        InputContextBundle bundle(env.allocator);
        bundle.id = Id(UntouchedLow);
        CHECK(CollectInputContext(*scene.manager, action.Value(), bundle).Error() ==
              ErrorCode::TypeMismatch);
        CHECK(bundle.id == Id(UntouchedLow) && bundle.actions.IsEmpty());

        HapticEffect effect(env.allocator);
        effect.id = Id(UntouchedLow);
        CHECK(CopyHapticEffect(*scene.manager, context.Value(), effect).Error() ==
              ErrorCode::TypeMismatch);
        CHECK(effect.id == Id(UntouchedLow) && effect.channels.IsEmpty());

        const AssetLease empty;
        CHECK(CollectInputContext(*scene.manager, empty, bundle).Error() == ErrorCode::StaleHandle);
        CHECK(CopyHapticEffect(*scene.manager, empty, effect).Error() == ErrorCode::StaleHandle);
        CHECK(bundle.id == Id(UntouchedLow) && effect.id == Id(UntouchedLow));

        // 회수된 lease(scope Revoke).
        auto scope = scene.manager->CreateScope();
        REQUIRE(scope);
        AssetRequest request;
        request.id = asset_test::FixedId(HapticPackage);
        request.scope = &scope.Value();
        auto scoped = scene.manager->LoadBlocking(request, "InputAssetTests.Scoped");
        REQUIRE(scoped);
        REQUIRE(scope.Value().Revoke());
        CHECK(!scoped.Value().IsValid());
        CHECK(CopyHapticEffect(*scene.manager, scoped.Value(), effect).Error() ==
              ErrorCode::StaleHandle);
        CHECK(effect.id == Id(UntouchedLow));

        // 다른 스레드(만들고 Join — 결정적).
        OtherThreadCollect call{&scene, &context.Value(), &bundle};
        platform::Thread thread;
        REQUIRE(thread.Start(CollectOnOtherThread, &call) == platform::ThreadStatus::Success);
        REQUIRE(thread.Join() == platform::ThreadStatus::Success);
        CHECK(call.error == ErrorCode::WrongThread);
        CHECK(bundle.id == Id(UntouchedLow) && bundle.actions.IsEmpty());
    }
}

// 할당 실패: 출력 allocator의 N번째 할당을 실패시키며 N을 1부터 늘린다. 실패마다 OutOfMemory이고
// 출력은 그대로이며, 실패가 끝나면 성공한다.
TEST(InputAssets_AllocationFailuresKeepOutput) {
    asset_test::AssetEnv env;
    InputAssetScene scene(env);
    REQUIRE(scene.Ready());
    REQUIRE(BuildScene(scene, env.allocator, Id(JumpLow), Id(ContextLow), Id(EffectLow)));
    REQUIRE(scene.Start());
    {
        auto context = scene.Load(ContextPackage);
        auto haptic = scene.Load(HapticPackage);
        REQUIRE(context && haptic);
        bool collected = false;
        uint32 collectFailures = 0;
        for (uint32 attempt = 1; attempt <= MaxAllocationAttempts && !collected; ++attempt) {
            test::NFailAllocator failing(env.allocator);
            failing.FailOnAttempt(attempt);
            InputContextBundle bundle(failing);
            bundle.id = Id(UntouchedLow);
            auto result = CollectInputContext(*scene.manager, context.Value(), bundle);
            if (result) {
                collected = true;
                CHECK(bundle.id == Id(ContextLow));
            } else {
                ++collectFailures;
                CHECK(result.Error() == ErrorCode::OutOfMemory);
                CHECK(bundle.id == Id(UntouchedLow) && bundle.actions.IsEmpty() &&
                      bundle.data.bindings.IsEmpty());
            }
        }

        CHECK(collected && collectFailures > 0);

        bool copied = false;
        uint32 copyFailures = 0;
        for (uint32 attempt = 1; attempt <= MaxAllocationAttempts && !copied; ++attempt) {
            test::NFailAllocator failing(env.allocator);
            failing.FailOnAttempt(attempt);
            HapticEffect effect(failing);
            effect.id = Id(UntouchedLow);
            auto result = CopyHapticEffect(*scene.manager, haptic.Value(), effect);
            if (result) {
                copied = true;
                CHECK(effect.id == Id(EffectLow) && effect.channels.Size() == 1);
            } else {
                ++copyFailures;
                CHECK(result.Error() == ErrorCode::OutOfMemory);
                CHECK(effect.id == Id(UntouchedLow) && effect.channels.IsEmpty());
            }
        }

        CHECK(copied && copyFailures > 0);
    }
}

// 진동 효과: 사본은 원본과 같다. lease 재생의 실패(빈 lease StaleHandle·다른 타입 TypeMismatch)는
// 재생 목록을 바꾸지 않는다. 성공한 재생은 lease를 놓고 패키지가 Discard된 뒤에도 사본으로 돈다.
TEST(InputAssets_HapticCopyAndPlay) {
    asset_test::AssetEnv env;
    InputAssetScene scene(env);
    REQUIRE(scene.Ready());
    REQUIRE(BuildScene(scene, env.allocator, Id(JumpLow), Id(ContextLow), Id(EffectLow)));
    REQUIRE(scene.Start());
    HapticPlayer player(env.allocator);
    {
        auto lease = scene.Load(HapticPackage);
        auto context = scene.Load(ContextPackage);
        REQUIRE(lease && context);
        HapticEffect effect(env.allocator);
        REQUIRE(CopyHapticEffect(*scene.manager, lease.Value(), effect));
        CHECK(effect.id == Id(EffectLow));
        REQUIRE(effect.channels.Size() == 1);
        CHECK(effect.Duration() == EffectSeconds);

        CHECK(player.PlayHapticEffect(*scene.manager, AssetLease(), HapticParams{}).Error() ==
              ErrorCode::StaleHandle);
        CHECK(player.PlayHapticEffect(*scene.manager, context.Value(), HapticParams{}).Error() ==
              ErrorCode::TypeMismatch);
        CHECK(player.ActiveEffectCount() == 0);

        REQUIRE(player.PlayHapticEffect(*scene.manager, lease.Value(), HapticParams{}));
        CHECK(player.ActiveEffectCount() == 1);
        CHECK(scene.Leases(HapticPackage) == 1);
    }

    // lease를 놓고 Pump하면 패키지가 Discard된다 — 재생은 사본으로 계속 돈다.
    CHECK(scene.LeasesAfterPump(HapticPackage) == 0);
    CHECK(!scene.IsLoaded(HapticPackage));
    HapticUpdateContext tick;
    tick.deltaSeconds = TickSeconds;
    tick.unscaledDeltaSeconds = TickSeconds;
    const MotorValues motors = player.Update(tick);
    CHECK(motors.leftLarge > 0.0f && motors.rightSmall > 0.0f);
}

} // namespace input_asset_tests_file
