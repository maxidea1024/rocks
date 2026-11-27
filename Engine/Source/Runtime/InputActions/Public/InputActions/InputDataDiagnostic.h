#pragma once

#include <Base/NumericLimits.h>
#include <Base/Types.h>

namespace rocks::input {

// 입력 데이터 검증이 거부한 까닭이다(Docs/Design/InputActions 2.7절).
enum class InputDataIssue : uint8 {
    None,
    // 컨텍스트 id가 0이다.
    ContextIdMissing,
    // 액션 id가 0이다.
    ActionIdMissing,
    // 같은 id의 액션이 둘이다(action = 뒤의 것).
    DuplicateAction,
    // 바인딩의 액션 id가 bundle 액션 목록에 없다.
    UnknownAction,
    // enum 값이 범위 밖이다.
    InvalidEnum,
    // 비유한(NaN·무한) 매개변수다.
    NonFiniteParameter,
    // DeadZone의 lower ≥ upper다(UE IsDataValid).
    DeadZoneRange,
    // ChordAction·ChordBlocker 트리거의 대상 id가 0이다(UE IsDataValid).
    ChordTargetMissing,
    // PlayerMappingSettings.name이 비었다(액션·바인딩에 값이 있을 때 — UE IsDataValid).
    MappingNameMissing,
    // Boolean 액션의 액션 수준 Scalar 모디파이어다(UE InputModifiers.cpp:82-95).
    ScalarOnBooleanAction,
    // 프로필 override 이름이 비었다.
    ProfileNameMissing,
    // 프로필 override 이름이 앞의 것과 같다(profile = 뒤의 것).
    DuplicateProfile,
    // 모디파이어의 응답 커브 slot이 평탄화되지 않았다(평가기는 slot 없는 값만 받는다 — 2.9절 IA9).
    UnresolvedCurveAsset
};

// 위치 칸의 "없음"이다.
inline constexpr uint32 NoInputDataIndex = NumericLimits<uint32>::Max();

// 검증 진단의 원소 종류다.
enum class InputDataElement : uint8 {
    None,
    Trigger,
    Modifier
};

// 검증이 거부한 첫 위치다. action은 bundle 액션 목록 번호(액션 설정 안이면), profile은 프로필
// override 번호(기본 bindings면 NoInputDataIndex), binding은 그 목록의 바인딩 번호,
// element·elementKind는 그 바인딩·액션 안의 트리거·모디파이어 번호다. 해당 없는 칸은
// NoInputDataIndex다.
struct InputDataDiagnostic {
    InputDataIssue issue = InputDataIssue::None;
    uint32 action = NoInputDataIndex;
    uint32 profile = NoInputDataIndex;
    uint32 binding = NoInputDataIndex;
    InputDataElement elementKind = InputDataElement::None;
    uint32 element = NoInputDataIndex;
};

} // namespace rocks::input
