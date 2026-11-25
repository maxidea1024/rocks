#include <Core/Math/MathUtility.h>
#include <InputActions/InputValue.h>

// 액션 값이다(Docs/Design/InputActions 2.1절 — UE InputActionValue.h).
namespace rocks::input {

InputValue::InputValue(InputValueType valueType, const Vec3d& raw) noexcept
    : value(raw), type(valueType) {
    switch (valueType) {
    case InputValueType::Boolean:
    case InputValueType::Axis1D:
        value.y = 0.0;
        value.z = 0.0;
        break;
    case InputValueType::Axis2D:
        value.z = 0.0;
        break;
    case InputValueType::Axis3D:
        break;
    }
}

InputValue InputValue::FromBool(bool down) noexcept {
    return InputValue(InputValueType::Boolean, Vec3d(down ? 1.0 : 0.0, 0.0, 0.0));
}

bool InputValue::IsNonZero(double tolerance) const noexcept {
    return value.LengthSquared() >= tolerance * tolerance;
}

double InputValue::Magnitude() const noexcept {
    switch (type) {
    case InputValueType::Boolean:
    case InputValueType::Axis1D:
        return value.x;
    case InputValueType::Axis2D:
        return math::Sqrt(value.x * value.x + value.y * value.y);
    case InputValueType::Axis3D:
        return value.Length();
    }

    return 0.0;
}

double InputValue::MagnitudeSquared() const noexcept {
    switch (type) {
    case InputValueType::Boolean:
    case InputValueType::Axis1D:
        return value.x * value.x;
    case InputValueType::Axis2D:
        return value.x * value.x + value.y * value.y;
    case InputValueType::Axis3D:
        return value.LengthSquared();
    }

    return 0.0;
}

InputValue& InputValue::operator+=(const InputValue& other) noexcept {
    value += other.value;
    if (static_cast<uint8>(other.type) > static_cast<uint8>(type)) {
        type = other.type;
    }

    return *this;
}

} // namespace rocks::input
