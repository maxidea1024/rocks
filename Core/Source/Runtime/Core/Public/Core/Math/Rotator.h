#pragma once

#include <Core/Math/Quaternion.h>

namespace rocks {

// 사람이 편집하기 쉬운 오일러 각 회전이다. UE 5.8 FRotator(TRotator<T>)와 단위·필드 순서·
// 회전 의미·변환 식이 같다(근거: UE Core/Public/Math/Rotator.h, Core/Private/Math/UnrealMath.cpp).
// - 단위는 도(degree)다(UE Rotator.h:24).
// - 필드와 생성자 순서는 pitch, yaw, roll이다(UE Rotator.h:45-52, :101).
// - 좌표계는 왼손 +X 전방·+Y 오른쪽·+Z 위다(MathUtility.h).
// - 각 성분의 양의 방향(UE Rotator.h:26-32·:45-52, UE 식을 옮긴 probe로 확인):
//   - pitch: 오른쪽 축(+Y) 회전. 양수는 기수가 위로 든다.
//     Rotator(90, 0, 0): Forward → Up, Up → Backward.
//   - yaw: 위 축(+Z) 회전. 양수는 위에서 내려다볼 때 시계 방향(전방이 오른쪽으로 돈다).
//     Rotator(0, 90, 0): Forward → Right, Right → Backward.
//   - roll: 전방 축(+X) 회전. 양수는 +X 방향으로 볼 때 시계 방향(오른쪽이 아래로 간다).
//     Rotator(0, 0, 90): Right → Down, Up → Right.
// - 적용 순서는 내재 yaw → pitch → roll이다. 물체를 위 축으로 yaw, 새 오른쪽 축으로 pitch,
//   마지막 전방 축으로 roll 한다. quaternion으로는 Q(+Z, yaw) * Q(+Y, -pitch) * Q(+X, -roll)
//   이다(축-각은 Quaternion.h의 왼손 양의 각 규약, 각은 라디안으로 바꾼 값). UE 주석의 말로는
//   yaw는 왼손, pitch·roll은 오른손 회전이다(Rotator.h:31-32).
// - 같은 회전에 대응하는 각도 조합은 여러 개다. 회전 동일성은 ToQuaternion 뒤
//   AngularDistance로, 각도 값 비교는 Equals(차를 NormalizeAxis로 감아 비교)로 한다.
//   operator==는 성분의 정확 비교다.
// - Rocks 고유 계약(UE와 다른 점): quaternion에서 만들 때 입력을 검증·정규화하고 실패를
//   Result로 돌려준다(UE FQuat::Rotator는 단위 quaternion을 가정하고 검사하지 않는다).
//   ClampAxis는 반올림 경계에서도 선언 구간 [0, 360)을 지킨다. 비유한 각은 비유한 결과가
//   되므로 외부 입력은 IsFinite로 먼저 검사한다(UE는 진단 빌드에서 NaN을 로그로 알리고 0
//   회전으로 바꾼다).
//
// 사용 예:
//   const Rotator<double> aim(10.0, 90.0, 0.0); // pitch 10°, yaw 90°(오른쪽)
//   const Vec3d facing = aim.Vector();          // ≈ (0, 0.985, 0.174)
//   const Quatd rotation = aim.ToQuaternion();
//   auto back = Rotator<double>::TryFromQuaternion(rotation); // ≈ (10, 90, 0)
template <typename T>
struct Rotator {
    static_assert(math::IsFloatingPointV<T>, "Rotator supports float and double");

    T pitch = static_cast<T>(0);
    T yaw = static_cast<T>(0);
    T roll = static_cast<T>(0);

    constexpr Rotator() noexcept = default;

    // 각 인자는 도(degree)다. UE TRotator(InPitch, InYaw, InRoll)와 같은 순서다.
    constexpr Rotator(T pitchDegrees, T yawDegrees, T rollDegrees) noexcept
        : pitch(pitchDegrees), yaw(yawDegrees), roll(rollDegrees) {
    }

    friend constexpr bool operator==(const Rotator&, const Rotator&) noexcept = default;

    constexpr bool IsFinite() const noexcept {
        return math::IsFinite(pitch) && math::IsFinite(yaw) && math::IsFinite(roll);
    }

    // 각을 [0, 360)으로 감는다(UE TRotator::ClampAxis, Rotator.h:688-701).
    // UE 본문은 Fmod 뒤 음수에 360을 더하므로 -1e-20 같은 아주 작은 음수에서 반올림으로 360을
    // 낸다. Rocks는 그 경우 0을 돌려 UE가 선언한 구간 [0, 360)(Rotator.h:418-424)을 지킨다.
    // 유한하지 않은 입력은 NaN이다.
    //   Rotator<double>::ClampAxis(-90.0); // 270
    //   Rotator<double>::ClampAxis(725.0); // 5
    static T ClampAxis(T angleDegrees) noexcept {
        constexpr T FullTurn = static_cast<T>(360);
        T angle = math::Fmod(angleDegrees, FullTurn);
        if (angle < static_cast<T>(0)) {
            angle += FullTurn;
        }

        return angle >= FullTurn ? static_cast<T>(0) : angle;
    }

    // 각을 (-180, 180]으로 감는다(UE TRotator::NormalizeAxis, Rotator.h:703-716 — ClampAxis
    // 뒤 180보다 크면 360을 뺀다). math::UnwindDegrees와 같은 값이다. 유한하지 않은 입력은
    // NaN이다.
    //   Rotator<double>::NormalizeAxis(190.0);  // -170
    //   Rotator<double>::NormalizeAxis(-180.0); // 180
    static T NormalizeAxis(T angleDegrees) noexcept {
        T angle = ClampAxis(angleDegrees);
        if (angle > static_cast<T>(180)) {
            angle -= static_cast<T>(360);
        }

        return angle;
    }

    // 각 성분을 (-180, 180]으로 감은 복사본이다(UE GetNormalized). 회전은 바뀌지 않는다.
    Rotator GetNormalized() const noexcept {
        return Rotator(NormalizeAxis(pitch), NormalizeAxis(yaw), NormalizeAxis(roll));
    }

    // 각 성분을 [0, 360)으로 감은 복사본이다(UE GetDenormalized·Clamp). 회전은 바뀌지 않는다.
    Rotator GetDenormalized() const noexcept {
        return Rotator(ClampAxis(pitch), ClampAxis(yaw), ClampAxis(roll));
    }

    // 성분마다 |NormalizeAxis(this - other)| <= toleranceDegrees면 true다(UE TRotator::Equals,
    // Rotator.h:649-651). 360° 감김 차이는 같다고 보지만 다른 성분 조합의 같은 회전
    // (예: (180, 0, 0)과 (0, 180, 180))은 구분한다. 회전 동일성은 ToQuaternion 뒤
    // AngularDistance로 본다. NaN이 있으면 false다.
    bool Equals(const Rotator& other, T toleranceDegrees) const noexcept {
        return math::Abs(NormalizeAxis(pitch - other.pitch)) <= toleranceDegrees &&
               math::Abs(NormalizeAxis(yaw - other.yaw)) <= toleranceDegrees &&
               math::Abs(NormalizeAxis(roll - other.roll)) <= toleranceDegrees;
    }

    // 회전된 전방 방향(단위 벡터)이다. roll은 전방 방향에 영향이 없다.
    // UE TRotator::Vector와 같은 식이다(UnrealMath.cpp:371-402): pitch·yaw를 Fmod 360으로
    // 감은 뒤 (cos p·cos y, cos p·sin y, sin p).
    //   Rotator<double>(0.0, 90.0, 0.0).Vector(); // ≈ Right (0, 1, 0)
    Vector3<T> Vector() const noexcept {
        constexpr T FullTurn = static_cast<T>(360);
        const T pitchNoWinding = math::Fmod(pitch, FullTurn);
        const T yawNoWinding = math::Fmod(yaw, FullTurn);
        T sinePitch;
        T cosinePitch;
        T sineYaw;
        T cosineYaw;
        math::SinCos(math::DegreesToRadians(pitchNoWinding), sinePitch, cosinePitch);
        math::SinCos(math::DegreesToRadians(yawNoWinding), sineYaw, cosineYaw);
        return Vector3<T>(cosinePitch * cosineYaw, cosinePitch * sineYaw, sinePitch);
    }

    // 단위 quaternion으로 변환한다. UE TRotator::Quaternion의 스칼라 식과 같다
    // (UnrealMath.cpp:518-535 double, :448-466 float). 각 성분을 Fmod 360으로 감고 반각의
    // sin·cos로
    //   x =  cr·sp·sy − sr·cp·cy,  y = −cr·sp·cy − sr·cp·sy,
    //   z =  cr·cp·sy − sr·sp·cy,  w =  cr·cp·cy + sr·sp·sy
    // 이며 Q(+Z, yaw) * Q(+Y, -pitch) * Q(+X, -roll)과 반올림 수준에서 같다.
    // sin·cos는 float도 math::SinCos(CRT 값)를 쓴다. UE float은 다항 근사 FMath::SinCos
    // (UnrealMathUtility.h:760) 또는 VectorSinCos라 float 결과는 반올림 수준보다 조금 더 다를 수
    // 있다(double은 UE도 정확한 sin·cos다, :801-806). 비유한 각은 비유한 quaternion이 되므로
    // 먼저 IsFinite로 검사한다.
    //   Rotator<double>(0.0, 90.0, 0.0).ToQuaternion(); // (0, 0, sin 45°, cos 45°)
    Quaternion<T> ToQuaternion() const noexcept {
        constexpr T FullTurn = static_cast<T>(360);
        const T degreesToRadians = math::Pi<T> / static_cast<T>(180);
        const T halfRadiansPerDegree = degreesToRadians / static_cast<T>(2);
        const T pitchNoWinding = math::Fmod(pitch, FullTurn);
        const T yawNoWinding = math::Fmod(yaw, FullTurn);
        const T rollNoWinding = math::Fmod(roll, FullTurn);
        T sp;
        T cp;
        T sy;
        T cy;
        T sr;
        T cr;
        math::SinCos(pitchNoWinding * halfRadiansPerDegree, sp, cp);
        math::SinCos(yawNoWinding * halfRadiansPerDegree, sy, cy);
        math::SinCos(rollNoWinding * halfRadiansPerDegree, sr, cr);
        return Quaternion<T>(cr * sp * sy - sr * cp * cy, -cr * sp * cy - sr * cp * sy,
                             cr * cp * sy - sr * sp * cy, cr * cp * cy + sr * sp * sy);
    }

    // vector를 이 회전으로 돌린다(UE TRotator::RotateVector). UE는 FRotationMatrix를 거치고
    // Rocks는 ToQuaternion().RotateVector를 쓴다. 같은 회전이며 결과는 반올림 수준에서 다를 수
    // 있다.
    Vector3<T> RotateVector(const Vector3<T>& vector) const noexcept {
        return ToQuaternion().RotateVector(vector);
    }

    // 역회전이다(UE TRotator::UnrotateVector). RotateVector와 같은 반올림 주의가 있다.
    Vector3<T> UnrotateVector(const Vector3<T>& vector) const noexcept {
        return ToQuaternion().UnrotateVector(vector);
    }

    // quaternion을 Rotator로 분해한다. 입력을 정규화한 뒤 UE TQuat::Rotator와 같은 식을 T
    // 정밀도로 계산한다(UnrealMath.cpp:640-677 float, :691-719 double).
    //   s = z·x − w·y
    //   s < −0.4999995: pitch = −90, yaw = NormalizeAxis(−2·atan2(x, w)), roll = 0
    //   s > 0.4999995:  pitch = 90,  yaw = NormalizeAxis(2·atan2(x, w)),  roll = 0
    //   그 밖:          pitch = asin(2s), yaw = atan2(2(w·z + x·y), 1 − 2(y² + z²)),
    //                   roll = atan2(−2(w·x + y·z), 1 − 2(x² + y²))
    // 모두 도로 바꾼다. 특이점에서는 yaw와 roll이 같은 축을 공유해 yaw에 합치며, 임계 안쪽의
    // 작은 roll·pitch 차이는 UE와 같이 버린다.
    // float는 UE와 같이 pitch에 math::FastAsin(근사, 절대 오차 약 2.3e-7 rad)을, double은 Asin을
    // 쓴다. 결과 구간도 UE와 같다: pitch [-90, 90], yaw·roll은 atan2 그대로 [-180, 180](-180도
    // 나온다 — 예: Quat(1, 0, 0, 0)의 roll은 -180), 특이점의 yaw는 (-180, 180].
    // q와 -q는 같은 각을 낸다(특이점의 yaw는 반올림 수준까지). 결과를 ToQuaternion으로 되돌리면
    // 같은 회전이다(특이점 임계 안쪽 입력은 버린 roll만큼 다르다).
    //   Rotator<double>::TryFromQuaternion(Quatd(0.0, 0.0, 1.0, 0.0)); // (0, 180, 0)
    // 실패(InvalidArgument): 비유한 성분 또는 영 quaternion(정규화 불가).
    static Result<Rotator> TryFromQuaternion(const Quaternion<T>& rotation) noexcept {
        auto unit = rotation.TryNormalize();
        if (!unit) {
            return Result<Rotator>::Failure(ErrorCode::InvalidArgument);
        }

        const Quaternion<T>& q = unit.Value();
        const T one = static_cast<T>(1);
        const T two = static_cast<T>(2);
        constexpr T SingularityThreshold = static_cast<T>(0.4999995);
        constexpr T QuarterTurn = static_cast<T>(90);
        const T radiansToDegrees = static_cast<T>(180) / math::Pi<T>;
        const T singularityTest = q.z * q.x - q.w * q.y;
        const T yawY = two * (q.w * q.z + q.x * q.y);
        const T yawX = one - two * (math::Square(q.y) + math::Square(q.z));
        if (singularityTest < -SingularityThreshold) {
            return Result<Rotator>::Success(Rotator(
                -QuarterTurn, NormalizeAxis(-two * math::Atan2(q.x, q.w) * radiansToDegrees),
                static_cast<T>(0)));
        }

        if (singularityTest > SingularityThreshold) {
            return Result<Rotator>::Success(
                Rotator(QuarterTurn, NormalizeAxis(two * math::Atan2(q.x, q.w) * radiansToDegrees),
                        static_cast<T>(0)));
        }

        const T pitchDegrees = math::FastAsin(two * singularityTest) * radiansToDegrees;
        const T yawDegrees = math::Atan2(yawY, yawX) * radiansToDegrees;
        const T rollDegrees = math::Atan2(-two * (q.w * q.x + q.y * q.z),
                                          one - two * (math::Square(q.x) + math::Square(q.y))) *
                              radiansToDegrees;
        return Result<Rotator>::Success(Rotator(pitchDegrees, yawDegrees, rollDegrees));
    }
};

} // namespace rocks
