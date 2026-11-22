#pragma once

#include <ShaderCompiler/ShaderSourceTree.h>

// 소스 트리·전처리기·정의 조립이 함께 쓰는 결과 만들기다(모듈 안 전용).
namespace rocks::shader::source_detail {

// code·path·line과 영어 문구로 결과를 만든다. detail이 있으면 "<message>: '<detail>'"이다. 긴 값은
// 버퍼 크기에서 잘린다.
ShaderSourceStatus MakeSourceStatus(ShaderSourceError code, StringView path, uint32 line,
                                    StringView message, StringView detail = StringView()) noexcept;

} // namespace rocks::shader::source_detail
