// Physics 모듈 수명 객체와 설정·타입 등록(Docs/Design/PhysicsModule 7.1.1절).
#include <Config/ConfigContext.h>
#include <Core/Math/MathUtility.h>
#include <Core/Module/ModuleEntry.h>
#include <Physics/Physics.module.generated.h>
#include <Physics/PhysicsSettings.generated.h>
#include <Physics/PhysicsSettings.h>

namespace rocks {

namespace {

// Physics 모듈의 수명 객체다(Docs/Design/ModuleLoader 3.2절). Jolt 공유 자원은 장면이 쥐므로
// (PMD21) 시작할 일은 없고, 물리 설정은 HostSettings가 설정 파일을 읽기 전에 모든 모듈의
// RegisterSettings를 부를 때 등록한다.
class PhysicsModule final : public DefaultModuleImplementation {
public:
    static Result<void> RegisterSettings(config::ConfigContext& context) noexcept {
        return context.RegisterSettings<PhysicsSettings>();
    }
};

} // namespace

Result<void> PhysicsSettings::Validate(const PhysicsSettings& settings) noexcept {
    if (!math::IsFinite(settings.defaultGravityZ) || settings.maxBodies == 0 ||
        settings.maxBodyPairs == 0 || settings.maxContactConstraints == 0 ||
        settings.tempAllocatorBytes == 0 ||
        !math::IsFinite(settings.minDeltaVelocityForHitEvents) ||
        settings.minDeltaVelocityForHitEvents < 0.0f) {
        return Result<void>::Failure(ErrorCode::InvalidArgument);
    }

    auto positive = [](float value) noexcept { return math::IsFinite(value) && value > 0.0f; };
    auto nonNegative = [](float value) noexcept { return math::IsFinite(value) && value >= 0.0f; };
    auto knownMode = [](CombineMode mode) noexcept {
        return static_cast<uint8>(mode) <= static_cast<uint8>(CombineMode::Max);
    };
    if (!positive(settings.maxAngularVelocity) || !positive(settings.maxLinearVelocity) ||
        !nonNegative(settings.restitutionThreshold) ||
        !nonNegative(settings.collisionMarginFraction) ||
        !nonNegative(settings.collisionMarginMax) || !knownMode(settings.frictionCombineMode) ||
        !knownMode(settings.restitutionCombineMode) ||
        static_cast<uint8>(settings.defaultDegreesOfFreedom) >
            static_cast<uint8>(SettingsDof::XYPlane)) {
        return Result<void>::Failure(ErrorCode::InvalidArgument);
    }

    return Result<void>::Success();
}

Result<void> RegisterPhysicsTypes(ModuleRegistration& registration) noexcept {
    return generated::Physics::RegisterModuleTypes(registration);
}

} // namespace rocks

ROCKS_IMPLEMENT_MODULE(rocks::PhysicsModule, Physics)
