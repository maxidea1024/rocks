#pragma once

#include <Core/Result.h>
#include <Core/Text/StringBuilder.h>
#include <Core/Text/StringView.h>
#include <SparksCompiler/Source.h>
#include <SparksTestHost/Conformance.h>

// conformance 실행기 내부 공용이다. 파일:
//   Expectation.cpp    `.expect` 해석과 기대 진단 표기, Report
//   CaseRunner.cpp     사례 하나 실행(RunCase)
//   RunGolden.cpp      `.run` 실행 golden(VM으로 최상위 함수를 불러 결과를 비교 — SparksVm 7절)
//   CorpusRunner.cpp   corpus·schema 폴더 탐색과 사례 짝 맞추기(RunCorpus)

namespace rocks::sparks::conformance::detail {

inline constexpr StringView SourceExtension = ".spark";
inline constexpr StringView ExpectExtension = ".expect";
inline constexpr StringView SyntaxTreeExtension = ".ast";
inline constexpr StringView NamesExtension = ".names";
inline constexpr StringView TypesExtension = ".types";
inline constexpr StringView IrExtension = ".ir";
inline constexpr StringView BytecodeExtension = ".bc";
inline constexpr StringView RunExtension = ".run";
inline constexpr StringView SchemaExtension = ".sparkbind.json";

// 실패 문장을 담을 builder의 inline 크기다(넘치면 allocator에서 늘린다).
inline constexpr usize MessageInlineBytes = 256;

// corpus 파일 하나의 상한이다. 원문 상한보다 크면 S0103 사례가 되므로 그보다 한 byte 많게 둔다.
inline constexpr usize MaxCorpusFileBytes = MaxSourceBytes + 1;

// 파일 사례를 넣는 패키지의 manifest와 모듈 경로다(Docs/Design/SparksLanguage.ko.md 13.1절).
inline constexpr StringView FileCaseManifest = "{ name: \"Test\", sparks: 1, profile: \"full\" }\n";
inline constexpr StringView FileCaseModulePath = "Main.spark";

// binding schema 파일의 진단 경로 앞부분이다.
inline constexpr StringView SchemaPathPrefix = "Schema/";

// 두 텍스트의 첫 다른 줄(1부터)과 그 줄의 내용이다. 같으면 line은 0이다(CaseRunner.cpp).
struct LineDifference {
    usize line = 0;
    StringView expected;
    StringView actual;
};

LineDifference FindLineDifference(StringView expected, StringView actual) noexcept;

// text의 offset부터 한 줄(LF 앞까지)을 돌려주고 offset을 다음 줄로 옮긴다.
StringView LineAt(StringView text, usize& offset) noexcept;

// 기대 진단 하나를 "    error S0412 [경로] [줄:열]\n"로 쓴다.
Result<void> FormatExpected(StringBuilderBase& out, const ExpectedDiagnostic& expected) noexcept;

// 폴더 사례의 파일(input.files)을 패키지·모듈로 compilation에 넣는다. 배치가 틀리면 reason에
// 이유를 쓰고 false(CaseRunner.cpp — `.run` 폴더 revision도 쓴다).
Result<bool> AddFolderSources(Allocator& allocator, const CaseInput& input, SourceSet& sources,
                              DiagnosticList& diagnostics, Compilation& compilation,
                              StringView& reason) noexcept;

// `.run` 사례: 컴파일한 모듈을 `.sparkc` 왕복 뒤 VM으로 실행해 결과 줄을 비교한다(진단 없는
// 파일 사례만). 다르면 report에 실패 항목을 더한다. Result 실패는 할당 실패뿐이다.
Result<void> CheckRunGolden(Allocator& allocator, const CaseInput& input,
                            const Compilation& compilation, uint32 mainModule,
                            Report& report) noexcept;

} // namespace rocks::sparks::conformance::detail
