#pragma once

#include <Core/Containers/Span.h>
#include <Core/Memory/SystemAllocator.h>
#include <Core/Optional.h>
#include <Core/Result.h>
#include <Core/Text/StringView.h>
#include <MaterialCompiler/MaterialDiagnosticCode.h>
#include <MaterialCompiler/MaterialIrBuilder.h>
#include <MaterialCompiler/MaterialTargetField.h>
#include <MaterialCompiler/MaterialTranslationTarget.h>
#include <MaterialSchema/MaterialNodeDefinition.h>
#include <MaterialSchema/NodePropertyReader.h>
#include <NodeGraph/GraphDiagnostic.h>
#include <NodeGraph/NodeTypeDescriptor.h>

// lowering 함수 하나가 보는 노드 문맥이다(Docs/Design/MaterialSystem 6.8절). 입력 값 요청(연결 →
// 핀 사용자 값 → 정의 기본값 → 필수 입력 오류), 속성 읽기, 출력 기록, 진단, 타입 규칙을 지키는 연산
// 도우미("입력 값과 변환"·"연산 묶음별 수 종류")를 준다.
//
// 값 규칙: 그래프 오류가 난 자리는 NoIrValue다. 도우미는 피연산자에 NoIrValue가 있으면 진단 없이
// NoIrValue를 돌려준다(오류가 한 번만 남는다).
namespace rocks::material::detail {

class Translator;

enum class EvaluationContext : uint8;

// 연산 묶음별 수 종류 규칙이다(6.8절 "연산 묶음별 수 종류").
enum class NumberRule : uint8 {
    // (a) Double 허용. 단항은 Int·UInt를 Float로 바꾼다.
    Arithmetic = 0,
    // Double은 Float로 낮추고 정수는 그대로다(Modulo·Clamp).
    NoDouble = 1,
    // (b) Float 전용: Double은 낮추고 Int·UInt는 Float로 바꾼다.
    FloatOnly = 2,
    // (c) 정수 전용: Float·Double은 V1이다.
    IntegerOnly = 3,
    // (d) Bool 전용.
    Logical = 4,
    // 같음 비교: Bool끼리도 받고, 그 밖은 Arithmetic과 같다.
    Equality = 5
};

class LoweringContext {
public:
    LoweringContext(Translator& translator, const nodegraph::GraphNode& node,
                    const MaterialNodeDefinition& definition) noexcept;

    LoweringContext(const LoweringContext&) = delete;
    LoweringContext& operator=(const LoweringContext&) = delete;

    const nodegraph::GraphNode& Node() const noexcept {
        return node_;
    }

    const MaterialNodeDefinition& Definition() const noexcept {
        return definition_;
    }

    IrBuilder& Builder() noexcept;

    // 지금 번역 중인 평가 문맥이다(6.9절).
    EvaluationContext Context() const noexcept;

    // 번역 대상이다. field를 읽은 항목으로 기록한다(6.9절 MS2b3 — 쿠킹 번역 키).
    const MaterialTranslationTarget& Target(MaterialTargetField field) noexcept;

    // 문맥 값 표(19.3.2절)의 name 값이다(IR Extern — 6.9절). 이전 정점 문맥에서 이전 변형이 있는
    // 값은 이전 프레임 값이다. arguments는 함수형 값의 인자(표의 argumentCount개)이고, 하나라도
    // NoIrValue면 진단 없이 NoIrValue다. 오류: 표에 없는 이름이나 인자 수 불일치는 lowering 함수의
    // 잘못이라 InvalidArgument.
    Result<IrValueId> ContextValue(StringView name, Span<const IrValueId> arguments = {}) noexcept;

    // 연결된 입력이 값 없이(오류가 이미 난 위쪽 노드) 들어왔는가. 그러면 이 노드는 실패로 끝나고,
    // 받는 쪽은 "값 없음"을 다시 진단하지 않는다(오류가 한 번만 남는다).
    bool Poisoned() const noexcept {
        return poisoned_;
    }

    // 속성 reader다. 정의에 속성이 없거나 Record를 읽지 못하면(InvalidProperty 진단) nullptr이다.
    Result<NodePropertyReader*> Properties() noexcept;

    // 속성 한 칸을 읽는다. 읽지 못하면 InvalidProperty를 진단하고 false다(out은 fallback).
    Result<bool> ReadFloat(StringView field, float fallback, float& out) noexcept;
    Result<bool> ReadDouble(StringView field, double fallback, double& out) noexcept;
    Result<bool> ReadInteger(StringView field, int64 fallback, int64& out) noexcept;
    Result<bool> ReadBool(StringView field, bool fallback, bool& out) noexcept;
    Result<bool> ReadGuid(StringView field, Guid& out) noexcept;
    // LinearColor 칸(r·g·b·a)이다.
    Result<bool> ReadColor(StringView field, float (&out)[MaxIrComponents]) noexcept;

    // 동적 핀(정의의 동적 핀 함수 결과)이다. 함수가 없으면 비어 있다. 계산이 실패하면
    // InvalidProperty를 진단하고 비어 있다.
    Result<Span<const nodegraph::PinDescriptor>> DynamicPins() noexcept;

    // 정적 입력 key에 실제 연결이 있는가(비활성 노드는 통과 쌍을 따라간다 — 끊기면 연결 없음).
    Result<bool> IsConnected(StringView key) noexcept;

    // 정적 입력 key의 값이다: 연결 → 핀 사용자 값 → 정의 기본값(Float 상수). 셋 다 없으면 필수
    // 입력은 MissingInput을 진단하고, 아니면 진단 없이 NoIrValue다(IsConnected로 구분한다).
    Result<IrValueId> Input(StringView key) noexcept;

    // 동적 입력 key의 값이다. 연결이 없으면 fallback(있으면 Float 스칼라 상수)이고, 없고 required면
    // MissingInput이다.
    Result<IrValueId> DynamicInput(Name key, Optional<float> fallback, bool required) noexcept;
    // 키 원문으로 찾는 동적 입력이다(필수 아님 — 연결이 없으면 fallback).
    Result<IrValueId> NamedDynamicInput(StringView key, Optional<float> fallback) noexcept;

    // 출력 하나를 기록한다. NoIrValue는 기록하지 않는다(받는 쪽이 NoIrValue를 본다).
    Result<void> Output(StringView key, IrValueId value) noexcept;
    Result<void> OutputByName(Name key, IrValueId value) noexcept;

    // 이 노드 위치의 진단이다. pin은 정적 키 원문(빈 문자열이면 노드 전체)이다.
    Result<void> Error(MaterialDiagnosticCode code, StringView pin, StringView message) noexcept;
    Result<void> Warning(MaterialDiagnosticCode code, StringView pin, StringView message) noexcept;
    Result<void> Info(MaterialDiagnosticCode code, StringView pin, StringView message) noexcept;

    // 다른 노드(종류 expectedType)의 출력이다(NamedRerouteUsage의 선언 — 순환은 V2). 그 id의 노드가
    // 없거나 종류가 다르면 found가 거짓이다.
    Result<IrValueId> NodeOutput(Guid node, StringView expectedType, StringView pin,
                                 bool& found) noexcept;

    IrType TypeOf(IrValueId value) const noexcept;

    // 상수다. 유한하지 않으면 V11을 pin 위치에 진단하고 NoIrValue다.
    Result<IrValueId> FloatConstant(Span<const float> components, StringView pin) noexcept;
    Result<IrValueId> FloatScalar(float value) noexcept;
    Result<IrValueId> DoubleConstant(double value, StringView pin) noexcept;
    // type의 모든 성분이 0·1인 상수다(Bool은 false·true).
    Result<IrValueId> Zero(IrType type) noexcept;
    Result<IrValueId> One(IrType type) noexcept;

    // 수 종류를 바꾼다(같으면 그대로).
    Result<IrValueId> Convert(IrValueId value, IrScalar scalar) noexcept;
    // 스칼라를 components개로 복제한다(이미 그 폭이면 그대로).
    Result<IrValueId> Splat(IrValueId value, uint8 components) noexcept;
    // 앞 components 성분이다(같은 폭이면 그대로).
    Result<IrValueId> Prefix(IrValueId value, uint8 components) noexcept;
    Result<IrValueId> Swizzle(IrValueId value, Span<const uint8> components) noexcept;
    Result<IrValueId> Append(IrValueId left, IrValueId right) noexcept;

    // 규칙대로 피연산자를 맞춘 연산이다(폭: 같거나 한쪽 스칼라, 수 종류: rule). 규칙을 어기면
    // V1이다.
    Result<IrValueId> Unary(IrOp op, IrValueId value, NumberRule rule) noexcept;
    Result<IrValueId> Binary(IrOp op, IrValueId left, IrValueId right, NumberRule rule) noexcept;
    Result<IrValueId> Ternary(IrOp op, IrValueId first, IrValueId second, IrValueId third,
                              NumberRule rule) noexcept;

    // 조건 선택이다. 두 값은 산술 공통 타입(Bool끼리도 된다)이고 조건은 Bool 스칼라나 같은 폭이다.
    Result<IrValueId> Select(IrValueId condition, IrValueId whenTrue, IrValueId whenFalse) noexcept;
    // 동적 분기다(조건 Bool 스칼라).
    Result<IrValueId> Branch(IrValueId condition, IrValueId whenTrue, IrValueId whenFalse) noexcept;

    // 출력 노드 핀처럼 타입이 정해진 자리로의 암묵 변환이다(5.3절). pin은 진단 위치다.
    Result<IrValueId> Coerce(IrValueId value, MaterialPinType type, StringView pin) noexcept;

    // values의 산술 공통 타입이다(폭: 같거나 스칼라, 수 종류: 6.8절). 맞지 않으면 V1을 진단하고
    // 빈 Optional이다. Bool은 모두 Bool일 때만 받는다.
    Result<Optional<IrType>> CommonType(Span<const IrValueId> values, NumberRule rule) noexcept;

    // value를 type으로 맞춘다(스칼라 복제 + 수 종류 변환 — CommonType 결과용).
    Result<IrValueId> Cast(IrValueId value, IrType type) noexcept;

    // UE CoerceParameter와 같은 맞춤이다(6.8절 "첫 인자 타입"): 수 종류를 target으로 바꾸고, 폭은
    // 같으면 그대로, 스칼라는 복제, target이 스칼라면 첫 성분이다. 그 밖(다른 폭의 두 벡터)과
    // Bool·수 섞임은 V1을 pin 위치에 진단하고 NoIrValue다.
    Result<IrValueId> CoerceTo(IrValueId value, IrType target, StringView pin) noexcept;

    // 값이 Float 스칼라 상수면 그 값이다.
    bool ConstantFloat(IrValueId value, float& out) const noexcept;
    // 값이 Bool 스칼라 상수면 그 값이다.
    bool ConstantBool(IrValueId value, bool& out) const noexcept;

private:
    Result<Name> PinName(StringView key) noexcept;
    const MaterialPinDefinition* FindPin(StringView key) const noexcept;
    Result<IrValueId> PinValue(const MaterialPinDefinition& pin, Name name) noexcept;
    Result<IrValueId> Connected(Name name, bool& connected) noexcept;
    Result<void> Diagnose(nodegraph::GraphDiagnosticSeverity severity, MaterialDiagnosticCode code,
                          StringView pin, StringView message) noexcept;
    Result<void> TypeError(StringView message, IrType left, IrType right) noexcept;

    Translator& translator_;
    const nodegraph::GraphNode& node_;
    const MaterialNodeDefinition& definition_;
    SystemAllocator scratch_;
    Optional<NodePropertyReader> properties_;
    bool propertiesOpened_ = false;
    nodegraph::PinList dynamicPins_;
    bool dynamicPinsComputed_ = false;
    bool poisoned_ = false;
};

} // namespace rocks::material::detail
