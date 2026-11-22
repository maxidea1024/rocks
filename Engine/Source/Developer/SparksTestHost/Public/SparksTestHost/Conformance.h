#pragma once

#include <Core/Containers/Array.h>
#include <Core/Containers/Span.h>
#include <Core/Memory/Allocator.h>
#include <Core/Result.h>
#include <Core/Text/String.h>
#include <Core/Text/StringView.h>
#include <Platform/File.h>
#include <SparksCompiler/Compilation.h>

// Sparks conformance corpus 실행기다(Docs/Design/SparksLanguage.ko.md 13.1절).
//
// 언제 쓰나: 언어 규칙마다 둔 사례를 컴파일해 기대값(`.expect`)과 맞춰 본다. 시험과 이후의
// `sparks test`가 쓴다.
//
// 사용 예:
//   conformance::Report report(allocator);
//   platform::FileSystem& files = platform::PhysicalFileSystem();
//   auto ran = conformance::RunCorpus(allocator, files, corpusPath, schemaPath, report);
//   bool passed = ran && report.Failures().IsEmpty();
//
// corpus 구조: <corpus>/<영역>/ 아래에 사례가 있다. 사례는 파일 하나(<사례>.spark) 또는 폴더 하나
// (<사례>/)이며, 같은 이름(stem)의 <사례>.expect가 짝이다. 파일 사례는 선택으로 <사례>.ast(구문
// 트리 golden)를 둔다. 사례 이름에는 점이 들어가도 된다(stem으로 짝을 맞춘다).
//   - 파일 사례: manifest `{ name: "Test", sparks: 1, profile: "full" }`인 패키지의 모듈 `Main`으로
//     컴파일한다. 진단 경로는 `<영역>/<사례>.spark`.
//   - 폴더 사례: 폴더 바로 아래에 SparksPackage.json5가 있으면 그 폴더가 패키지 하나, 없으면 바로
//     아래 폴더 하나하나가 패키지(각자 SparksPackage.json5 필수)다. 패키지 안에는 .spark와 루트의
//     SparksPackage.json5만 둔다. 진단 경로는 사례 폴더 기준 경로다.
// 사례는 선택으로 <사례>.names(이름 해석 golden, AppendNameDump — 폴더 사례는 모듈마다
// "== <경로>" 줄과 덤프를 경로 순으로 이은 것)·<사례>.types(식 타입,
// AppendTypeDump)·<사례>.ir(typed IR, AppendIrDump — 진단 없는 사례만)·<사례>.bc(bytecode)·
// <사례>.run(VM 실행 golden — 파일 사례의 최상위 함수 호출과 결과, SparksVm 7절)도 같은 모양으로
// 둔다. `.run`이 있는 파일 사례 옆의 `<사례>.<버전>.spark`(자기 `.expect` 없음)는 사례가 아니라
// `reload <버전>`의 revision 원문이다(SparksReload K9). 짝이 맞지 않거나(`.spark`·폴더 없는
// `.expect`·`.ast`·`.names`, `.expect` 없는 사례, 폴더 사례 옆의 `.ast`)
// 낯선 파일이 있으면 그 자리의 실패로 기록한다(빈 기대값이 조용히 통과하지 않게).
//
// 사례 실행: binding schema(schemaDirectory의 *.sparkbind.json, 진단 경로 `Schema/<파일>`)와 사례의
// 패키지로 Compilation을 만들어 검사한다(LC2a: schema·manifest·모듈 경로·의존·어휘·구문
// 분석·import).
// `.ast`가 있으면 구문 트리 덤프(AppendSyntaxDump)가 그 내용과 byte 단위로 같아야 한다.
//
// `.expect` 형식(13.1절): UTF-8 텍스트(처음의 BOM은 무시), 한 줄에 지시 하나, 단어는 공백이나
// tab으로 나눈다. 빈 줄과 '#'로 시작하는 줄은 주석.
//   error S0412 14:14                 이 위치에 이 코드의 진단(파일 사례)
//   error S0103                       원문 위치가 없는 진단(파일 전체, 파일 사례)
//   error S0301 Game/Main.spark 3:8   폴더 사례는 사례 폴더 기준 파일 경로를 쓴다
//   error S1108 Game/combat.spark     폴더 사례의 파일 전체 진단
//   clean                             진단이 없어야 한다(error와 함께 쓸 수 없다)
//   stage syntax                      그 단계까지만 컴파일한다(syntax·imports·names, 한 번, 기본은
//                                     전체 — 구문 사례가 뒤 단계 규칙에 묶이지 않게)
// 진단은 8.1절 순서대로 모두 일치해야 한다. 파일 사례에 경로를 쓰거나 폴더 사례에 경로를 빠뜨리면
// 형식 오류다.
//
// 결과 구분: 함수의 Result 실패는 실행기 자체가 끝까지 돌지 못한 것(할당 실패, corpus·schema 폴더를
// 열 수 없음)이다. 사례가 기대와 다르거나 corpus 형식이 틀린 것은 Report의 실패 항목이다.
//
// 스레드: 한 Report는 한 스레드에서 쓴다. 함수는 전역 상태를 쓰지 않는다.

namespace rocks::sparks::conformance {

// `.expect`의 `error` 지시 하나다. 위치가 없는 지시(파일 전체 진단)는 line·column이 0이다.
struct ExpectedDiagnostic {
    uint32 code = 0;
    uint32 line = 0;
    uint32 column = 0;
    // 폴더 사례의 파일 경로(사례 폴더 기준, `/` 구분). 파일 사례는 비어 있다. TryParseExpectation에
    // 준 text를 가리키므로 text가 살아 있는 동안 유효하다.
    StringView path;
};

// 해석한 `.expect`다.
class Expectation final {
public:
    explicit Expectation(Allocator& allocator) noexcept : diagnostics_(allocator) {
    }

    bool IsClean() const noexcept {
        return clean_;
    }

    // `stage` 지시의 단계다(없으면 CompilationStage::All).
    CompilationStage Stage() const noexcept {
        return stage_;
    }

    Span<const ExpectedDiagnostic> Diagnostics() const noexcept {
        return diagnostics_.View();
    }

private:
    friend ROCKS_SPARKSTESTHOST_API Result<bool>
    TryParseExpectation(StringView text, Expectation& out, String& problem) noexcept;

    Array<ExpectedDiagnostic> diagnostics_;
    CompilationStage stage_ = CompilationStage::All;
    bool clean_ = false;
};

// `.expect` 원문을 해석한다. 형식이 틀리면 false를 돌려주고 problem에 이유(영어 한 문장)를 쓴다.
// out은 호출 전 내용을 버리고 새로 채운다(실패하면 내용은 정해지지 않는다). out의 경로는 text를
// 가리킨다. Result 실패는 할당 실패뿐이다.
ROCKS_SPARKSTESTHOST_API Result<bool> TryParseExpectation(StringView text, Expectation& out,
                                                          String& problem) noexcept;

// 실행 결과다. 실패 항목은 "사례 이름: 이유" 형태의 영어 문장이며, 진단이 다르면 실제 진단 출력을
// 뒤에 붙인다.
class Report final {
public:
    explicit Report(Allocator& allocator) noexcept : allocator_(&allocator), failures_(allocator) {
    }

    uint32 CasesRun() const noexcept {
        return casesRun_;
    }

    Span<const String> Failures() const noexcept {
        return failures_.View();
    }

    // 실패 항목을 하나 더한다(실행기가 쓴다). text는 복사한다. 실패: UTF-8이 아니면
    // InvalidArgument, 할당 실패는 그 오류(목록 불변).
    Result<void> TryAddFailure(StringView text) noexcept;

    // 실행한 사례 수를 하나 늘린다(실행기가 쓴다).
    void CountCase() noexcept {
        ++casesRun_;
    }

private:
    Allocator* allocator_;
    Array<String> failures_;
    uint32 casesRun_ = 0;
};

// 사례의 파일 하나(폴더 사례의 파일, binding schema 파일)다.
struct CaseFile {
    StringView path; // 진단 경로(폴더 사례는 사례 폴더 기준, schema는 `Schema/<파일>`)
    StringView text;
};

// 폴더 사례의 reload revision 하나다(Docs/Design/SparksReload.ko.md 6절 LC7c2b — 사례 폴더 옆
// `<사례>.<버전>/`, 바뀐 파일만).
struct CaseRevision {
    StringView version;
    Span<const CaseFile> files; // 사례 폴더 기준 경로
};

// 사례 하나의 입력이다.
struct CaseInput {
    // 사례 이름이다. 파일 사례는 진단 경로이기도 하다("Lexical/Bom.spark"), 폴더 사례는
    // "<영역>/<사례>"다.
    StringView name;
    bool folder = false;
    StringView source;            // 파일 사례의 `.spark` 내용
    Span<const CaseFile> files;   // 폴더 사례의 파일들
    Span<const CaseFile> schemas; // 함께 컴파일할 binding schema 파일들
    StringView expect;            // `.expect` 내용
    bool hasSyntaxTree = false;   // `.ast`가 있는지(파일 사례만)
    StringView syntaxTree;        // `.ast` 내용(hasSyntaxTree일 때만)
    bool hasTypes = false;        // `.types`가 있는지(식 타입 golden, AppendTypeDump)
    StringView types;             // `.types` 내용(hasTypes일 때만)
    bool hasIr = false;           // `.ir`가 있는지(typed IR golden, AppendIrDump)
    StringView ir;                // `.ir` 내용(hasIr일 때만)
    bool hasBytecode = false;     // `.bc`가 있는지(bytecode golden, AppendBytecodeDump)
    StringView bytecode;          // `.bc` 내용(hasBytecode일 때만)
    bool hasRun = false;          // `.run`이 있는지(실행 golden — Docs/Design/SparksVm.ko.md 7절)
    // Gate G4(SparksRuntime 4절): `.run`이 통과하면 모든 `AWAIT`에서 fault·취소를 하나씩 주입해
    // 다시 돌리고 CloseSession 뒤 ledger·유지 자원이 0인지만 본다(출력은 비교하지 않는다).
    bool gateSweep = false;
    StringView run; // `.run` 내용(hasRun일 때만)
    // `.run` `reload <버전>`의 revision 원문(Docs/Design/SparksReload.ko.md K9 — 파일 사례 옆
    // `<사례>.<버전>.spark`, path는 버전).
    Span<const CaseFile> revisions;
    // 폴더 사례의 revision(`<사례>.<버전>/` — 바뀐 파일만, 같은 경로로 덮는다).
    Span<const CaseRevision> folderRevisions;
    bool hasNames = false; // `.names`가 있는지
    StringView names;      // `.names` 내용(hasNames일 때만)
};

// 사례 하나를 실행한다. 기대와 다르면 report에 실패 항목을 더한다(한 사례에 진단·구문 트리 실패가
// 각각 하나씩 있을 수 있다). 폴더 사례의 파일 배치는 RunCorpus가 검사한 것으로 보며, 맞지 않으면
// 실패 항목이다. Result 실패는 할당 실패뿐이다.
ROCKS_SPARKSTESTHOST_API Result<void> RunCase(Allocator& allocator, const CaseInput& input,
                                              Report& report) noexcept;

// corpus 폴더 전체를 실행한다. 영역과 사례는 이름의 byte 순으로 돈다(결과가 결정적이다).
// schemaDirectory의 `*.sparkbind.json`을 모든 사례와 함께 컴파일한다(비어 있으면 schema 없음).
// 실패: corpus·schema 폴더를 열거할 수 없으면 NotFound(없음) 또는 InvalidArgument(그 밖의 파일
// 오류), 할당 실패는 그 오류.
ROCKS_SPARKSTESTHOST_API Result<void> RunCorpus(Allocator& allocator, platform::FileSystem& files,
                                                StringView corpusDirectory,
                                                StringView schemaDirectory,
                                                Report& report) noexcept;

} // namespace rocks::sparks::conformance
