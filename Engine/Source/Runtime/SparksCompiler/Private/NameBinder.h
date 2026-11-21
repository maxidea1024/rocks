#pragma once

#include <Core/Containers/Array.h>
#include <Core/Containers/HashMap.h>
#include <Core/Hash.h>
#include <Core/Result.h>
#include <Core/Text/StringBuilder.h>
#include <SparksCompiler/Compilation.h>

#include "PreludeMembers.h"

// 이름 해석(LC2b)이다(Docs/Design/SparksLanguage.ko.md 3.9절, S0306–S0325, schema S1120의 이름
// 부분). Compilation::Check가 import 해석 뒤에 부른다. 기능별 파일로 나뉜다:
//   BindSetup.cpp         진단·심볼·이름 공간·타입 intern, 언어 이름, 엔진 schema 선언과 해석
//   BindDeclarations.cpp  모듈 선언·import 이름·선언 타입·기반·상속·멤버 검사·override·interface·
//                         services
//   BindBodies.cpp        함수·handler·초기값 본문의 지역 이름과 단순 이름
//   NameDump.cpp          타입 표기·이름 해석 덤프(Compilation 쪽)

namespace rocks::sparks::detail {

// 이름 공간의 종류다. 열쇠는 (종류 << 32 | 번호)다.
enum class NameSpaceKind : uint32 {
    Language,     // 언어 이름·문맥 이름·RPC 접두(번호 0)
    Engine,       // 엔진 타입(번호 0)
    ModuleTop,    // 모듈 최상위 선언(번호 = 모듈)
    ModuleImport, // 모듈이 import로 도입한 이름(번호 = 모듈)
    Members,      // 타입 멤버(번호 = 타입 심볼 — class·struct·enum·interface·엔진 타입)
    Parameters    // 매개변수 목록 하나(번호 = 그 선언의 심볼)
};

// 둘 이상의 모듈이 선언한 이름의 표시다(packageNames_ — import 제안을 하지 않는다).
inline constexpr uint32 AmbiguousSymbol = NoSymbol - 1;

// 중복을 보지 않는 선언(CheckDeclarationName)이다.
inline constexpr uint64 NoSpace = NumericLimits<uint64>::Max();

constexpr uint64 SpaceKey(NameSpaceKind kind, uint32 id) noexcept {
    constexpr uint32 KindShift = 32;
    return (static_cast<uint64>(kind) << KindShift) | id;
}

struct NameKey {
    uint64 space = 0;
    StringView name;
};

struct NameKeyHash {
    uint64 operator()(const NameKey& key) const noexcept {
        return HashCombine(MixHash(key.space), HashStringBytes(key.name));
    }
};

struct NameKeyEqual {
    bool operator()(const NameKey& left, const NameKey& right) const noexcept {
        return left.space == right.space && left.name == right.name;
    }
};

// ASCII 대소문자를 무시하는 열쇠 비교다(중복·값 이름 충돌).
struct FoldedKeyHash {
    uint64 operator()(const NameKey& key) const noexcept {
        return HashCombine(MixHash(key.space), HashStringBytesIgnoringAsciiCase(key.name));
    }
};

struct FoldedKeyEqual {
    bool operator()(const NameKey& left, const NameKey& right) const noexcept {
        return left.space == right.space && EqualsIgnoringAsciiCase(left.name, right.name);
    }
};

struct FoldedNameHash {
    uint64 operator()(StringView name) const noexcept {
        return HashStringBytesIgnoringAsciiCase(name);
    }
};

struct FoldedNameEqual {
    bool operator()(StringView left, StringView right) const noexcept {
        return EqualsIgnoringAsciiCase(left, right);
    }
};

// 문맥 이름 bit다(LanguageNames.h ContextNames 순서).
struct ContextBits {
    static constexpr uint32 World = 1u << 0;
    static constexpr uint32 Session = 1u << 1;
    static constexpr uint32 Input = 1u << 2;
    static constexpr uint32 Time = 1u << 3;
    static constexpr uint32 Caller = 1u << 4;
};

// 본문 하나를 해석하는 자리다.
struct BodyContext {
    uint32 module = NoModule;
    uint32 owner = NoSymbol;   // class·struct 심볼(최상위 fn·const는 NoSymbol)
    uint32 contextNames = 0;   // 쓸 수 있는 문맥 이름 bit
    bool hasLocals = false;    // 함수·handler 본문인지(필드·const 초기값은 아니다)
    bool reportUnknown = true; // S0317·S0318을 내는지(3.9절 연쇄 방지)
    bool reportContext = true; // S0321을 내는지(기반을 모르는 class는 아니다)
};

class FlowAnalyzer;
class IrBuilder;
class TypeChecker;

class NameBinder final {
public:
    NameBinder(Compilation& compilation, DiagnosticList& diagnostics) noexcept;

    Result<void> Run() noexcept;

private:
    friend class FlowAnalyzer;
    friend class IrBuilder;
    friend class TypeChecker;

    // ---- 공통(BindSetup.cpp) ----
    void Report(DiagnosticCode code, SourceId source, SourceSpan span, StringView message,
                StringView explanation, StringView fix) noexcept;
    bool Succeeded(const StringBuilderBase& builder) noexcept;
    uint32 AddSymbol(const Symbol& symbol) noexcept;
    // space에 이름을 더한다(정확한 이름·대소문자를 접은 이름 모두 처음 것이 남는다).
    void InsertName(uint64 space, StringView name, uint32 symbol) noexcept;
    uint32 FindName(uint64 space, StringView name) const noexcept;
    uint32 FindFolded(uint64 space, StringView name) const noexcept;
    uint32 InternType(uint32 symbol, Span<const uint32> arguments, bool optional) noexcept;
    uint32 AddSignature(uint32 returnType, Span<const uint32> parameters, bool isAsync) noexcept;
    // 모듈 구문 트리의 이름 노드 글자다.
    StringView NodeText(uint32 module, uint32 node) const noexcept;
    SourceSpan NodeSpan(uint32 module, uint32 node) const noexcept;
    void SetNodeSymbol(uint32 module, uint32 node, uint32 symbol) noexcept;
    void SetNodeType(uint32 module, uint32 node, uint32 type) noexcept;

    void DeclareLanguageNames() noexcept;
    void DeclareEngine() noexcept;
    void ResolveEngine() noexcept;

    // ---- 부록 B 멤버(BindPrelude.cpp) ----
    struct PreludeScope {
        uint32 owner = NoSymbol;    // 주인 타입(타입 인자 `T` 등을 찾는다)
        uint32 function = NoSymbol; // 함수 자기 타입 매개변수(`<T>`)
        uint32 vector = NoSymbol;   // 벡터 공통 멤버의 `$V`
    };

    void DeclarePreludeMembers() noexcept;
    void DeclarePreludeMember(const PreludeMember& member, uint32 index, uint32 owner,
                              uint32 vector) noexcept;
    uint32 ParsePreludeType(StringView text, usize& index, const PreludeScope& scope) noexcept;
    uint32 ParsePreludeSignature(StringView text, PreludeScope scope, uint32& flags) noexcept;
    uint32 Malformed() noexcept;
    uint32 MalformedSignature() noexcept;
    // schema 타입 표기를 해석한다. 못 하면 S1120을 내고 NoType.
    uint32 ResolveNotation(uint32 schema, uint32 function, StringView text,
                           SourceSpan span) noexcept;
    uint32 ResolveNotationAt(uint32 schema, uint32 function, StringView text, usize& index,
                             SourceSpan span, bool& failed) noexcept;
    void ReportSchema(uint32 schema, SourceSpan span, StringView message) noexcept;
    // 엔진 기반 체인이 그 핵심 엔진 타입을 지나는지다.
    bool EngineChainHas(uint32 type, uint32 target) const noexcept;

    // ---- 선언(BindDeclarations.cpp) ----
    void CollectModuleStates() noexcept;
    void DeclareModule(uint32 module) noexcept;
    void DeclareMembers(uint32 module, uint32 owner, uint32 list) noexcept;
    void DeclareImports(uint32 module) noexcept;
    void ResolveModuleBases(uint32 module) noexcept;
    void ResolveDeclarationTypes(uint32 module) noexcept;
    uint32 ResolveSignature(const BodyContext& context, uint32 symbol, uint32 parameters,
                            uint32 returnType, bool isAsync) noexcept;
    void DeclareParameters(const BodyContext& context, uint32 owner, uint32 list) noexcept;
    void ResolveBases(uint32 module, uint32 classSymbol, uint32 list) noexcept;
    void CheckInheritance() noexcept;
    void CheckMembers(uint32 module) noexcept;
    void CheckMember(uint32 module, uint32 owner, uint32 member) noexcept;
    void CheckOverride(uint32 module, uint32 owner, uint32 member) noexcept;
    void CheckInterfaces(uint32 classSymbol) noexcept;
    void CheckInheritedNames(uint32 module) noexcept;
    void CheckServices() noexcept;
    // 기반 체인(자기 제외)에서 이름이 같은 멤버다. folded면 Sparks 멤버를 대소문자 무시로 찾는다.
    uint32 FindInherited(uint32 owner, StringView name, bool foldSparks) const noexcept;
    // 타입 심볼(자기 포함)과 기반 체인에서 멤버를 정확한 이름으로 찾는다.
    uint32 FindMemberInChain(uint32 type, StringView name) const noexcept;
    bool SameSignature(uint32 left, uint32 right) const noexcept;
    bool IsFunctionSymbol(uint32 symbol) const noexcept;
    uint32 ContextNamesOf(uint32 classSymbol) const noexcept;

    // 선언 이름 검사 1–4단계다(3.9절). 진단을 냈으면 false. space는 중복을 보는 선언 공간이다.
    enum class NameRule : uint8 {
        Pascal,
        Camel
    };

    bool CheckDeclarationName(uint32 module, uint32 symbol, NameRule rule, StringView what,
                              bool lifecycleMember, uint64 space) noexcept;
    void ReportNameDiagnostic(DiagnosticCode code, uint32 symbol, StringView message,
                              StringView explanation, StringView fix) noexcept;

    // ---- 이름 찾기(BindBodies.cpp) ----
    // 식의 단순 이름을 찾는다. 문맥 이름을 못 쓰는 자리면 S0321, 못 찾으면 S0317을 낸다.
    uint32 ResolveSimpleName(const BodyContext& context, uint32 node, StringView name,
                             SourceSpan span) noexcept;
    // 진단 없이 찾는다(보이는 이름 전부 — 3.9절 "찾기"). 쓸 수 없는 자리의 문맥 이름이고 앞선
    // 대상이 없으면 NoSymbol이며 unavailableContext(있으면)를 참으로 둔다.
    uint32 LookupValue(const BodyContext& context, StringView name,
                       bool* unavailableContext = nullptr) const noexcept;
    class Pick;
    // 타입 이름 공간에서 찾는다(모듈 최상위·import·엔진·언어).
    uint32 LookupType(uint32 module, StringView name) const noexcept;
    uint32 ResolveTypeNode(const BodyContext& context, uint32 typeNode) noexcept;
    // Path 노드를 타입 심볼로 해석한다(못 하면 진단을 내고 NoSymbol). recordNode면 그 Path의
    // 타입도 기록한다(기반 목록·`as?`).
    uint32 ResolveTypePath(const BodyContext& context, uint32 path) noexcept;
    // 경로를 찾기만 한다(ResolveTypePath가 그 결과로 import 사용·찾지 못함을 기록한다).
    uint32 LookupTypePath(const BodyContext& context, uint32 path) noexcept;
    // module에서 이름으로 찾은 심볼이 import한 모듈의 최상위 선언·별칭이면 그 import를 쓴 것으로
    // 표시한다(3.11절 S0714).
    void NoteUse(uint32 module, uint32 symbol) noexcept;
    bool IsTypeSymbol(uint32 symbol) const noexcept;
    void ReportUnknown(const BodyContext& context, bool type, StringView name,
                       SourceSpan span) noexcept;
    void BindBodies(uint32 module) noexcept;
    void BindFunctionBody(const BodyContext& context, uint32 function) noexcept;
    void BindStatement(const BodyContext& context, uint32 node) noexcept;
    void BindExpression(const BodyContext& context, uint32 node) noexcept;
    void BindTypeArguments(const BodyContext& context, uint32 list) noexcept;
    void BindEventPath(const BodyContext& context, uint32 path) noexcept;
    void PushScope() noexcept;
    void PopScope() noexcept;
    // 지역 이름(매개변수·지역 변수·binder)을 검사하고 현재 범위에 더한다.
    uint32 DeclareLocal(const BodyContext& context, SymbolKind kind, uint32 nameNode,
                        uint32 declaration, uint32 type, bool isLet) noexcept;
    uint32 FindLocal(StringView name) const noexcept;

    struct ModuleState {
        bool syntaxErrors = false; // S01xx·S02xx가 있다
        bool tooDeep = false;      // S0215 — 뒤 단계로 넘기지 않는다(8.1절 "깊이 상한")
        bool incomplete = false;   // 3.9절 "연쇄 방지"의 모듈 조건
    };

    struct LocalEntry {
        uint32 symbol = NoSymbol;
        uint32 previous = NoSymbol; // 같은 접은 이름의 이전 항목
    };

    Compilation& compilation_;
    DiagnosticList& diagnostics_;
    Allocator& allocator_;
    HashMap<NameKey, uint32, NameKeyHash, NameKeyEqual> names_;
    HashMap<NameKey, uint32, FoldedKeyHash, FoldedKeyEqual> foldedNames_;
    Array<ModuleState> moduleStates_;
    // 패키지마다 최상위 선언 이름 → 심볼(같은 이름이 둘 이상이면 NoSymbol — import 제안용)이다.
    HashMap<NameKey, uint32, NameKeyHash, NameKeyEqual> packageNames_;
    Array<LocalEntry> locals_;
    Array<uint32> scopeMarks_;
    HashMap<StringView, uint32, FoldedNameHash, FoldedNameEqual> localIndex_;
    // import마다(Compilation::imports_ 번호) 그 import의 선언을 이름으로 썼는지, 모듈마다
    // 이름·타입을 찾지 못했는지다(3.11절 S0714와 그 연쇄 방지).
    Array<uint8> importUsed_;
    Array<uint8> unresolvedNames_;
    // 핵심 엔진 타입 심볼(없으면 NoSymbol)이다.
    uint32 actor_ = NoSymbol;
    uint32 component_ = NoSymbol;
    uint32 widgetController_ = NoSymbol;
    uint32 worldService_ = NoSymbol;
    uint32 voidType_ = NoType;
    // 부록 B 자리표시($N·$S·$I·$F·$V 순)와 문맥 이름 Time의 타입 심볼이다.
    static constexpr usize PlaceholderCount = 5;
    uint32 placeholders_[PlaceholderCount] = {NoSymbol, NoSymbol, NoSymbol, NoSymbol, NoSymbol};
    uint32 timeType_ = NoSymbol;
    uint32 firstContextSymbol_ = NoSymbol; // ContextNames 순서로 이어진다
    ErrorCode error_ = ErrorCode::None;
};

} // namespace rocks::sparks::detail
