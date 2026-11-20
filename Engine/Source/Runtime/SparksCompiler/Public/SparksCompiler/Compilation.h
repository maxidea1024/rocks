#pragma once

#include <Base/NumericLimits.h>
#include <Core/Containers/Array.h>
#include <Core/Containers/HashSet.h>
#include <Core/Containers/Span.h>
#include <Core/Memory/Allocator.h>
#include <Core/Result.h>
#include <Core/Text/StringView.h>
#include <SparksCompiler/BindingSchema.h>
#include <SparksCompiler/Diagnostic.h>
#include <SparksCompiler/PackageManifest.h>
#include <SparksCompiler/Source.h>
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
// 검사 순서(LC2a): binding schema → manifest → 패키지 이름 중복 → 패키지마다 모듈 경로·의존·최상위
// 이름 → 패키지 의존 순환 → manifest가 맞는 패키지의 모듈 구문 분석 → import 해석. 뒤 마일스톤이
// 이름 해석·타입 검사를 더한다.
//
// 수명·스레드: 원문(SourceSet)은 Compilation보다 오래 산다. 결과(트리·해석)는 Compilation이 가진다.
// 한 객체는 한 스레드에서 쓴다.

namespace rocks::sparks {

// 모듈·패키지 번호가 없음을 나타낸다.
inline constexpr uint32 NoModule = NumericLimits<uint32>::Max();
inline constexpr uint32 NoPackage = NumericLimits<uint32>::Max();

// import 하나의 해석 결과다.
struct ImportResolution {
    uint32 node = NoNode;     // 그 모듈 구문 트리의 Import 노드
    uint32 module = NoModule; // 대상 모듈. 해석하지 못했으면(진단을 냈거나 판정하지 않음) NoModule
};

namespace detail {
class CompilationChecker;
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

private:
    friend class detail::CompilationChecker;

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
    // 패키지마다 경로가 맞는 모듈 번호를 import 경로 byte 순으로 담는다(packages_와 같은 번호, 그
    // 구간이 sortedModules_ 안의 [first, first + count)).
    Array<uint32> sortedModules_;
    Array<uint32> sortedFirst_;
    Array<uint32> sortedCount_;
    bool checked_ = false;
};

} // namespace rocks::sparks
