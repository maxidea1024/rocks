#pragma once

#include <Base/Types.h>

// 머티리얼 번역 진단 코드다(Docs/Design/MaterialSystem 5.4·6.8절). NodeGraph GraphDiagnostic.code에
// 넣는다. 1–15는 검증 규칙 V 번호이고, 100번대는 번역 코드다.
namespace rocks::material {

enum class MaterialDiagnosticCode : uint32 {
    // 핀 타입·폭·암묵 변환(V1).
    TypeMismatch = 1,
    // 데이터 흐름 순환(V2).
    Cycle = 2,
    // 함수 호출·인스턴스 부모 순환(V3).
    FunctionCycle = 3,
    // 단계 규칙 — 정점 진입점에 픽셀 전용 값(V4).
    StageViolation = 4,
    // 영역·블렌드 모드가 쓰지 않는 출력 핀 연결(V5, 경고).
    InactiveOutput = 5,
    // 렌더 경로·pass·형식이 주지 않는 기능(V6).
    UnsupportedCapability = 6,
    // static 입력에 비static 값(V7).
    NonStaticValue = 7,
    // 같은 이름·다른 종류 파라미터(V8).
    ParameterConflict = 8,
    // 자원·보간·상수 예산 초과(V9).
    BudgetExceeded = 9,
    // 텍스처 샘플 종류와 텍스처 설정 불일치(V10).
    SamplerMismatch = 10,
    // NaN·Inf 상수(V11).
    NonFiniteConstant = 11,
    // 등록되지 않은 노드(V12).
    UnknownNode = 12,
    // 임의 HLSL 노드 규칙(V13).
    CustomCodeRule = 13,
    // 레이어 스택 규칙(V14).
    LayerStackRule = 14,
    // Render Path Switch가 깊이 출력에 닿음(V15).
    RenderPathDepth = 15,
    // 필수 입력에 연결이 없다.
    MissingInput = 101,
    // 카탈로그 노드지만 이 단계에서 아직 번역하지 않는다.
    NotYetLowered = 102,
    // 노드 속성을 읽을 수 없거나 값이 규칙 밖이다.
    InvalidProperty = 103,
    // 긴 벡터를 짧은 입력에 넣어 앞 성분만 썼다(정보).
    NarrowedValue = 104,
    // 핀 사용자 값 Record가 규칙 밖이다.
    InvalidPinValue = 105,
    // 번역 때 0 나누기가 확정된다.
    DivideByZero = 106,
    // 출력에서 거슬러 가는 노드 사슬이 번역 상한보다 깊다.
    GraphTooDeep = 107,
    // 노드가 아직 없는 선행(19.4.1절 MPQ-*)을 기다린다(TODO 노드 — 6.10절 MS2c1).
    PrerequisiteMissing = 108
};

constexpr uint32 ToDiagnosticCode(MaterialDiagnosticCode code) noexcept {
    return static_cast<uint32>(code);
}

} // namespace rocks::material
