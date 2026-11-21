#pragma once

#include <Base/NumericLimits.h>
#include <Core/Containers/Array.h>
#include <Core/Containers/HashSet.h>
#include <Core/Containers/Span.h>
#include <Core/Memory/Allocator.h>
#include <Core/Result.h>
#include <Core/Text/StringBuilder.h>
#include <Core/Text/StringView.h>
#include <SparksCompiler/BindingSchema.h>
#include <SparksCompiler/Diagnostic.h>
#include <SparksCompiler/PackageManifest.h>
#include <SparksCompiler/Source.h>
#include <SparksCompiler/Symbols.h>
#include <SparksCompiler/SyntaxTree.h>

// Sparks 컴파일 단위다(Docs/Design/SparksLanguage.ko.md 3.4절 패키지·모듈·import, 10.2절 binding
// schema, 18.2절 LC2a). 한 번의 컴파일에 함께 넣는 엔진 binding schema 파일과 소스
// 패키지(manifest와 모듈 파일)를 받아 검사한다.
//
// 사용 예:
//   Compilation compilation(allocator, sources);
//   compilation.AddBindingSchema(engineSchemaId);
//   auto game = compilation.AddPackage(manifestId);
//   compilation.AddModule(game.Value(), doorId, "Props/Door.spark");
//   auto checked = compilation.Check(diagnostics);   // 진단은 diagnostics에
//
// 검사 순서: binding schema → manifest → 패키지 이름 중복 → 패키지마다 모듈 경로·의존·최상위 이름 →
// 패키지 의존 순환 → manifest가 맞는 패키지의 모듈 구문 분석 → import 해석(LC2a) → 이름 해석(LC2b,
// 3.9절: 언어·엔진 이름 → 모듈 선언 → import 이름 → 선언 타입·기반 → 상속 → 멤버 검사 → 본문 →
// services). 뒤 마일스톤이 타입 검사를 더한다.
//
// 수명·스레드: 원문(SourceSet)은 Compilation보다 오래 산다. 결과(트리·해석)는 Compilation이 가진다.
// 한 객체는 한 스레드에서 쓴다.

namespace rocks::sparks {

// Check가 어디까지 검사하는지다(corpus `.expect`의 `stage` — 13.1절). 뒤 단계는 앞 단계를 포함한다.
enum class CompilationStage : uint8 {
    Syntax,  // schema·manifest·모듈 경로·의존·어휘·구문 분석까지
    Imports, // 위에 더해 import 해석(LC2a)
    Names,   // 위에 더해 이름 해석(LC2b)
    Types,   // 위에 더해 타입 검사(LC2c)
    Flow,    // 위에 더해 흐름·대입·미사용·상수식·필드·프로필 검사(LC2d)
    Rules,   // 위에 더해 문맥·수명 규칙(이벤트·hook·효과·자원·권한·저장 — LC3)
    All = Rules
};

// 모듈·패키지 번호가 없음을 나타낸다.
inline constexpr uint32 NoModule = NumericLimits<uint32>::Max();
inline constexpr uint32 NoPackage = NumericLimits<uint32>::Max();

// import 하나의 해석 결과다.
struct ImportResolution {
    uint32 node = NoNode;     // 그 모듈 구문 트리의 Import 노드
    uint32 module = NoModule; // 대상 모듈. 해석하지 못했으면(진단을 냈거나 판정하지 않음) NoModule
    // 경로가 가리키는 모듈이다. 해석한 import와 같은 모듈의 거듭된 import(S0303)·자기
    // import(S0304)는 그 모듈이고, 경로를 해석하지 못했거나 판정하지 않았으면 NoModule이다(3.9절
    // 연쇄 방지).
    uint32 target = NoModule;
};

namespace detail {
class CompilationChecker;
class FlowAnalyzer;
class NameBinder;
class TypeChecker;
} // namespace detail

class Compilation final {
public:
    Compilation(Allocator& allocator, const SourceSet& sources) noexcept;

    Compilation(const Compilation&) = delete;
    Compilation& operator=(const Compilation&) = delete;
    ~Compilation() noexcept = default;

    // 엔진 binding schema 파일 하나를 더한다. 실패: Check 뒤면 InvalidArgument, 할당 실패.
    Result<void> AddBindingSchema(SourceId id) noexcept;

    // 소스 패키지 하나(manifest 파일)를 더하고 패키지 번호를 돌려준다. 실패: Check 뒤면
    // InvalidArgument, 할당 실패.
    Result<uint32> AddPackage(SourceId manifest) noexcept;

    // package에 모듈 파일 하나를 더하고 모듈 번호를 돌려준다. path는 패키지 루트 기준 경로(`/`
    // 구분, `Combat/Score.spark`)이며 규칙 검사는 Check가 한다(S1108). 실패: 없는 package, 같은
    // 패키지에 같은 path를 두 번, Check 뒤면 InvalidArgument, 할당 실패.
    Result<uint32> AddModule(uint32 package, SourceId source, StringView path) noexcept;

    // Check가 검사할 마지막 단계를 정한다(기본 CompilationStage::All). 뒤 단계의 결과 접근은 빈
    // 결과다(예: Syntax면 ModuleImports가 비고 NodeSymbol이 NoSymbol). 실패: Check 뒤면
    // InvalidArgument.
    Result<void> SetLastStage(CompilationStage stage) noexcept;

    // 더한 입력을 검사한다(한 번만). 진단은 diagnostics에 붙인다. 실패: 두 번째 호출은
    // InvalidArgument, 할당 실패는 그 오류(결과·진단은 정해지지 않는다).
    Result<void> Check(DiagnosticList& diagnostics) noexcept;

    // ---- 결과(Check 뒤) ----

    uint32 SchemaCount() const noexcept {
        return static_cast<uint32>(schemas_.Size());
    }

    // schema 파일을 형식 오류 없이 읽었는지다.
    bool SchemaIsValid(uint32 schema) const noexcept;
    const BindingSchema& Schema(uint32 schema) const noexcept;

    uint32 PackageCount() const noexcept {
        return static_cast<uint32>(packages_.Size());
    }

    // manifest를 오류 없이 읽었고 이름이 겹치지 않는지다(아니면 그 패키지의 모듈은 분석하지
    // 않는다).
    bool PackageIsValid(uint32 package) const noexcept;
    // manifest다. 무효인 패키지도 `name` 값 자체가 맞으면 Name()은 그 이름이다(나머지 값은 정해지지
    // 않는다).
    const PackageManifest& Manifest(uint32 package) const noexcept;
    // 패키지 이름으로 찾는다. 같은 이름(대소문자만 다른 것 포함)이면 처음 더한 패키지가 이름을
    // 가지므로 그 패키지가 유효할 때만 돌려준다. 없으면 NoPackage.
    uint32 FindPackage(StringView name) const noexcept;

    uint32 ModuleCount() const noexcept {
        return static_cast<uint32>(modules_.Size());
    }

    uint32 ModulePackage(uint32 module) const noexcept;
    SourceId ModuleSource(uint32 module) const noexcept;
    // AddModule에 준 경로다.
    StringView ModulePath(uint32 module) const noexcept;
    // import 경로(`Combat.Score`)다. 경로가 규칙에 맞지 않으면 빈 문자열이다.
    StringView ModuleImportPath(uint32 module) const noexcept;
    // 구문 트리다. 분석하지 않았으면(manifest 오류, 원문 검사 실패) nullptr.
    const SyntaxTree* ModuleTree(uint32 module) const noexcept;
    // 모듈의 import마다 해석 결과다(트리의 import 순서).
    Span<const ImportResolution> ModuleImports(uint32 module) const noexcept;
    // package 안에서 import 경로로 모듈을 찾는다(경로가 맞는 모듈만). 없으면 NoModule.
    uint32 FindModule(uint32 package, StringView importPath) const noexcept;

    // ---- 이름 해석 결과(Check 뒤, LC2b — 3.9절) ----

    // 심볼 전부다(번호는 결정적이다 — 언어 이름 → 엔진 → 모듈 순).
    Span<const Symbol> Symbols() const noexcept {
        return symbols_.View();
    }

    // 타입 표다(0번은 NoType). 타입 인자는 TypeArguments의 구간이다.
    Span<const TypeEntry> Types() const noexcept {
        return types_.View();
    }

    Span<const uint32> TypeArguments() const noexcept {
        return typeArguments_.View();
    }

    Span<const Signature> Signatures() const noexcept {
        return signatures_.View();
    }

    Span<const uint32> SignatureParameters() const noexcept {
        return signatureParameters_.View();
    }

    // class의 해석한 기반들이다(Symbol::firstBase·baseCount 구간).
    Span<const uint32> SymbolBases() const noexcept {
        return symbolBases_.View();
    }

    // 모듈 구문 트리 노드의 심볼이다: 선언 이름 노드는 그 선언, 식의 Identifier와 on·emit 경로의
    // 첫 Name은 가리키는 대상. 없거나 해석하지 못했으면 NoSymbol.
    uint32 NodeSymbol(uint32 module, uint32 node) const noexcept;

    // Type 노드와 기반 목록·`as?`의 Path 노드가 나타내는 타입이다. 없거나 해석하지 못했으면 NoType.
    uint32 NodeType(uint32 module, uint32 node) const noexcept;

    // 식 노드의 타입이다(LC2c — 기대 타입에 맞춘 뒤의 타입, 상수는 그 자리의 타입). 값이 아닌 식
    // (타입 이름·void·대기 등)이거나 해석하지 못했으면 NoType.
    uint32 ExpressionType(uint32 module, uint32 node) const noexcept;

    // 타입의 정규 표기를 붙인다: 내장 타입은 정규 이름(`int64` → `int`), prelude는 그 이름, 엔진
    // 타입은 `Rocks.이름`, 사용자 타입은 `패키지:모듈.이름`, 타입 매개변수는 그 이름, 제네릭은
    // `Map<string, int>`, 선택은 끝의 `?`, NoType은 `?`. 실패: 할당 실패.
    Result<void> AppendTypeText(StringBuilderBase& out, uint32 type) const noexcept;

    // 모듈 하나의 이름 해석 덤프를 붙인다(SD58, corpus `.names` golden). 형식: 한 줄(LF로 끝)에 한
    // 자리, 원문 위치(시작 byte) 순서(같으면 노드 번호 순):
    //   <줄>:<열> decl <종류> <정규 이름>          선언(최상위·멤버·case·별칭·매개변수·지역 이름)
    //   <줄>:<열> <글자> -> <대상>                  식의 이름, on·emit 경로의 첫 이름
    //   <줄>:<열> type <정규 타입>                  Type 노드, 기반 목록·`as?`의 Path
    // 종류는 class·struct·enum·interface·const·fn·field·method·event·case·imethod·param·local·
    // alias다. 정규 이름은 `패키지:모듈.이름`(멤버·case는 `패키지:모듈.타입.이름`), 매개변수·지역
    // 이름은 `이름@줄:열`, 별칭은 `이름=패키지:모듈`이다. 대상은 "<종류> <정규 이름>"이며 그 밖의
    // 종류는 builtin·prelude·context·rpc(이름), engine-type(`Rocks.이름`)·engine-field·engine-fn·
    // engine-event(`Rocks.타입.이름`)다. 해석하지 못한 이름은 `?`다. 결정적이다.
    // 실패: 분석하지 않은 모듈(ModuleTree가 nullptr)은 InvalidArgument, 할당 실패는 그 오류.
    Result<void> AppendNameDump(StringBuilderBase& out, uint32 module) const noexcept;

    // 모듈 하나의 식 타입 덤프를 붙인다(SD58, corpus `.types` golden, LC2c). 형식: 한 줄(LF로 끝)에
    // 식 하나, 시작이 앞선 식 먼저(같으면 끝이 먼 바깥 식 먼저, 그래도 같으면 노드 번호 순):
    //   <줄>:<열>-<줄>:<열> <노드 종류> <정규 타입>
    // 범위는 `.ast`와 같은 노드 범위(끝은 마지막 글자 다음)다.
    // 노드 종류는 SyntaxKindName, 타입은 AppendTypeText 표기다(기대 타입에 맞춘 뒤의 타입 — 상수는
    // 그 자리의 타입). 상수로 접힌 식(`1 + 2`)은 바깥 식 하나만 적고 안쪽 상수 피연산자는 적지
    // 않는다. 값이 아닌 식(타입 이름·이름공간·void·대기)과 오류 난 식도 적지 않는다. 결정적이다.
    // 실패: 타입 검사를 하지 않은 모듈(단계가 Types보다 앞이거나 이름 해석 전에 멈춘 모듈)은
    // InvalidArgument, 할당 실패는 그 오류.
    Result<void> AppendTypeDump(StringBuilderBase& out, uint32 module) const noexcept;

private:
    friend class detail::CompilationChecker;
    friend class detail::FlowAnalyzer;
    friend class detail::NameBinder;
    friend class detail::TypeChecker;

    // 타입 intern 색인이다. 열쇠는 타입 번호이고 해시·비교는 그 타입의 심볼·인자·선택 여부로 한다.
    struct TypeProbe {
        uint32 symbol = NoSymbol;
        Span<const uint32> arguments;
        bool optional = false;
    };

    struct TypeHash {
        const Compilation* owner = nullptr;
        uint64 operator()(uint32 type) const noexcept;
        uint64 operator()(const TypeProbe& probe) const noexcept;
    };

    struct TypeEqual {
        const Compilation* owner = nullptr;
        bool operator()(uint32 left, uint32 right) const noexcept;
        bool operator()(uint32 left, const TypeProbe& right) const noexcept;
    };

    TypeProbe ProbeOf(uint32 type) const noexcept;
    Result<void> AppendSymbolName(StringBuilderBase& out, uint32 symbol) const noexcept;

    // 이름이 정확히 name인 패키지 중 처음 더한 것이다(유효 여부와 무관, Check 뒤). 없으면
    // NoPackage.
    uint32 FindNamedPackage(StringView name) const noexcept;

    // AddModule의 (패키지, 경로) 중복 색인이다. 열쇠는 모듈 번호이고 해시·비교는 그 모듈의 패키지와
    // 경로 글자로 한다(경로 글자는 커지는 text_에 있으므로 열쇠에 포인터를 두지 않는다).
    struct ModulePathProbe {
        uint32 package = 0;
        StringView path;
    };

    struct ModulePathHash {
        const Compilation* owner = nullptr;
        uint64 operator()(uint32 module) const noexcept;
        uint64 operator()(const ModulePathProbe& probe) const noexcept;
    };

    struct ModulePathEqual {
        const Compilation* owner = nullptr;
        bool operator()(uint32 left, uint32 right) const noexcept;
        bool operator()(uint32 left, const ModulePathProbe& right) const noexcept;
    };

    struct TextRef {
        uint32 offset = 0;
        uint32 length = 0;
    };

    struct SchemaEntry {
        BindingSchema schema;
        SourceId source;
        bool valid = false;
    };

    struct PackageEntry {
        PackageManifest manifest;
        SourceId source;
        bool valid = false;
    };

    struct ModuleEntry {
        uint32 package = NoPackage;
        SourceId source;
        TextRef path;
        TextRef importPath;
        bool validPath = false;
        bool parsed = false;
        uint32 firstImport = 0;
        uint32 importCount = 0;
    };

    StringView Text(TextRef text) const noexcept {
        return StringView(text_.Data() + text.offset, text.length);
    }

    Allocator* allocator_;
    const SourceSet* sources_;
    Array<char> text_;
    Array<SchemaEntry> schemas_;
    Array<PackageEntry> packages_;
    Array<ModuleEntry> modules_;
    Array<SyntaxTree> trees_; // modules_와 같은 번호
    Array<ImportResolution> imports_;
    HashSet<uint32, ModulePathHash, ModulePathEqual> modulePaths_;
    // 이름 있는 패키지 번호를 이름 byte 순(같으면 번호 순)으로 담는다(FindNamedPackage).
    Array<uint32> packagesByName_;
    Array<Symbol> symbols_;
    Array<TypeEntry> types_;
    Array<uint32> typeArguments_;
    HashSet<uint32, TypeHash, TypeEqual> typeIndex_;
    Array<Signature> signatures_;
    Array<uint32> signatureParameters_;
    Array<uint32> symbolBases_;
    // 모듈마다 노드 번호 → 심볼·타입이다(분석한 모듈의 구간이 nodeBase_[모듈]부터 노드 수만큼,
    // 분석하지 않은 모듈은 NoNode).
    Array<uint32> nodeSymbols_;
    Array<uint32> nodeTypes_;
    Array<uint32> exprTypes_;
    Array<uint32> nodeBase_;
    // 패키지마다 경로가 맞는 모듈 번호를 import 경로 byte 순으로 담는다(packages_와 같은 번호, 그
    // 구간이 sortedModules_ 안의 [first, first + count)).
    Array<uint32> sortedModules_;
    Array<uint32> sortedFirst_;
    Array<uint32> sortedCount_;
    CompilationStage lastStage_ = CompilationStage::All;
    bool checked_ = false;
};

} // namespace rocks::sparks
