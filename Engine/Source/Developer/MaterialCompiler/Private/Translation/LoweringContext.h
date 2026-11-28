#pragma once

#include <Core/Containers/Span.h>
#include <Core/Memory/SystemAllocator.h>
#include <Core/Optional.h>
#include <Core/Result.h>
#include <Core/Text/NamePool.h>
#include <Core/Text/StringView.h>
#include <MaterialCompiler/MaterialDiagnosticCode.h>
#include <MaterialCompiler/MaterialCollectionLayout.h>
#include <MaterialCompiler/MaterialColorCurveSnapshot.h>
#include <MaterialCompiler/MaterialSourceProvider.h>
#include <MaterialCompiler/MaterialIrBuilder.h>
#include <MaterialCompiler/MaterialStaticParameterValue.h>
#include <MaterialCompiler/MaterialTextureFacts.h>
#include <MaterialCompiler/MaterialTargetField.h>
#include <MaterialCompiler/MaterialTranslationTarget.h>
#include <MaterialSchema/MaterialAttributeDefinition.h>
#include <MaterialSchema/MaterialNodeDefinition.h>
#include <MaterialSchema/MaterialTraitsDesc.h>
#include <MaterialSchema/NodePropertyReader.h>
#include <NodeGraph/GraphDiagnostic.h>
#include <NodeGraph/NodeTypeDescriptor.h>

#include "AttributeEntry.h"
#include "SurfaceRecord.h"

// lowering 함수 하나가 보는 노드 문맥이다(Docs/Design/MaterialSystem 6.8절). 입력 값 요청(연결 →
// 핀 사용자 값 → 정의 기본값 → 필수 입력 오류), 속성 읽기, 출력 기록, 진단, 타입 규칙을 지키는 연산
// 도우미("입력 값과 변환"·"연산 묶음별 수 종류")를 준다.
//
// 값 규칙: 그래프 오류가 난 자리는 NoIrValue다. 도우미는 피연산자에 NoIrValue가 있으면 진단 없이
// NoIrValue를 돌려준다(오류가 한 번만 남는다).
namespace rocks::material::detail {

class Translator;

// 핀 타입이 정하는 IR 타입이다(수·Bool 타입만 — 텍스처·Numeric·Any는 false).
bool LoweringPinIrType(MaterialPinType pin, IrType& out) noexcept;

// 진단 문구용 타입 이름이다(HLSL 철자, 텍스처는 UE DescribeType 이름 — 영어).
StringView LoweringTypeName(IrType type) noexcept;

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

    // 번역 입력의 이름 pool이다(동적 핀 키 원문 등).
    NamePool& Names() noexcept;

    // 지금 번역 중인 평가 문맥이다(6.9절).
    EvaluationContext Context() const noexcept;

    // RVT 출력 노드 입력을 번역하는 중인가(6.11절 MS2d3 RVT 범위).
    bool InVirtualTextureScope() const noexcept;

    // 번역 중인 머티리얼의 기본 속성(영역·블렌드 모드)이다.
    const MaterialTraitsDesc& Traits() const noexcept;

    // 번역 대상이다. field를 읽은 항목으로 기록한다(6.9절 MS2b3 — 쿠킹 번역 키).
    const MaterialTranslationTarget& Target(MaterialTargetField field) noexcept;

    // 번역 입력의 static 파라미터 값이다(6.10절 MS2c2). id·종류가 같은 값이 없으면 nullptr이다(노드
    // 기본값을 쓴다).
    const MaterialStaticParameterValue*
    FindStaticParameter(Guid id, MaterialStaticParameterKind kind) const noexcept;

    // 번역 입력의 컬렉션 layout이다. 없으면 nullptr이다.
    const MaterialCollectionLayout* FindCollection(Guid asset) const noexcept;

    // 표면 기록을 더하고 그 표면 값을 돌려준다(record.node는 이 노드로 채운다 — 6.11절 MS2d1).
    Result<IrValueId> MakeSurface(SurfaceRecord record) noexcept;
    // 표면 값의 기록이다(아니면 nullptr).
    const SurfaceRecord* SurfaceOf(IrValueId value) const noexcept;

    // 속성 집합 값을 만든다(6.12절 MS2e1 — entries는 설정된 칸, 번역기 기록 밖의 배열이어야 한다).
    Result<IrValueId> MakeAttributes(Span<const AttributeEntry> entries) noexcept;
    // 속성 집합 값의 칸을 out에 복사한다(속성 집합 값이 아니면 false).
    Result<bool> CopyAttributes(IrValueId value, Array<AttributeEntry>& out) const noexcept;
    // 속성 정의다(내장 → 등록 표, 없으면 nullptr).
    const MaterialAttributeDefinition* FindAttribute(Guid id) const noexcept;
    const MaterialAttributeDefinition* FindAttributeByName(StringView name) const noexcept;
    // 키가 key인 내장 속성이다(Aggregate 동적 핀 — 키 = 속성 이름, 없으면 nullptr).
    Result<const MaterialAttributeDefinition*> FindAttributeByKey(Name key) noexcept;
    // 설정되지 않은 칸의 기본값이다(UE CompileDefaultValue — 6.12절 MS2e1 기본값 규칙).
    // FrontMaterial은 NoIrValue(표면 변환이 정한다).
    Result<IrValueId> AttributeDefault(const MaterialAttributeDefinition& definition) noexcept;
    // 칸 값(없으면 기본값)이다.
    Result<IrValueId> AttributeValue(Span<const AttributeEntry> entries,
                                     const MaterialAttributeDefinition& definition) noexcept;
    // 칸에 넣을 값을 속성 형으로 맞춘다(5.3절 Coerce — UE ValidCast). 표면 칸은 표면 값만 받는다.
    Result<IrValueId> FitAttribute(IrValueId value, const MaterialAttributeDefinition& definition,
                                   StringView pin) noexcept;

    // value가 결과에 닿을 때만 이 노드 위치에 낼 오류다(지연 진단 — UE처럼 쓰이는 출력의 검사만
    // 남긴다, 6.10절 MS2c4). message는 문자열 상수여야 한다(번역 끝까지 산다).
    Result<void> DeferError(IrValueId value, MaterialDiagnosticCode code,
                            StringView message) noexcept;

    // UserSceneTexture 이름을 번호로 바꾼다. 상한을 넘으면 UE 문구로 진단하고 false다(6.10절
    // MS2c4).
    Result<bool> UseUserSceneTexture(StringView name, uint32& index) noexcept;

    // 번역 입력의 텍스처 사실이다(6.10절 MS2c3). 없으면 nullptr이다.
    const MaterialTextureFacts* FindTexture(Guid asset) const noexcept;

    // 번역 입력의 원본 공급이다(없으면 nullptr).
    const MaterialSourceProvider* Sources() const noexcept;

    // 원본 공급의 색 커브 snapshot이다(6.13절 MS2f4 — 찾으면 번역 의존 ColorCurve로 기록한다).
    // 없거나 공급이 없으면 nullptr이다. 오류: 할당 실패.
    Result<const MaterialColorCurveSnapshot*> FindColorCurve(Guid asset) noexcept;

    // 원본 공급의 색 커브 atlas snapshot이다(6.10절 MS2c5 — 찾으면 번역 의존 CurveAtlas로
    // 기록한다). 없거나 공급이 없으면 nullptr이다. 오류: 할당 실패.
    Result<const MaterialCurveAtlasSnapshot*> FindCurveAtlas(Guid asset) noexcept;

    // 자기 샘플러를 쓰는 텍스처 slot(출처, id)을 센다. 번역 대상 maxSamplers를 넘으면 이 노드
    // 위치에 V9를 진단하고 false다(6.10절 MS2c3).
    Result<bool> UseSampler(IrTextureSource source, Guid id) noexcept;

    // 컬렉션 하나를 참조한다(6.10절 MS2c2). 머티리얼당 상한(MaxMaterialCollections)을 넘으면 이
    // 노드 위치에 V9를 진단하고 false다.
    Result<bool> UseCollection(Guid asset) noexcept;

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
    // field가 "parameterId"이고 노드 속성에 shared가 있으면(파라미터 노드) IR 정체 Guid로
    // 바꾼다(6.12절 MS2e2 파라미터 scope — 주 그래프 노드는 그대로).
    Result<bool> ReadGuid(StringView field, Guid& out) noexcept;
    // 파라미터 노드의 값 hook 결과다(12.8절 MS3d1 — 정의의 ParameterValue). 읽지 못하면
    // InvalidProperty를 진단하고 false다. 오류: hook이 없는 노드 InvalidArgument(lowering의 잘못),
    // 할당 실패.
    Result<bool> ParameterMetadata(MaterialParameterMetadata& out) noexcept;
    // 이 파라미터 노드의 출처를 기록한다(identity는 ReadGuid("parameterId")가 준 정체 — 공유
    // 여부는 속성 shared). 오류: 할당 실패.
    Result<void> RecordParameterOrigin(Guid identity,
                                       const MaterialParameterMetadata& metadata) noexcept;
    // LinearColor 칸(r·g·b·a)이다.
    Result<bool> ReadColor(StringView field, float (&out)[MaxIrComponents]) noexcept;
    // MaterialAssetReference 칸의 AssetId Guid다(null이면 0).
    Result<bool> ReadAssetReference(StringView field, Guid& out) noexcept;

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
    // 정적 입력 key를 context 문맥에서 번역한 값이다(VertexInterpolator — 6.9절 MS2b4a). 연결이
    // 없으면 NoIrValue다.
    Result<IrValueId> InputInContext(EvaluationContext context, StringView key) noexcept;

    // 키 원문으로 찾는 동적 입력이다(필수 아님 — 연결이 없으면 fallback).
    Result<IrValueId> NamedDynamicInput(StringView key, Optional<float> fallback) noexcept;

    // 출력 하나를 기록한다. NoIrValue는 기록하지 않는다(받는 쪽이 NoIrValue를 본다).
    Result<void> Output(StringView key, IrValueId value) noexcept;
    Result<void> OutputByName(Name key, IrValueId value) noexcept;

    // 이 노드 위치의 진단이다. pin은 정적 키 원문(빈 문자열이면 노드 전체)이다.
    Result<void> Error(MaterialDiagnosticCode code, StringView pin, StringView message) noexcept;
    Result<void> Warning(MaterialDiagnosticCode code, StringView pin, StringView message) noexcept;
    Result<void> Info(MaterialDiagnosticCode code, StringView pin, StringView message) noexcept;

    // 이 노드가 펼친 FunctionInput 사본이면 그 호출 노드 입력(call, pin)이다(6.12절 MS2e2).
    bool FunctionInputBinding(Guid& call, Name& pin) const noexcept;
    // 호출 노드 call의 입력 pin 값이다(FunctionInput 사본 — 연결된 입력이 값 없이 오면 이 노드도
    // 실패다).
    Result<IrValueId> CallerInput(Guid call, Name pin, bool& connected) noexcept;
    // 이 레이어 노드의 결과다(6.12절 MS2e3). 활성 레이어가 없으면 passThrough가 참이고
    // NoIrValue다. 오류는 진단한 뒤 NoIrValue다.
    Result<IrValueId> LayerValue(bool& passThrough) noexcept;

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

    // Float width 핀 자리로의 암묵 변환이다(Coerce — 스칼라 복제, 넓은 벡터는 앞 성분, 좁은 벡터는
    // V1). UE Substrate·custom 출력 입력처럼 HLSL 암묵 변환을 따르는 자리에 쓴다(6.11절).
    Result<IrValueId> CoerceToWidth(IrValueId value, uint8 width, StringView pin) noexcept;

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
    // 속성 집합 둘의 칸별 선택이다(Select·Branch — 6.12절 MS2e1).
    Result<IrValueId> SelectAttributes(IrValueId condition, IrValueId whenTrue, IrValueId whenFalse,
                                       bool branch) noexcept;
    // 텍스처 값이면 V1을 진단하고 rejected를 참으로 한다(돌려주는 값은 NoIrValue). 아니면 value다.
    Result<IrValueId> RejectTexture(IrValueId value, bool& rejected) noexcept;

    Translator& translator_;
    const nodegraph::GraphNode& node_;
    const MaterialNodeDefinition& definition_;
    SystemAllocator scratch_;
    Optional<NodePropertyReader> properties_;
    bool propertiesOpened_ = false;
    nodegraph::PinList dynamicPins_;
    bool dynamicPinsComputed_ = false;
    bool poisoned_ = false;
    // RejectTexture의 마지막 판정이다(도우미 안에서만 쓴다).
    bool textureRefused_ = false;
};

} // namespace rocks::material::detail
