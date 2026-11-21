#pragma once

#include <Core/Text/Json.h>

#include "NameBinder.h"

// 타입 검사(LC2c)다(Docs/Design/SparksLanguage.ko.md 3.10절, S0326·S04xx·S05xx·S0109). 이름 해석
// (NameBinder)의 이름 공간과 심볼을 그대로 쓰며 NameBinder::Run의 마지막에 돈다. 파일:
//   TypeRelations.cpp    숫자 분류·대입 가능·파생·`==`·`<`·기본값·제네릭 치환·타입 문장
//   CheckExpressions.cpp 식·연산자·상수·기대 타입 맞추기
//   CheckCalls.cpp       멤버 접근·호출·생성·변환·검증 리터럴
//   CheckStatements.cpp  선언 초기값·본문·문장·반환·switch

namespace rocks::sparks::detail {

// 숫자 타입의 분류다.
enum class NumericKind : uint8 {
    None,
    Signed,
    Unsigned,
    Float
};

struct NumericInfo {
    NumericKind kind = NumericKind::None;
    uint32 bits = 0;
};

// 컴파일 때 아는 값이다(상수식·switch case·const).
struct ConstantValue {
    enum class Kind : uint8 {
        None,
        Integer,
        Float,
        Bool,
        String,
        Enum,
        Invalid // 값을 정하려다 진단이 난 상수(const 초기값 오류 등) — 더 알리지 않는다
    };

    Kind kind = Kind::None;
    bool negative = false; // Integer: 값은 negative ? -magnitude : magnitude
    uint64 magnitude = 0;
    double real = 0.0;
    bool boolean = false;
    uint32 symbol = NoSymbol; // Enum: case 심볼
    StringView text;          // String: 리터럴 원문(따옴표·`@` 포함 — 값은 DecodeStringLiteral)
};

// ID manifest 항목의 이름(또는 alias) 하나다(CheckStableIds.cpp — kind는 manifest kind 번호).
struct ManifestIdName {
    uint8 kind = 0;
    StringView name;
    uint32 entry = 0;
};

// 기대 타입이 앞선 오류로 정해지지 않은 자리의 표지다(Check·Coerce의 expected·type 인자로만 쓴다).
// 기대 타입이 없는 자리(NoType)와 달리 `null`·`[]`·Map 리터럴·`Asset`·`Row`·`CVar(...)`에 S0417을
// 내지 않는다(3.10절 연쇄 방지). 타입 표에 없는 값이므로 타입 번호로 쓰지 않는다.
inline constexpr uint32 UnresolvedType = NumericLimits<uint32>::Max() - 2;

// 지역 번호 표(심볼 → 지역 번호)의 "없음" 표지다(struct 그래프·초기값 의존 그래프).
inline constexpr uint32 NoLocalIndex = NumericLimits<uint32>::Max();

// 필드·const 초기값이 다른 초기값을 따라가는 깊이 상한이다(S0427).
inline constexpr uint32 MaxInitializerDepth = 64;

// 식 하나의 결과다.
struct ExpressionValue {
    enum class Kind : uint8 {
        Error,        // 해석하지 못했다(진단은 이미 있다 — 더 내지 않는다)
        Typed,        // 그 타입의 값
        UntypedInt,   // 타입 없는 정수 상수
        UntypedFloat, // 타입 없는 실수 상수
        Null,         // `null`
        Void,         // 결과 없음
        Wait,         // 대기(type은 await 결과)
        TypeName,     // 식에 쓴 타입 이름(symbol)
        Namespace,    // prelude 이름 공간·import 별칭(symbol)
        Function,     // 부를 함수(symbol, receiver)
        Event,        // 이벤트(symbol, receiver) — await하면 void
        Literal       // 검증 리터럴 이름(`Action`·`Asset`·`Loc`·`CVar` — symbol)
    };

    Kind kind = Kind::Error;
    uint32 type = NoType;
    uint32 symbol = NoSymbol;
    uint32 receiver = NoType; // Function·Event: 수신자 타입(인스턴스 멤버·제네릭 치환)
    bool instance = false;    // Function: 값으로 불렀다(static·이름 공간 함수가 아니다)
    bool chained = false;     // `?.`로 얻었다(호출 결과도 `?`가 된다)
    bool stale = false;       // await 뒤에 읽은 객체 지역 이름이라 `T?`가 되었다(3.11절)
    ConstantValue constant;
};

// 본문 하나의 검사 자리다.
struct CheckContext {
    uint32 module = NoModule;
    uint32 owner = NoSymbol;     // class·struct 심볼
    uint32 thisType = NoType;    // class 본문이면 그 타입(아니면 `this` 없음)
    uint32 returnType = NoType;  // 함수 반환 타입(void 포함, 모르면 NoType)
    bool returnKnown = false;    // 반환 규칙을 검사하는지(시그니처를 해석했다)
    bool resultFunction = false; // 반환 타입이 Result<T>
    uint32 resultValue = NoType; // Result<T>의 T(Result<void>면 void 타입)
};

class FlowAnalyzer;
class IrBuilder;
class IrWriter;
class BytecodeEncoder;

class TypeChecker final {
public:
    explicit TypeChecker(NameBinder& binder) noexcept;

    void Run() noexcept;

private:
    friend class FlowAnalyzer;
    friend class IrBuilder;
    friend class IrWriter;
    friend class BytecodeEncoder;

    // 노드 대표 토큰의 글자다(연산자·멤버 이름·리터럴).
    StringView TokenText(uint32 module, uint32 node) const noexcept;

    // 모듈의 모든 타입 표기에 타입 인자 제약과 `void` 자리를 검사한다(3.10절 "타입 인자 제약").
    void CheckTypeRules(uint32 module) noexcept;

    // Sparks class나 엔진 object 타입인지다(`Class<T>`의 T, `?` 없음).
    bool IsClassType(uint32 type) const noexcept;

    // 타입(또는 그 기반 체인)의 멤버 이름 공간이 불완전한지다(3.9절 — 기반을 해석하지 못한 class).
    // 그런 타입의 멤버를 찾지 못해도 S0326을 내지 않는다(연쇄 방지).
    bool HasIncompleteMembers(uint32 type) const noexcept;

    // 정해진 상수 접기(3.10절): 두 값이 모두 상수면 결과 값을 result.constant에 둔다. 범위·유한성을
    // 어기면 S0503·S0505를 내고 false. operandType은 피연산자의 타입(비교면 비교하는 타입)이다.
    bool FoldBinary(const CheckContext& context, uint32 node, StringView op,
                    const ExpressionValue& left, const ExpressionValue& right, uint32 operandType,
                    ExpressionValue& result) noexcept;

    // 엔진 object·Sparks class·interface처럼 파괴될 수 있는 객체 타입인지다(에셋 제외, 부록 B
    // `[객체]`).
    bool IsObjectType(uint32 type) const noexcept;

    // 찾지 못한 멤버 이름과 가장 비슷한 멤버(편집 거리 2 이하, 같으면 byte 순)다. 없으면 빈 문자열.
    StringView SimilarMember(Span<const uint64> spaces, uint32 receiverType,
                             StringView name) const noexcept;

    // 식 안의 타입 인자가 함수가 아닌 것에 붙었으면 S0320을 내고 true다(3.10절 "호출").
    bool RejectTypeArguments(const CheckContext& context, uint32 typeArguments,
                             StringView name) noexcept;

    // S0425: 타입 이름으로 인스턴스 멤버(valueType NoType), 값으로 enum case·static 함수를 썼다.
    // 재귀 경로(CheckMember)의 frame을 작게 두려고 문장 만들기를 따로 둔다.
    void ReportStaticInstance(const CheckContext& context, uint32 node, StringView name,
                              uint32 valueType) noexcept;

    // ---- 선언 규칙(CheckDeclarations.cpp, 3.11절) ----
    // `?` 없는 객체 필드(S0428)·상수식(S0510)·const 타입(S0512)·프로필(S1113)을 검사한다.
    void CheckDeclarationRules(uint32 module) noexcept;
    void CheckProfile(uint32 module) noexcept;
    // const 초기값 순환(S0511, 순환 하나에 하나)을 찾는다(모든 모듈의 검사 뒤).
    void CheckConstCycles() noexcept;
    // root가 상수식이 아니면 처음(왼쪽부터) 상수식이 아닌 하위 식, 상수식이면 NoNode다.
    // allowComponent면 component 생성도 받는다(필드 초기값).
    uint32 FirstNonConstant(uint32 module, uint32 root, bool allowComponent) noexcept;
    bool IsConstType(uint32 type) const noexcept;
    // 선언(class·필드 등)에 그 이름의 attribute가 있는지다(이름만 본다 — 인자는 LC3).
    bool HasAttribute(uint32 module, uint32 declaration, StringView name) const noexcept;
    // (패키지 이름, 모듈 import 경로, 원문 위치) byte 순서로 left가 앞인지다(순환 진단 위치).
    bool DeclaredBefore(uint32 left, uint32 right) const noexcept;

    // ---- 이벤트·hook(CheckEvents.cpp, 3.12절 — Rules 단계) ----
    // class·struct 본문의 hook·handler·이벤트 선언과 함수의 재진입 표시를 검사한다.
    void CheckHandlers(const CheckContext& context, uint32 typeNode) noexcept;
    // 구간 하나짜리 `on X()`(seen은 이 class에서 이미 본 hook 이름)다.
    void CheckHook(const CheckContext& context, uint32 handler, uint32 nameNode,
                   Array<StringView>& seen) noexcept;
    void CheckPathHandler(const CheckContext& context, uint32 handler) noexcept;
    // handler 인자를 expected의 앞부분과 대조한다(S0805·S0810).
    void CheckHandlerParameters(const CheckContext& context, uint32 handler, StringView eventName,
                                Span<const uint32> expected, DiagnosticCode code) noexcept;
    // 재진입 표시(SD11): allowed면 await가 있을 때 표시가 하나 있어야 하고(S0816), 아니면 표시가
    // 없어야 한다(S0817).
    void CheckReentrancy(const CheckContext& context, uint32 node, bool allowed, bool awaits,
                         uint32 reportNode) noexcept;
    bool SamePath(uint32 leftModule, uint32 left, uint32 rightModule, uint32 right) const noexcept;
    // `emit` 경로를 해석해 이벤트 시그니처를 돌려준다(진단을 냈거나 모르면 NoSignature).
    uint32 ResolveEmit(const CheckContext& context, uint32 node) noexcept;
    void CollectExtensionEvents() noexcept;
    void CheckExtensionEvents() noexcept;
    // type(또는 그 원소)의 이벤트다(멤버 이벤트, Actor·World 확장 이벤트). 같은 이름의 확장
    // 이벤트가 import하지 않은 모듈에만 있으면 hiddenModule에 그 모듈을 둔다.
    // engineOnly면 Sparks 멤버 이벤트·언어 이벤트를 받지 않는다(`this` 원천). 보이는 확장 이벤트가
    // 다른 모듈에도 있으면 otherModule에 그 모듈을 둔다(모호성 — S0814).
    uint32 FindEvent(uint32 module, uint32 type, StringView name, uint32* hiddenModule,
                     uint32* otherModule, bool engineOnly) noexcept;
    uint32 FindExtensionEvent(uint32 module, uint32 target, StringView name, uint32* hiddenModule,
                              uint32* otherModule = nullptr) const noexcept;
    void ReportAmbiguousExtension(const CheckContext& context, uint32 node, uint32 event,
                                  uint32 otherModule) noexcept;
    // from 모듈에서 module을 import하는 경로(다른 패키지면 패키지 이름 포함)를 out에 붙인다.
    void AppendModuleDisplay(StringBuilderBase& out, uint32 from, uint32 module) const noexcept;
    // 식의 `X.E`에서 X 타입의 확장 이벤트다(없으면 NoSymbol, 모호하면 진단을 내고
    // AmbiguousExtension).
    uint32 MemberExtensionEvent(const CheckContext& context, uint32 node, uint32 type,
                                StringView name) noexcept;
    static constexpr uint32 AmbiguousExtension = NumericLimits<uint32>::Max() - 3;
    // package가 dependency에 (전이로) 의존하는지다.
    bool DependsOn(uint32 package, uint32 dependency) noexcept;
    bool HasChanged(uint32 field) const noexcept;
    bool ContainsAwait(uint32 module, uint32 node) noexcept;
    uint32 EngineSymbol(StringView name) const noexcept;

    // ---- 실행 문맥·효과·attribute(CheckContexts.cpp, 3.13절 — Rules 단계) ----
    // 함수·hook·handler 본문 하나의 효과다(direct는 본문이 직접, total은 부른 함수까지).
    struct EffectBody {
        uint32 module = NoModule;
        uint32 node = NoNode;
        uint32 symbol = NoSymbol; // 함수 심볼(handler는 NoSymbol)
        uint32 owner = NoSymbol;  // class·struct 심볼(최상위 함수는 NoSymbol)
        uint32 direct = 0;
        uint32 total = 0;
        uint32 firstSite = 0; // effectSites_ 구간
        uint32 siteCount = 0;
    };

    // 효과가 생기는 자리(await·emit·대입·호출)다. 호출이면 callee에 부른 함수가 있다.
    struct EffectSite {
        uint32 body;
        uint32 node;
        uint32 bits;
        uint32 callee;
    };

    // `while` 경로 판정의 상태(대기했는지)다.
    static constexpr uint8 WaitStateWaited = 1;
    static constexpr uint8 WaitStateNotWaited = 2;

    void CheckAttributes(uint32 module) noexcept;
    void CheckAttributeList(const CheckContext& context, uint32 declaration, uint32 place,
                            bool isVar, bool noParameters, bool worldServiceVar) noexcept;
    void CheckAttributeArguments(const CheckContext& context, uint32 declaration, uint32 attribute,
                                 StringView name, uint8 shape) noexcept;
    // attribute 인자가 숫자(리터럴, 앞의 `-`, 숫자 const 이름)인지다.
    bool IsAttributeNumber(uint32 module, uint32 expression, bool integer,
                           bool nonNegative) noexcept;
    void AnalyzeEffects() noexcept;
    void CollectBodyEffects(uint32 bodyIndex) noexcept;
    void CheckBodyContext(uint32 bodyIndex) noexcept;
    // 이 본문(class 메서드)이 대신하는 기반·interface 함수를 dispatchPairs_에 더한다.
    void AddDispatch(uint32 body) noexcept;
    void AddInheritedImplementations() noexcept;
    // 호출 대상 심볼이 부를 수 있는 본문(자기 본문, 재정의, interface 구현)마다 visit(본문)이다.
    template <typename Visit>
    void ForEachCallTarget(uint32 callee, Visit&& visit) const noexcept;
    uint32 SiteEffects(const EffectSite& site) const noexcept;
    // 문맥 attribute(자기 것, 없으면 재정의한 기반 함수 사슬의 것)다.
    StringView ContextAttribute(uint32 body) const noexcept;
    // 문장 안에 정지하는 `await`가 있는지다(효과 분석 뒤).
    bool StatementWaits(uint32 module, uint32 node) noexcept;
    uint32 AwaitEffect(uint32 module, uint32 operand) noexcept;
    uint32 WriteEffect(uint32 module, uint32 target) noexcept;
    // states(WaitState* 비트)로 node에 들어갔을 때 나가는 상태다. 대기하지 않은 `continue`가
    // 있으면 continuesWithoutWait를 켠다.
    uint8 WaitStates(uint32 module, uint32 node, uint8 states, bool& continuesWithoutWait) noexcept;

    // ---- 권한·RPC·저장 타입(CheckNetwork.cpp, 3.15절 — Rules 단계) ----
    // 저장 가능 타입(objects면 객체 참조를 더한 복제 타입)인지다.
    // 타입별 판정을 storable_에 기억한다(계산 중인 타입은 가능 — 컨테이너를 거친 재귀 struct).
    bool IsStorableType(uint32 type, bool objects) noexcept;
    bool ComputeStorable(uint32 value, bool objects) noexcept;
    // 타입이나 그 타입 인자가 유지 자원인지다(S0904가 알리므로 저장·복제 판정은 건너뛴다).
    bool ContainsHeldType(uint32 type) noexcept;
    // 재정의(override) 함수가 대신하는 기반 사슬의 멤버다(재정의가 아니면 NoSymbol).
    uint32 OverriddenMember(uint32 function) const noexcept;
    // 재정의 사슬이 엔진 overridable 함수에 닿는지다(엔진이 직접 부르는 진입 본문).
    bool OverridesEngineFunction(uint32 function) const noexcept;
    // `@rpc` 함수의 대상 접두 이름(`Server` 등)이다. 재정의는 기반의 것을 잇는다. RPC가 아니면
    // 빈 글자, 대상을 모르면 "?".
    StringView RpcTarget(uint32 function) const noexcept;
    // `Server.F` 등 RPC 접두 뒤의 멤버다(S1004).
    ExpressionValue RpcMember(const CheckContext& context, uint32 node, uint32 prefix,
                              StringView name) noexcept;
    void CheckNetwork(uint32 module) noexcept;
    // 권한 시험(`Owner.HasAuthority`, Actor의 `HasAuthority`, 그것을 한 항으로 둔 `&&`)인지다.
    bool IsAuthorityTest(uint32 module, uint32 expression) const noexcept;
    // 부정 권한 시험(`!HasAuthority`, 그것을 한 항으로 둔 `||`)인지다: 거짓이면 권한이 있다.
    bool IsNegatedAuthorityTest(uint32 module, uint32 expression) const noexcept;
    // 문장이 반드시 `return`으로 끝나는지다(`return` 또는 마지막 문장이 그런 블록).
    bool AlwaysReturns(uint32 module, uint32 statement) const noexcept;

    // ---- 유지 자원·로드(CheckResources.cpp, 3.14절 — Rules 단계) ----
    // 유지 자원 타입(schema `held` 엔진 객체, `LoadRequest<T>`, 그 `T?`)인지다.
    bool IsHeldType(uint32 type) noexcept;
    void CheckResources(uint32 module) noexcept;
    // `acquire` 효과의 엔진 함수가 유지 자원 타입을 돌려주는지 본다(S1120).
    void CheckAcquireReturns() noexcept;
    void CheckHeldEvents() noexcept;

    // ---- 안정 ID(CheckStableIds.cpp, SparksBytecode 6.1절 — Rules 단계) ----
    void CheckStableIds() noexcept;
    void CheckPackageIds(uint32 package, Span<const uint8> unstable) noexcept;
    // 이름 키의 모듈을 이 컴파일에서 판정했는지다(진단이 난 모듈의 항목은 S1005로 보지 않는다).
    bool JudgedModule(uint32 package, StringView name, Span<const uint8> unstable) const noexcept;
    // manifest를 읽어 활성 항목의 이름·alias를 names에 둔다. 형식 오류면 S1006을 내고 false.
    bool ReadIdManifest(uint32 package, JsonDocument& document,
                        Array<ManifestIdName>& names) noexcept;
    bool AddPendingId(uint8 kind, StringView name, StringView former) noexcept;

    // ---- typed IR 내림(IrDump.cpp, LC4 — Codegen 단계, 진단 없는 컴파일에서만) ----
    void LowerProgram() noexcept;
    // 모듈 interface의 정규 텍스트(SparksBytecode 6.2절 — interface hash의 입력)다.
    void AppendInterfaceText(StringBuilderBase& out, uint32 module) noexcept;
    void AppendInterfaceType(StringBuilderBase& out, uint32 module, uint32 type) noexcept;
    void LowerModule(uint32 module) noexcept;
    // 획득한 지역 자원 뒤의 정지하는 await(S0905 — 효과 분석 뒤).
    void CheckResourceWaits() noexcept;

    // ---- 흐름 분석(CheckFlow.cpp, 3.11절) ----
    // stale이면 await 뒤 객체 지역 이름의 읽기를 staleRead_에 표시하고(본문 검사 전), 아니면
    // S0710–S0712를 낸다(본문 검사 뒤).
    void AnalyzeFlow(const CheckContext& context, uint32 function, bool stale) noexcept;
    // owner class(와 기반 class)가 구현하는 interface에 그 이름의 메서드가 있는지다.
    bool ImplementsInterfaceMethod(uint32 owner, StringView name) const noexcept;
    // 쓰지 않는 import(S0714)를 알린다.
    void CheckUnusedImports(uint32 module) noexcept;
    // await 뒤에 읽은 객체 지역 이름에 `.`을 쓴 것(S0412)을 알린다.
    void ReportStaleRead(const CheckContext& context, uint32 objectNode) noexcept;

    // ---- 대입 경로(CheckAssignments.cpp, 3.11절) ----
    // 대입 대상·변경 메서드 수신자 경로를 검사한다(S0701–S0706·S0731, 위치는 reportNode).
    void CheckWritablePath(const CheckContext& context, uint32 path, uint32 reportNode) noexcept;
    // 부록 B의 값 타입 변경 메서드(Array.Add 등)인지다.
    bool IsMutatingMember(uint32 symbol) const noexcept;
    // 엔진 필드가 schema `access: readWrite`인지다.
    bool IsEngineFieldWritable(uint32 symbol) const noexcept;

    // 나누는 쪽이 값 0인 정수 상수(타입 없는 상수·`const`)면 S0504를 내고 true다(3.10절).
    bool ReportZeroDivisor(const CheckContext& context, const ExpressionValue& divisor,
                           uint32 node) noexcept;

    // 진단 문장의 타입 표기다(정규 이름 대신 선언 이름 — `Mode`, `Array<Box>?`).
    void AppendDisplayType(StringBuilderBase& out, uint32 type) const noexcept;

    // ---- 타입 관계(TypeRelations.cpp) ----
    NumericInfo Numeric(uint32 type) const noexcept;
    bool IsOptional(uint32 type) const noexcept;
    uint32 NonOptional(uint32 type) noexcept;
    uint32 MakeOptional(uint32 type) noexcept;
    uint32 SymbolOf(uint32 type) const noexcept;
    uint32 Argument(uint32 type, uint32 index) const noexcept;
    uint32 TypeOfSymbol(uint32 symbol) noexcept;
    bool IsReference(uint32 type) const noexcept;
    bool IsAssetType(uint32 type) const noexcept;
    bool IsEnumType(uint32 type) const noexcept;
    bool IsVector(uint32 type) const noexcept;
    bool IsLanguage(uint32 type, StringView name) const noexcept;
    bool IsDerived(uint32 type, uint32 base) const noexcept;
    bool IsAssignable(uint32 from, uint32 to) noexcept;
    bool IsEquatable(uint32 type) const noexcept;
    bool IsOrdered(uint32 type) const noexcept;
    bool HasDefault(uint32 type) const noexcept;

    // struct 기본값 판정 상태다(structDefault_ 값).
    static constexpr uint8 StructDefaultUnknown = 0;
    static constexpr uint8 StructDefaultComputing = 1;
    static constexpr uint8 StructDefaultPresent = 2;
    static constexpr uint8 StructDefaultMissing = 3;

    // 값 struct 순환(S0426)을 찾고 struct 기본값을 정한다(CheckStructs.cpp).
    // cycles=false: 표기한 필드만으로 기본값을 정한다(검사 전). cycles=true: 추론한 필드까지 넣어
    // 순환을 알린다(검사 뒤).
    void AnalyzeStructs(bool cycles) noexcept;
    // 필드·const 초기값을 이름 의존 순서대로 미리 정한다(재귀 깊이를 쓰지 않게 — S0427 앞 단계).
    void PrepareInitializers() noexcept;
    // 초기값 깊이를 하나 들어간다. 상한을 넘으면 S0427을 내고 false(그 초기값은 해석하지 못함).
    bool EnterInitializer(uint32 module, uint32 initializer) noexcept;
    bool IsEngineStruct(uint32 symbol) const noexcept;
    bool EngineStructHasDefault(uint32 symbol) noexcept;
    bool SparksStructHasDefault(uint32 symbol) const noexcept;
    bool IsInterpolatable(uint32 type) const noexcept;
    // 제네릭 치환: 주인 타입 인자(receiver)·함수 타입 인자·Math 자리표시(chosen)를 바꾼다.
    uint32 Substitute(uint32 type, uint32 receiver, Span<const uint32> functionArguments,
                      uint32 chosen) noexcept;
    bool ContainsPlaceholder(uint32 type) const noexcept;
    bool PlaceholderAccepts(uint32 placeholder, uint32 type) const noexcept;

    // ---- 식(CheckExpressions.cpp) ----
    ExpressionValue Check(const CheckContext& context, uint32 node, uint32 expected) noexcept;
    // 잎(리터럴·this)과 괄호다. 아래의 `...Value`·`Judge...`는 재귀가 끝난 뒤의 판정이고, 같은
    // 이름의 wrapper가 판정 값의 타입을 기록한다(재귀 경로의 frame을 작게 — Check 참고).
    ExpressionValue CheckLeaf(const CheckContext& context, uint32 node) noexcept;
    ExpressionValue CheckParenthesized(const CheckContext& context, uint32 node,
                                       uint32 expected) noexcept;
    ExpressionValue CheckIdentifier(const CheckContext& context, uint32 node) noexcept;
    ExpressionValue ValueOfSymbol(const CheckContext& context, uint32 symbol, uint32 receiver,
                                  bool instance) noexcept;
    ExpressionValue CheckBinary(const CheckContext& context, uint32 node) noexcept;
    ExpressionValue CheckLogical(const CheckContext& context, uint32 node) noexcept;
    ExpressionValue CombineLogical(const CheckContext& context, uint32 node,
                                   const ExpressionValue& left,
                                   const ExpressionValue& right) noexcept;
    ExpressionValue CombineLogicalValue(const CheckContext& context, uint32 node,
                                        const ExpressionValue& left,
                                        const ExpressionValue& right) noexcept;
    ExpressionValue CheckCoalesce(const CheckContext& context, uint32 node,
                                  uint32 expected) noexcept;
    ExpressionValue CheckCoalesceValue(const CheckContext& context, uint32 node,
                                       uint32 expected) noexcept;
    ExpressionValue CombineBinary(const CheckContext& context, uint32 node,
                                  const ExpressionValue& left,
                                  const ExpressionValue& right) noexcept;
    ExpressionValue CombineBinaryValue(const CheckContext& context, uint32 node,
                                       const ExpressionValue& left,
                                       const ExpressionValue& right) noexcept;
    ExpressionValue CheckUnary(const CheckContext& context, uint32 node, uint32 expected) noexcept;
    ExpressionValue CheckUnaryValue(const CheckContext& context, uint32 node,
                                    uint32 expected) noexcept;
    ExpressionValue CheckIndex(const CheckContext& context, uint32 node) noexcept;
    ExpressionValue IndexValue(const CheckContext& context, uint32 node,
                               const ExpressionValue& object) noexcept;
    ExpressionValue JudgeIndex(const CheckContext& context, uint32 node,
                               const ExpressionValue& object) noexcept;
    ExpressionValue CheckCollection(const CheckContext& context, uint32 node,
                                    uint32 expected) noexcept;
    ExpressionValue CheckCollectionValue(const CheckContext& context, uint32 node,
                                         uint32 expected) noexcept;
    ExpressionValue CheckString(const CheckContext& context, uint32 node) noexcept;
    ExpressionValue CheckCast(const CheckContext& context, uint32 node) noexcept;
    ExpressionValue CastOf(const CheckContext& context, uint32 node,
                           const ExpressionValue& operand) noexcept;
    ExpressionValue CastOfValue(const CheckContext& context, uint32 node,
                                const ExpressionValue& operand) noexcept;
    ExpressionValue CheckAwaitTry(const CheckContext& context, uint32 node) noexcept;
    ExpressionValue CheckAwaitTryValue(const CheckContext& context, uint32 node) noexcept;
    // 값을 type 자리에 맞춘다(대입 가능·상수 범위). 맞으면 그 타입, 아니면 진단을 내고 NoType.
    uint32 Coerce(const CheckContext& context, const ExpressionValue& value, uint32 type,
                  uint32 node) noexcept;
    // 자리 타입이 없을 때의 타입(상수는 int·float). 값이 아니면 진단을 내고 NoType.
    uint32 Materialize(const CheckContext& context, const ExpressionValue& value,
                       uint32 node) noexcept;
    // 값 하나를 요구하는 자리에서 값이 아닌 것(S0405·S0422)을 알린다. 값이면 true.
    bool RequireValue(const CheckContext& context, const ExpressionValue& value,
                      uint32 node) noexcept;
    bool ConstantFits(const ConstantValue& constant, bool integer, uint32 type) const noexcept;
    void Record(const CheckContext& context, uint32 node, uint32 type) noexcept;
    void Report(const CheckContext& context, DiagnosticCode code, uint32 node, StringView message,
                StringView explanation, StringView fix) noexcept;
    void ReportAt(const CheckContext& context, DiagnosticCode code, SourceSpan span,
                  StringView message, StringView explanation, StringView fix) noexcept;
    void ReportMismatch(const CheckContext& context, uint32 node, uint32 from, uint32 to) noexcept;

    // ---- 멤버·호출(CheckCalls.cpp) ----
    ExpressionValue CheckMember(const CheckContext& context, uint32 node) noexcept;
    // 받는 값을 검사한 뒤의 멤버 판정이다(재귀 경로 밖 — CheckBinary 참고).
    ExpressionValue MemberOf(const CheckContext& context, uint32 node,
                             const ExpressionValue& receiver) noexcept;
    ExpressionValue MemberOfValue(const CheckContext& context, uint32 node,
                                  const ExpressionValue& receiver) noexcept;
    ExpressionValue CheckCall(const CheckContext& context, uint32 node, uint32 expected) noexcept;
    // 부르는 값을 검사한 뒤의 호출 판정이다(재귀 경로 밖 — CheckBinary 참고).
    ExpressionValue CallValue(const CheckContext& context, uint32 node, uint32 expected,
                              const ExpressionValue& callee) noexcept;
    ExpressionValue JudgeCall(const CheckContext& context, uint32 node, uint32 expected,
                              const ExpressionValue& callee) noexcept;
    ExpressionValue CallFunction(const CheckContext& context, uint32 node,
                                 const ExpressionValue& callee, uint32 typeArguments) noexcept;
    ExpressionValue Construct(const CheckContext& context, uint32 node, uint32 typeSymbol) noexcept;
    ExpressionValue Convert(const CheckContext& context, uint32 node, uint32 typeSymbol) noexcept;
    ExpressionValue CheckLiteral(const CheckContext& context, uint32 node, uint32 literal,
                                 uint32 expected) noexcept;
    // 타입의 멤버(자기·기반 체인, 언어 멤버)를 찾는다.
    uint32 FindMember(uint32 type, StringView name) const noexcept;
    bool MemberAvailable(uint32 member, uint32 receiver) const noexcept;

    // ---- 문장(CheckStatements.cpp) ----
    void CheckModule(uint32 module) noexcept;
    void CheckFunction(const CheckContext& outer, uint32 function, uint32 symbol) noexcept;
    void CheckStatement(const CheckContext& context, uint32 node) noexcept;
    void CheckSwitch(const CheckContext& context, uint32 node) noexcept;
    // switch의 대상·case 값(대상 타입을 돌려준다, case 상수는 seen에)과 빠진 case·default 판정이다.
    uint32 CheckSwitchValues(const CheckContext& context, uint32 node,
                             Array<ConstantValue>& seen) noexcept;
    void CheckSwitchCoverage(const CheckContext& context, uint32 node, uint32 type,
                             Span<const ConstantValue> seen) noexcept;
    // 문장 재귀(CheckStatement)의 frame을 작게 두려고 나눈 머리·단순 문장 검사다.
    void CheckCondition(const CheckContext& context, uint32 expression) noexcept;
    void CheckIfHead(const CheckContext& context, uint32 node) noexcept;
    void CheckForHead(const CheckContext& context, uint32 node) noexcept;
    void CheckSimpleStatement(const CheckContext& context, uint32 node) noexcept;
    void CheckReturn(const CheckContext& context, uint32 node) noexcept;
    // `x op= v`(x의 읽기 타입 readType)를 `x = x op v`의 연산 규칙으로 검사한다(3.10절 "연산자").
    void CheckCompoundAssignment(const CheckContext& context, uint32 node, StringView op,
                                 uint32 readType, const ExpressionValue& value,
                                 uint32 valueNode) noexcept;
    bool Terminates(uint32 module, uint32 node) const noexcept;
    uint32 FieldType(uint32 field) noexcept;
    ConstantValue ConstOf(uint32 symbol) noexcept;

    NameBinder& binder_;
    Compilation& compilation_;
    Allocator& allocator_;
    // 언어 타입 심볼(정규 이름)이다.
    uint32 boolType_ = NoType;
    uint32 intType_ = NoType;
    uint32 floatType_ = NoType;
    uint32 stringType_ = NoType;
    uint32 voidType_ = NoType;
    uint32 errorEnum_ = NoSymbol; // 엔진 `Error`
    // 필드·const 타입 추론의 진행 표시(심볼 번호 → 0 미정, 1 진행 중, 2 끝)다.
    Array<uint8> inference_;
    // const 심볼의 값이다(심볼 번호).
    Array<ConstantValue> constants_;
    // 모든 case를 다룬 enum switch((모듈 << 32) | 노드)다(끝나는 문장 판정).
    Array<uint64> exhaustiveSwitches_;
    // struct 기본값 판정(심볼 번호 → StructDefault*)이다. AnalyzeStructs가 채운다.
    Array<uint8> structDefault_;
    // enum case 목록(선언 순): enum 심볼 → 첫 case, case → 다음 case(NoSymbol 끝)이다.
    Array<uint32> firstCase_;
    Array<uint32> nextCase_;
    // 지금 정하고 있는 초기값의 깊이다(S0427).
    uint32 initializerDepth_ = 0;
    // 흐름·대입·미사용·상수식·필드·프로필 검사(LC2d, 3.11절)를 하는지다(단계가 Flow 이상).
    bool flow_ = false;
    // staleModule_ 모듈의 노드마다 await 뒤 상태로 읽는 지역 이름인지다(3.11절 — 1이면 객체 타입의
    // 읽기를 `T?`로 본다).
    Array<uint8> staleRead_;
    uint32 staleModule_ = NoModule;
    // 문맥·수명 규칙(LC3, 3.12절)을 검사하는지다(단계가 Rules 이상).
    bool rules_ = false;
    // 확장 이벤트 심볼(심볼 번호 순)이다.
    Array<uint32> extensionEvents_;
    // 효과 분석(3.13절)의 본문·자리와 함수 심볼 → 본문 번호(NoLocalIndex 없음)다.
    Array<EffectBody> effectBodies_;
    Array<EffectSite> effectSites_;
    Array<uint32> effectOfSymbol_;
    // 디스패치 표: (기반·interface 함수 심볼 << 32 | 본문) 쌍과, 심볼 → dispatchBodies_ 구간.
    Array<uint64> dispatchPairs_;
    // S0905를 볼 지역 자원 선언·`if (let)`((모듈 << 32) | 노드)이다.
    Array<uint64> resourceWaits_;
    // 권한 이벤트(`@authorityOnly`·server 발행) handler((모듈 << 32) | 노드)다(3.15절).
    Array<uint64> authorityHandlers_;
    // IsStorableType 기억 표(타입 × 객체 허용 여부 → StorableState).
    Array<uint8> storable_;
    Array<uint32> dispatchFirst_;
    Array<uint32> dispatchBodies_;
};

} // namespace rocks::sparks::detail
