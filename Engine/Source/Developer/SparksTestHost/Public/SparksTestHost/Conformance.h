#pragma once

#include <Core/Containers/Array.h>
#include <Core/Containers/Span.h>
#include <Core/Memory/Allocator.h>
#include <Core/Result.h>
#include <Core/Text/String.h>
#include <Core/Text/StringView.h>
#include <Platform/File.h>

// Sparks conformance corpus 실행기다(Docs/Design/SparksLanguage.ko.md 13.1절).
//
// 언제 쓰나: 언어 규칙마다 둔 사례(`.spark`)를 컴파일해 기대값(`.expect`)과 맞춰 본다. 시험과
// 이후의 `sparks test`가 쓴다.
//
// 사용 예:
//   conformance::Report report(allocator);
//   platform::FileSystem& files = platform::PhysicalFileSystem();
//   auto ran = conformance::RunCorpus(allocator, files, corpusPath, report);
//   bool passed = ran && report.Failures().IsEmpty();
//
// corpus 구조: <corpus>/<영역>/<사례>.spark 와 같은 이름(stem)의 <사례>.expect. 사례 이름에는 점이
// 들어가도 된다(stem으로 짝을 맞춘다). 영역 아래에 다른 파일이나 폴더가 있거나 짝이 맞지 않으면
// 그 자리의 실패로 기록한다(빈 기대값이 조용히 통과하지 않게).
//
// `.expect` 형식(13.1절): UTF-8 텍스트(처음의 BOM은 무시), 한 줄에 지시 하나, 단어는 공백이나
// tab으로 나눈다. 빈 줄과 '#'로 시작하는 줄은 주석.
//   error S0412 14:14   이 위치에 이 코드의 진단이 나야 한다(8.1절 순서대로 모두 일치)
//   error S0103         원문 위치가 없는 진단(파일 전체)
//   clean               진단이 없어야 한다(error와 함께 쓸 수 없다)
//
// 결과 구분: 함수의 Result 실패는 실행기 자체가 끝까지 돌지 못한 것(할당 실패, corpus 폴더를 열 수
// 없음)이다. 사례가 기대와 다르거나 corpus 형식이 틀린 것은 Report의 실패 항목이다.
//
// 스레드: 한 Report는 한 스레드에서 쓴다. 함수는 전역 상태를 쓰지 않는다.

namespace rocks::sparks::conformance {

// `.expect`의 `error` 지시 하나다. 위치가 없는 지시(파일 전체 진단)는 line·column이 0이다.
struct ExpectedDiagnostic {
    uint32 code = 0;
    uint32 line = 0;
    uint32 column = 0;
};

// 해석한 `.expect`다.
class Expectation final {
public:
    explicit Expectation(Allocator& allocator) noexcept : diagnostics_(allocator) {
    }

    bool IsClean() const noexcept {
        return clean_;
    }

    Span<const ExpectedDiagnostic> Diagnostics() const noexcept {
        return diagnostics_.View();
    }

private:
    friend Result<bool> TryParseExpectation(StringView text, Expectation& out,
                                            String& problem) noexcept;

    Array<ExpectedDiagnostic> diagnostics_;
    bool clean_ = false;
};

// `.expect` 원문을 해석한다. 형식이 틀리면 false를 돌려주고 problem에 이유(영어 한 문장)를 쓴다.
// out은 호출 전 내용을 버리고 새로 채운다(실패하면 내용은 정해지지 않는다).
// Result 실패는 할당 실패뿐이다.
Result<bool> TryParseExpectation(StringView text, Expectation& out, String& problem) noexcept;

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

// 사례 하나를 실행한다. caseName은 진단 경로로도 쓴다(예: "Lexical/Bom.spark").
Result<void> RunCase(Allocator& allocator, StringView caseName, StringView source,
                     StringView expectText, Report& report) noexcept;

// corpus 폴더 전체를 실행한다. 영역과 사례는 이름의 byte 순으로 돈다(결과가 결정적이다).
// 실패: corpus 폴더를 열거할 수 없으면 NotFound(없음) 또는 InvalidArgument(그 밖의 파일 오류),
// 할당 실패는 그 오류.
Result<void> RunCorpus(Allocator& allocator, platform::FileSystem& files,
                       StringView corpusDirectory, Report& report) noexcept;

} // namespace rocks::sparks::conformance
