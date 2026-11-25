#pragma once

#include <Core/Containers/Array.h>
#include <Core/Result.h>
#include <Core/Text/StringView.h>
#include <SparksCompiler/Compilation.h>

// 언어 서버 내부 공용이다(Docs/Design/SparksTools.ko.md 10절). 파일:
//   LspFraming.cpp        Content-Length 틀
//   LspText.cpp           UTF-16 위치·file URI
//   LspServer.cpp         수명·문서 저장소·분석·진단 발행·요청 분기
//   LspSemanticTokens.cpp 의미 강조(T12)
namespace rocks::sparks::tools::detail {

// 의미 강조 legend다(10.7절 — 순서가 번호다).
inline constexpr StringView LspTokenTypes[] = {
    "namespace",  "type",  "class",    "struct", "enum",     "interface", "typeParameter",
    "enumMember", "event", "function", "method", "property", "parameter", "variable"};
inline constexpr StringView LspTokenModifiers[] = {"declaration", "readonly", "static", "async",
                                                   "defaultLibrary"};

// compilation 모듈의 의미 강조 data(토큰마다 줄 차이·시작 차이·길이·종류·modifier bit — UTF-16)를
// data 뒤에 붙인다. 구문 트리가 없는 모듈은 아무것도 붙이지 않는다. 실패: 할당 실패.
Result<void> LspAppendSemanticTokens(const Compilation& compilation, const SourceSet& sources,
                                     uint32 module, Array<uint32>& data) noexcept;

} // namespace rocks::sparks::tools::detail
