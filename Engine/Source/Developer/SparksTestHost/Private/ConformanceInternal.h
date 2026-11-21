#pragma once

#include <Core/Result.h>
#include <Core/Text/StringBuilder.h>
#include <Core/Text/StringView.h>
#include <SparksCompiler/Source.h>
#include <SparksTestHost/Conformance.h>

// conformance 실행기 내부 공용이다. 파일:
//   Expectation.cpp    `.expect` 해석과 기대 진단 표기, Report
//   CaseRunner.cpp     사례 하나 실행(RunCase)
//   CorpusRunner.cpp   corpus·schema 폴더 탐색과 사례 짝 맞추기(RunCorpus)

namespace rocks::sparks::conformance::detail {

inline constexpr StringView SourceExtension = ".spark";
inline constexpr StringView ExpectExtension = ".expect";
inline constexpr StringView SyntaxTreeExtension = ".ast";
inline constexpr StringView NamesExtension = ".names";
inline constexpr StringView TypesExtension = ".types";
inline constexpr StringView IrExtension = ".ir";
inline constexpr StringView BytecodeExtension = ".bc";
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

// 기대 진단 하나를 "    error S0412 [경로] [줄:열]\n"로 쓴다.
Result<void> FormatExpected(StringBuilderBase& out, const ExpectedDiagnostic& expected) noexcept;

} // namespace rocks::sparks::conformance::detail
