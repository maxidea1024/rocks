#pragma once

#include <Core/Containers/Array.h>
#include <Core/Result.h>
#include <Core/Text/StringBuilder.h>
#include <SparksCompiler/Compilation.h>

// Compilation::Check의 내부다(Docs/Design/SparksLanguage.ko.md 3.4절, 10.2절, 18.2절 LC2a). 기능별
// 파일로 나뉜다:
//   Compilation.cpp     입력 API·결과 접근·Check 진입
//   CheckPackages.cpp   binding schema·manifest·패키지 이름·모듈 경로·의존·순환
//   CheckImports.cpp    모듈 구문 분석·import 해석

namespace rocks::sparks::detail {

class CompilationChecker final {
public:
    CompilationChecker(Compilation& compilation, DiagnosticList& diagnostics) noexcept;

    Result<void> Run() noexcept;

private:
    // ---- 진단(CheckPackages.cpp) ----
    void Report(DiagnosticCode code, SourceId source, SourceSpan span, StringView message,
                StringView explanation, StringView fix) noexcept;
    void ReportFile(DiagnosticCode code, SourceId source, StringView message,
                    StringView explanation, StringView fix) noexcept;
    // 문장 하나를 만든다. 실패하면 error_를 세우고 false.
    bool Succeeded(const StringBuilderBase& builder) noexcept;

    // ---- 패키지(CheckPackages.cpp) ----
    void ReadSchemas() noexcept;
    void CheckSchemaDuplicates() noexcept;
    void ReadManifests() noexcept;
    void CheckPackageNames() noexcept;
    void BuildPackageIndex() noexcept;
    void CheckModulePaths(uint32 package) noexcept;
    void CheckPathCase(uint32 package) noexcept;
    void BuildModuleIndex(uint32 package) noexcept;
    void CheckModuleFolders(uint32 package) noexcept;
    void CheckDependencies(uint32 package) noexcept;
    void CheckCycles() noexcept;

    // ---- 모듈(CheckImports.cpp) ----
    void ParseModules() noexcept;
    // 어휘 진단(S01xx)의 위치를 모은다(그 자리의 import 경로는 판정하지 않는다).
    void CollectLexicalErrors() noexcept;
    // source의 span 안에서 시작하는 어휘 진단이 있는지다.
    bool HasLexicalError(SourceId source, SourceSpan span) const noexcept;
    void ResolveImports(uint32 module) noexcept;
    // 모듈 import 경로 중 name과 가장 가까운 것이다(제안용, 없으면 빈 문자열).
    StringView SuggestModule(uint32 package, StringView name) const noexcept;

    Compilation& compilation_;
    DiagnosticList& diagnostics_;
    Allocator& allocator_;
    // (원문 번호 << 32) | 시작 byte, 오름차순이다(HasLexicalError).
    Array<uint64> lexicalErrors_;
    // 이름을 읽지 못한 manifest가 있는지다(S1106을 내지 않는다).
    bool anyNameUnknown_ = false;
    ErrorCode error_ = ErrorCode::None;
};

// 진단 문장 builder의 inline 크기다.
inline constexpr usize CompilationMessageInlineBytes = 256;

} // namespace rocks::sparks::detail
