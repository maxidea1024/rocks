#include "ShaderSourceTestSupport.h"

#include <Base/InitializerList.h>
#include <Core/Hash/Sha256.h>
#include <Platform/Thread.h>
#include <ShaderCompiler/ShaderSourceCache.h>
#include <TestSupport/Test.h>

using namespace rocks;
namespace shader_preprocessor_tests_file {

using namespace rocks::shader;
using namespace rocks::shader_source_test;

// 셰이더 전처리기(Docs/Design/ShaderSystem 11.1절 SC3) 시험이다: 정규화, 주석·줄 이음,
// 지시문 하나씩·중첩, #include·#pragma once, 매크로, #line·__FILE__, 결정성. 시험마다 임시
// fixture를 /Engine(빈 Platform.rsh)·/Plugin/Test로 mount하고 /Plugin/Test/Main.rsf를 엔트리로
// 쓴다.

TEST(ShaderPreprocessor_PassesPlainSourceThrough) {
    CHECK(Expect("plain", "float4 Main() : SV_Target\n{\n\treturn 1.0f;\n}\n",
                 "#line 1 \"/Plugin/Test/Main.rsf\"\nfloat4 Main() : SV_Target\n{\n\treturn "
                 "1.0f;\n}\n"));
    // 마지막 줄바꿈이 없어도 출력은 '\n'으로 끝난다. 빈 파일은 빈 출력이다.
    CHECK(Expect("noeol", "int a;", "#line 1 \"/Plugin/Test/Main.rsf\"\nint a;\n"));
    CHECK(Expect("empty", "", ""));
}

TEST(ShaderPreprocessor_NormalizesBomAndLineEndings) {
    // BOM + CRLF + 홀로 쓴 CR은 LF 원문과 같은 출력이다. 폐포 SHA-256은 원래 byte로 계산한다.
    const char lf[] = "int a;\nint b;\nint c;\n";
    const char mixed[] = "\xEF\xBB\xBFint a;\r\nint b;\rint c;\r\n";
    Case plain;
    REQUIRE(plain.Make("lf"));
    REQUIRE(plain.File("Main.rsf", lf));
    REQUIRE(plain.Run());
    Case converted;
    REQUIRE(converted.Make("crlf"));
    REQUIRE(converted.File("Main.rsf", mixed));
    REQUIRE(converted.Run());
    CHECK(Same(converted.shader, plain.shader.text.View()));
    CHECK(Same(plain.shader, "#line 1 \"/Plugin/Test/Main.rsf\"\nint a;\nint b;\nint c;\n"));
    bool found = false;
    for (const ShaderIncludeFile& file : converted.shader.includes) {
        if (file.virtualPath.View() == StringView(Entry)) {
            found = true;
            CHECK(file.sha256 == Sha256::Hash(Span<const uint8>(
                                     reinterpret_cast<const uint8*>(mixed), sizeof(mixed) - 1)));
        }
    }

    CHECK(found);

    // UTF-8이 아니면(셋째 줄) InvalidEncoding, NUL도 그렇다. BOM 뒤 UTF-8 주석은 받는다.
    CHECK(ExpectError("utf8", "int a;\nint b;\nint \xC3\x28;\n", ShaderSourceError::InvalidEncoding,
                      3));
    CHECK(ExpectError("nul", StringView("int a;\r\nint\0b;\n", 15),
                      ShaderSourceError::InvalidEncoding, 2));
    CHECK(Expect("utf8ok", "\xEF\xBB\xBF// \xED\x95\x9C\xEA\xB8\x80\nint a;\n",
                 "#line 2 \"/Plugin/Test/Main.rsf\"\nint a;\n"));
}

TEST(ShaderPreprocessor_RemovesCommentsAndSplicesLines) {
    // 주석은 공백 하나, 문자열 안의 주석 표기는 그대로, 줄 이음 뒤 토큰은 원래 줄에 놓인다.
    CHECK(Expect("comments",
                 "int a; // line comment\n"
                 "int /* block */ b;\n"
                 "string s = \"// not /* a comment\";\n",
                 "#line 1 \"/Plugin/Test/Main.rsf\"\n"
                 "int a;\n"
                 "int   b;\n"
                 "string s = \"// not /* a comment\";\n"));
    CHECK(Expect("block",
                 "int a; /* first\n"
                 "second */ int b;\n"
                 "int c;\n",
                 "#line 1 \"/Plugin/Test/Main.rsf\"\n"
                 "int a;\n"
                 "   int b;\n"
                 "int c;\n"));
    CHECK(Expect("splice",
                 "int \\\n"
                 "  a;\n"
                 "// comment \\\n"
                 "continued\n"
                 "int b;\n",
                 "#line 1 \"/Plugin/Test/Main.rsf\"\n"
                 "int\n"
                 "   a;\n"
                 "\n"
                 "\n"
                 "int b;\n"));
    CHECK(ExpectError("unterminated", "int a;\n/* open\nint b;\n",
                      ShaderSourceError::UnterminatedComment, 2));
    CHECK(ExpectError("trigraph", "int a;\nint b ?\?= 1;\n", ShaderSourceError::Trigraph, 2));
    CHECK(ExpectError("literal", "int a;\nstring s = \"open;\n",
                      ShaderSourceError::UnterminatedLiteral, 2));
    // 주석 안의 trigraph와 비활성 구역의 닫히지 않은 아포스트로피는 오류가 아니다.
    CHECK(Expect("inactive", "// what?\?=\n#if 0\ndon't\n#endif\nint a;\n",
                 "#line 5 \"/Plugin/Test/Main.rsf\"\nint a;\n"));
}

TEST(ShaderPreprocessor_HandlesEachDirective) {
    CHECK(Expect("define",
                 "#define VALUE 3\n"
                 "int a = VALUE;\n"
                 "#undef VALUE\n"
                 "int b = VALUE;\n",
                 "#line 2 \"/Plugin/Test/Main.rsf\"\n"
                 "int a = 3;\n"
                 "\n"
                 "int b = VALUE;\n"));
    CHECK(Expect("ifdef",
                 "#define ON\n"
                 "#ifdef ON\n"
                 "int on;\n"
                 "#endif\n"
                 "#ifndef ON\n"
                 "int off;\n"
                 "#else\n"
                 "int other;\n"
                 "#endif\n",
                 "#line 3 \"/Plugin/Test/Main.rsf\"\n"
                 "int on;\n"
                 "\n"
                 "\n"
                 "\n"
                 "\n"
                 "int other;\n"));
    CHECK(Expect("elif",
                 "#if 0\n"
                 "int a;\n"
                 "#elif 1\n"
                 "int b;\n"
                 "#elif 1\n"
                 "int c;\n"
                 "#else\n"
                 "int d;\n"
                 "#endif\n",
                 "#line 4 \"/Plugin/Test/Main.rsf\"\nint b;\n"));
    // 중첩: 바깥이 거짓이면 안쪽 #else·#elif도 비활성이고 안쪽 식은 계산하지 않는다(0 나눗셈).
    CHECK(Expect("nested",
                 "#if 1\n"
                 "#if 0\n"
                 "int a;\n"
                 "#else\n"
                 "int b;\n"
                 "#endif\n"
                 "#endif\n"
                 "#if 0\n"
                 "#if 1 / 0\n"
                 "#elif 1 / 0\n"
                 "#else\n"
                 "int c;\n"
                 "#endif\n"
                 "#unknown directive is ignored here\n"
                 "#endif\n"
                 "int d;\n",
                 "#line 5 \"/Plugin/Test/Main.rsf\"\n"
                 "int b;\n"
                 "#line 16 \"/Plugin/Test/Main.rsf\"\n"
                 "int d;\n"));
    // 빈 지시문 '#'은 아무 일도 하지 않는다.
    CHECK(Expect("null", "#\n# \nint a;\n", "#line 3 \"/Plugin/Test/Main.rsf\"\nint a;\n"));
    // 그 밖의 #pragma는 그 줄에 그대로 남는다(매크로를 펼치지 않는다).
    CHECK(Expect("pragma",
                 "#define ROW row_major\n"
                 "#pragma pack_matrix(ROW)   \n"
                 "int a;\n",
                 "#line 2 \"/Plugin/Test/Main.rsf\"\n"
                 "#pragma pack_matrix(ROW)\n"
                 "int a;\n"));

    Case error;
    REQUIRE(error.Make("error"));
    REQUIRE(error.File("Main.rsf", "int a;\n#if 1\n#error Unsupported   format here\n#endif\n"));
    const ShaderSourceStatus failed = error.Run();
    CHECK(failed.code == ShaderSourceError::ErrorDirective && failed.line == 3);
    CHECK(strstr(failed.message, "Unsupported   format here") != nullptr);
    CHECK(error.shader.text.IsEmpty());

    CHECK(ExpectError("line", "#line 10 \"x\"\n", ShaderSourceError::InvalidDirective, 1));
    CHECK(ExpectError("warning", "int a;\n#warning x\n", ShaderSourceError::InvalidDirective, 2));
    CHECK(ExpectError("number", "# 1 \"x\"\n", ShaderSourceError::InvalidDirective, 1));
    CHECK(ExpectError("else", "#else\n", ShaderSourceError::UnbalancedConditional, 1));
    CHECK(ExpectError("endif", "int a;\n#endif\n", ShaderSourceError::UnbalancedConditional, 2));
    CHECK(ExpectError("elifafterelse", "#if 1\n#else\n#elif 1\n#endif\n",
                      ShaderSourceError::UnbalancedConditional, 3));
    CHECK(ExpectError("open", "#if 1\nint a;\n#ifdef X\n#endif\n",
                      ShaderSourceError::UnbalancedConditional, 1));
    CHECK(
        ExpectError("ifdefextra", "#ifdef A B\n#endif\n", ShaderSourceError::InvalidDirective, 1));
}

TEST(ShaderPreprocessor_ResolvesIncludesAndPragmaOnce) {
    Case test;
    REQUIRE(test.Make("include"));
    REQUIRE(test.File("Main.rsf", "#include \"Lib/Common.rsh\"\n"
                                  "#include \"/Plugin/Test/Lib/Common.rsh\"\n"
                                  "#include \"Lib/Guarded.rsh\"\n"
                                  "#include \"Lib/Guarded.rsh\"\n"
                                  "int main_;\n"
                                  "#if 0\n"
                                  "#include \"Missing.rsh\"\n"
                                  "#endif\n"));
    REQUIRE(
        test.File("Lib/Common.rsh", "#pragma once\nint common;\n#include \"../Shared/Types.h\"\n"));
    REQUIRE(test.File("Shared/Types.h", "#include \"./Inner.h\"\nint types;\n"));
    REQUIRE(test.File("Shared/Inner.h", "int inner;\n"));
    REQUIRE(
        test.File("Lib/Guarded.rsh", "#ifndef GUARDED\n#define GUARDED\nint guarded;\n#endif\n"));
    // 엔트리 끝의 비활성 구역 include는 읽지 않는다(없는 파일이어도 오류가 아니고 폐포에도 없다).
    const ShaderSourceStatus status = test.Run();
    PrintStatus("include", status);
    REQUIRE(status);
    CHECK(Same(test.shader, "#line 2 \"/Plugin/Test/Lib/Common.rsh\"\n"
                            "int common;\n"
                            "#line 1 \"/Plugin/Test/Shared/Inner.h\"\n"
                            "int inner;\n"
                            "#line 2 \"/Plugin/Test/Shared/Types.h\"\n"
                            "int types;\n"
                            "#line 3 \"/Plugin/Test/Lib/Guarded.rsh\"\n"
                            "int guarded;\n"
                            "#line 5 \"/Plugin/Test/Main.rsf\"\n"
                            "int main_;\n"));
    // 폐포: 읽은 파일 전부(Platform.rsh 포함), 가상 경로 byte 순, 한 번씩.
    const char* const expected[] = {"/Engine/Public/Platform.rsh",  "/Plugin/Test/Lib/Common.rsh",
                                    "/Plugin/Test/Lib/Guarded.rsh", "/Plugin/Test/Main.rsf",
                                    "/Plugin/Test/Shared/Inner.h",  "/Plugin/Test/Shared/Types.h"};
    REQUIRE(test.shader.includes.Size() == sizeof(expected) / sizeof(expected[0]));
    for (usize index = 0; index < test.shader.includes.Size(); ++index) {
        CHECK(test.shader.includes[index].virtualPath.View() == StringView(expected[index]));
    }

    const StringView inner("int inner;\n");
    CHECK(test.shader.includes[4].sha256 ==
          Sha256::Hash(
              Span<const uint8>(reinterpret_cast<const uint8*>(inner.Data()), inner.ByteCount())));
}

TEST(ShaderPreprocessor_RejectsUnsupportedIncludes) {
    CHECK(ExpectError("angle", "int a;\n#include <Common.rsh>\n",
                      ShaderSourceError::UnsupportedInclude, 2));
    CHECK(
        ExpectError("next", "#include_next \"A.rsh\"\n", ShaderSourceError::UnsupportedInclude, 1));
    CHECK(ExpectError("drive", "#include \"C:/Shaders/A.rsh\"\n",
                      ShaderSourceError::UnsupportedInclude, 1));
    CHECK(ExpectError("backslash", "#include \"Lib\\A.rsh\"\n",
                      ShaderSourceError::UnsupportedInclude, 1));
    CHECK(ExpectError("computed", "#define FILE \"A.rsh\"\n#include FILE\n",
                      ShaderSourceError::UnsupportedInclude, 2));
    CHECK(ExpectError("extra", "#include \"A.rsh\" x\n", ShaderSourceError::InvalidDirective, 1));
    CHECK(ExpectError("missing", "int a;\n\n#include \"Missing.rsh\"\n",
                      ShaderSourceError::FileNotFound, 3));
    CHECK(
        ExpectError("escape", "#include \"../../../A.rsh\"\n", ShaderSourceError::InvalidPath, 1));
    CHECK(
        ExpectError("root", "#include \"../../Other/A.rsh\"\n", ShaderSourceError::InvalidPath, 1));
    CHECK(
        ExpectError("extension", "#include \"A.hlsl\"\n", ShaderSourceError::InvalidExtension, 1));
    CHECK(ExpectError("empty", "#include \"\"\n", ShaderSourceError::InvalidDirective, 1));

    // 엔트리는 .rsf여야 한다.
    Case test;
    REQUIRE(test.Make("entry"));
    REQUIRE(test.File("Main.rsh", "int a;\n"));
    CHECK(PreprocessShader(test.tree, "/Plugin/Test/Main.rsh", {}, test.shader).code ==
          ShaderSourceError::InvalidArgument);
    CHECK(PreprocessShader(test.tree, "/Plugin/Test/Absent.rsf", {}, test.shader).code ==
          ShaderSourceError::FileNotFound);

    // 오류는 include한 파일 안의 위치로 보고한다.
    Case nested;
    REQUIRE(nested.Make("nestederror"));
    REQUIRE(nested.File("Main.rsf", "#include \"Lib/Bad.rsh\"\n"));
    REQUIRE(nested.File("Lib/Bad.rsh", "int a;\nint b;\n#error inside\n"));
    const ShaderSourceStatus failed = nested.Run();
    CHECK(failed.code == ShaderSourceError::ErrorDirective && failed.line == 3);
    CHECK(strcmp(failed.path, "/Plugin/Test/Lib/Bad.rsh") == 0);
}

TEST(ShaderPreprocessor_ExpandsMacros) {
    // 객체형·함수형·중첩 인자·인자 미리 전개.
    CHECK(Expect("function",
                 "#define TWICE(x) ((x) * 2)\n"
                 "#define ADD(a, b) (a + b)\n"
                 "#define ONE 1\n"
                 "int v = ADD(TWICE(ONE), (3, 4));\n",
                 "#line 4 \"/Plugin/Test/Main.rsf\"\n"
                 "int v = (((1) * 2) + (3, 4));\n"));
    // '#': 문자열화(리터럴 안의 '"'·'\' 이스케이프, 공백 하나), 인자는 펼치지 않는다.
    CHECK(Expect("stringize",
                 "#define STR(x) #x\n"
                 "#define ONE 1\n"
                 "string a = STR(ONE  +   \"q\\n\");\n",
                 "#line 3 \"/Plugin/Test/Main.rsf\"\n"
                 "string a = \"ONE + \\\"q\\\\n\\\"\";\n"));
    // '##': 붙이기, 빈 인자(자리 표시), 붙인 결과는 다시 펼친다.
    CHECK(Expect("paste",
                 "#define CAT(a, b) a ## b\n"
                 "#define XY 7\n"
                 "int a = CAT(X, Y);\n"
                 "int b = CAT(, Y);\n"
                 "int c = CAT(X, );\n"
                 "int d = CAT(, ) 0;\n",
                 "#line 3 \"/Plugin/Test/Main.rsf\"\n"
                 "int a = 7;\n"
                 "int b = Y;\n"
                 "int c = X;\n"
                 "int d = 0;\n"));
    // 가변 인자: 생략·여러 개·인자 없는 매크로 F().
    CHECK(Expect("variadic",
                 "#define CALL(f, ...) f(__VA_ARGS__)\n"
                 "#define NONE() 5\n"
                 "int a = CALL(g, 1, 2, 3);\n"
                 "int b = CALL(h);\n"
                 "int c = NONE();\n",
                 "#line 3 \"/Plugin/Test/Main.rsf\"\n"
                 "int a = g(1, 2, 3);\n"
                 "int b = h();\n"
                 "int c = 5;\n"));
    // 재귀 금지: 자기 자신과 서로 부르는 매크로는 한 번만 펼친다.
    CHECK(Expect("recursion",
                 "#define self self + 1\n"
                 "#define A B\n"
                 "#define B A\n"
                 "#define f(x) x f\n"
                 "int a = self;\n"
                 "int b = A;\n"
                 "int c = f(f)(1);\n",
                 "#line 5 \"/Plugin/Test/Main.rsf\"\n"
                 "int a = self + 1;\n"
                 "int b = A;\n"
                 "int c = f f(1);\n"));
    // 결과 끝의 함수형 이름은 뒤 원문의 '('로 호출된다. 호출은 여러 줄에 걸칠 수 있다.
    CHECK(Expect("rescan",
                 "#define f(x) [x]\n"
                 "#define g f\n"
                 "int a = g(1);\n"
                 "int b = f((2,\n"
                 "  3)) + f\n"
                 "(4);\n"
                 "int c = f;\n",
                 "#line 3 \"/Plugin/Test/Main.rsf\"\n"
                 "int a = [1];\n"
                 "int b = [(2, 3)]\n"
                 " + [4]\n"
                 ";\n"
                 "int c = f;\n"));
    // 같은 내용의 재정의는 받는다.
    CHECK(Expect("redefine", "#define A (1 + 2)\n#define A (1 + 2)\nint a = A;\n",
                 "#line 3 \"/Plugin/Test/Main.rsf\"\nint a = (1 + 2);\n"));
    // 전개 결과가 옆 토큰과 붙어 다른 토큰이 되지 않게 공백을 넣는다.
    CHECK(Expect("separate",
                 "#define PLUS +\n#define E(x) x\nint a = 1 +PLUS 2;\nint b = E(-)-1;\n",
                 "#line 3 \"/Plugin/Test/Main.rsf\"\nint a = 1 + + 2;\nint b = - -1;\n"));
}

TEST(ShaderPreprocessor_RejectsInvalidMacros) {
    CHECK(ExpectError("redefined", "#define A 1\nint a;\n#define A 2\n",
                      ShaderSourceError::MacroRedefined, 3));
    CHECK(ExpectError("pasteedge", "#define A(x) ## x\n", ShaderSourceError::InvalidMacroDefinition,
                      1));
    CHECK(ExpectError("pasteend", "#define A(x) x ##\n", ShaderSourceError::InvalidMacroDefinition,
                      1));
    CHECK(ExpectError("hash", "#define A(x) #y\n", ShaderSourceError::InvalidMacroDefinition, 1));
    CHECK(ExpectError("duplicate", "#define A(x, x) x\n", ShaderSourceError::InvalidMacroDefinition,
                      1));
    CHECK(
        ExpectError("params", "#define A(x y) x\n", ShaderSourceError::InvalidMacroDefinition, 1));
    CHECK(ExpectError("named", "#define A(args...) args\n",
                      ShaderSourceError::InvalidMacroDefinition, 1));
    CHECK(ExpectError("vaargs", "#define A(x) __VA_ARGS__\n",
                      ShaderSourceError::InvalidMacroDefinition, 1));
    CHECK(ExpectError("builtin", "#define __LINE__ 3\n", ShaderSourceError::InvalidMacroDefinition,
                      1));
    CHECK(ExpectError("defined", "#define defined 1\n", ShaderSourceError::InvalidMacroDefinition,
                      1));
    CHECK(ExpectError("noname", "#define 3 4\n", ShaderSourceError::InvalidMacroDefinition, 1));
    CHECK(ExpectError("undef", "#undef __FILE__\n", ShaderSourceError::InvalidMacroDefinition, 1));
    CHECK(ExpectError("count", "#define F(a, b) a\nint x;\nint y = F(1);\n",
                      ShaderSourceError::InvalidMacroCall, 3));
    CHECK(ExpectError("many", "#define F(a) a\nint y = F(1, 2);\n",
                      ShaderSourceError::InvalidMacroCall, 2));
    CHECK(ExpectError("unterminated", "#define F(a) a\nint y = F(1,\n#define B\n);\n",
                      ShaderSourceError::InvalidMacroCall, 2));
    CHECK(
        ExpectError("eof", "#define F(a) a\nint y = F(1", ShaderSourceError::InvalidMacroCall, 2));
    CHECK(ExpectError("paste", "#define CAT(a, b) a ## b\nint y = CAT(+, /);\n",
                      ShaderSourceError::InvalidPaste, 2));
    CHECK(ExpectError("date", "int a;\nstring d = __DATE__;\n",
                      ShaderSourceError::NondeterministicMacro, 2));
    CHECK(ExpectError("time", "#define T __TIME__\nstring t = T;\n",
                      ShaderSourceError::NondeterministicMacro, 2));
    CHECK(ExpectError("timestamp", "string t = __TIMESTAMP__;\n",
                      ShaderSourceError::NondeterministicMacro, 1));
}

TEST(ShaderPreprocessor_WritesLineDirectivesAndFileMacro) {
    Case test;
    REQUIRE(test.Make("line"));
    REQUIRE(test.File("Main.rsf", "int a = __LINE__;\n"
                                  "string f = __FILE__;\n"
                                  "#include \"Lib/A.rsh\"\n"
                                  "int b;\n"
                                  "\n\n\n\n\n\n\n"
                                  "int c; // 8 lines further: blank lines\n"
                                  "\n\n\n\n\n\n\n\n"
                                  "int d; // 9 lines further: #line\n"
                                  "#define WHERE __FILE__ : __LINE__\n"
                                  "string w = WHERE;\n"));
    REQUIRE(test.File("Lib/A.rsh", "string g = __FILE__;\n"));
    const ShaderSourceStatus status = test.Run();
    PrintStatus("line", status);
    REQUIRE(status);
    CHECK(Same(test.shader, "#line 1 \"/Plugin/Test/Main.rsf\"\n"
                            "int a = 1;\n"
                            "string f = \"/Plugin/Test/Main.rsf\";\n"
                            "#line 1 \"/Plugin/Test/Lib/A.rsh\"\n"
                            "string g = \"/Plugin/Test/Lib/A.rsh\";\n"
                            "#line 4 \"/Plugin/Test/Main.rsf\"\n"
                            "int b;\n"
                            "\n\n\n\n\n\n\n"
                            "int c;\n"
                            "#line 21 \"/Plugin/Test/Main.rsf\"\n"
                            "int d;\n"
                            "\n"
                            "string w = \"/Plugin/Test/Main.rsf\" : 23;\n"));
}

TEST(ShaderPreprocessor_PassesBuiltinNamesOutsideConditions) {
    // 조건 밖의 컴파일러 내장 이름은 그대로 남아 실제 컴파일러가 펼친다.
    CHECK(Expect("builtins", "uint m = __SHADER_TARGET_MAJOR;\nbool s = __spirv__;\n",
                 "#line 1 \"/Plugin/Test/Main.rsf\"\nuint m = __SHADER_TARGET_MAJOR;\nbool s = "
                 "__spirv__;\n"));
}

TEST(ShaderPreprocessor_IsDeterministic) {
    // 같은 입력은 같은 byte다. 같은 원문을 다른 실제 디렉터리에 두고 mount해도 같다(작업
    // 디렉터리·실제 경로가 출력에 들어가지 않는다).
    const char main[] = "#include \"A.rsh\"\n#define M(x) x * __LINE__\nfloat v = M(2) + "
                        "__FILE__;\n";
    const char a[] = "#pragma once\nfloat a; \t // tail\n";
    Case first;
    Case second;
    REQUIRE(first.Make("det1"));
    REQUIRE(second.Make("det2"));
    REQUIRE(first.File("Main.rsf", main) && first.File("A.rsh", a));
    REQUIRE(second.File("Main.rsf", main) && second.File("A.rsh", a));
    REQUIRE(first.Run());
    PreprocessedShader again(TestAllocator());
    REQUIRE(PreprocessShader(first.tree, Entry, {}, again));
    REQUIRE(second.Run());
    CHECK(Same(again, first.shader.text.View()));
    CHECK(Same(second.shader, first.shader.text.View()));
    REQUIRE(first.shader.includes.Size() == second.shader.includes.Size());
    for (usize index = 0; index < first.shader.includes.Size(); ++index) {
        CHECK(first.shader.includes[index].virtualPath.View() ==
              second.shader.includes[index].virtualPath.View());
        CHECK(first.shader.includes[index].sha256 == second.shader.includes[index].sha256);
    }

    // 출력에 실제 디렉터리 경로가 없다.
    CHECK(first.shader.text.View().Find(StringView("rocks-shader-"),
                                        StringComparison::CaseSensitive) == StringView::NotFound);
}

TEST(ShaderPreprocessor_AppliesDefinesInNameOrder) {
    Case test;
    REQUIRE(test.Make("defines"));
    REQUIRE(test.File("Main.rsf", "int a = ALPHA + BETA;\nint e = EMPTY;\nint p = PAREN;\n"));
    const ShaderDefine defines[] = {
        {"BETA", "(2)"}, {"ALPHA", "1"}, {"EMPTY", ""}, {"PAREN", "(x) x"}};
    const ShaderSourceStatus status = test.Run(defines);
    PrintStatus("defines", status);
    REQUIRE(status);
    CHECK(Same(test.shader,
               "#line 1 \"/Plugin/Test/Main.rsf\"\nint a = 1 + (2);\nint e =;\nint p = (x) x;\n"));

    const ShaderDefine duplicate[] = {{"A", "1"}, {"A", "1"}};
    CHECK(test.Run(duplicate).code == ShaderSourceError::DuplicateDefine);
    const ShaderDefine badName[] = {{"1A", "1"}};
    CHECK(test.Run(badName).code == ShaderSourceError::InvalidDefine);
    const ShaderDefine badValue[] = {{"A", "1\n2"}};
    CHECK(test.Run(badValue).code == ShaderSourceError::InvalidDefine);
    const ShaderDefine openLiteral[] = {{"A", "\"open"}};
    CHECK(test.Run(openLiteral).code == ShaderSourceError::InvalidDefine);
    const ShaderDefine builtin[] = {{"__FILE__", "1"}};
    CHECK(test.Run(builtin).code == ShaderSourceError::InvalidDefine);
    // 원문이 같은 이름을 다른 내용으로 다시 정의하면 MacroRedefined다.
    REQUIRE(test.File("Main.rsf", "#define ALPHA 2\n"));
    const ShaderDefine alpha[] = {{"ALPHA", "1"}};
    CHECK(test.Run(alpha).code == ShaderSourceError::MacroRedefined);
}

TEST(ShaderPreprocessor_ForcesPlatformHeader) {
    // Platform.rsh가 엔트리 앞에 오고, 엔트리가 직접 include해도(#pragma once) 한 번이다.
    Case test;
    REQUIRE(test.Make("platform", "#pragma once\n#define FROM_PLATFORM 9\nint platform_;\n"));
    REQUIRE(test.File("Main.rsf",
                      "#include \"/Engine/Public/Platform.rsh\"\nint a = FROM_PLATFORM;\n"));
    const ShaderSourceStatus status = test.Run();
    PrintStatus("platform", status);
    REQUIRE(status);
    CHECK(Same(test.shader, "#line 3 \"/Engine/Public/Platform.rsh\"\n"
                            "int platform_;\n"
                            "#line 2 \"/Plugin/Test/Main.rsf\"\n"
                            "int a = 9;\n"));

    // /Engine이 mount되지 않으면 Platform.rsh를 찾지 못한다.
    ShaderSourceTree bare(TestAllocator());
    REQUIRE(bare.Mount(FixturePlugin, test.fixture.Path()));
    PreprocessedShader shader(TestAllocator());
    const ShaderSourceStatus missing =
        PreprocessShader(bare, "/Plugin/Test/Plugin/Main.rsf", {}, shader);
    CHECK(missing.code == ShaderSourceError::UnknownMount);
    CHECK(strcmp(missing.path, ShaderPlatformHeaderPath) == 0);
}

// 생성 include 연결(RenderBase 10.6절 2항): 연결한 가상 경로의 include는 연결 대상 파일을 읽고,
// 폐포·#line에는 대상 경로가 남는다. 연결이 없으면 GeneratedSource, 대상 경로가 규칙 밖이면 그 줄의
// InvalidPath다.
TEST(ShaderPreprocessor_GeneratedIncludeReadsTheMappedSource) {
    Case test;
    REQUIRE(test.Make("generated"));
    REQUIRE(test.File("Main.rsf", "#include \"/Engine/Generated/VertexInput.rsh\"\n"
                                  "int b = VERTEX_VALUE;\n"));
    REQUIRE(test.File("InputA.rsh", "#pragma once\n#define VERTEX_VALUE 7\nint fromInput;\n"));
    const ShaderGeneratedInclude mapped[] = {{StringView("/Engine/Generated/VertexInput.rsh"),
                                              StringView("/Plugin/Test/InputA.rsh"), StringView()}};
    const ShaderSourceStatus status =
        PreprocessShader(test.tree, Entry, Span<const ShaderDefine>(), test.shader, {}, mapped);
    PrintStatus("generated", status);
    REQUIRE(status);
    CHECK(test.shader.text.View().Contains(StringView("int b = 7;")));
    CHECK(test.shader.text.View().Contains(StringView("\"/Plugin/Test/InputA.rsh\"")));
    bool target = false;
    bool generated = false;
    for (const ShaderIncludeFile& file : test.shader.includes) {
        target = target || file.virtualPath.View() == StringView("/Plugin/Test/InputA.rsh");
        generated = generated || file.virtualPath.View().Contains(StringView("Generated"));
    }

    CHECK(target && !generated);

    // 연결이 없으면 소스 트리 규칙대로 GeneratedSource다.
    const ShaderSourceStatus unmapped = test.Run();
    CHECK(unmapped.code == ShaderSourceError::GeneratedSource && unmapped.line == 1);

    // 연결 대상이 가상 경로 규칙 밖이면 include 줄의 InvalidPath다.
    const ShaderGeneratedInclude invalid[] = {{StringView("/Engine/Generated/VertexInput.rsh"),
                                               StringView("Plugin/Test/InputA.rsh"), StringView()}};
    const ShaderSourceStatus rejected =
        PreprocessShader(test.tree, Entry, Span<const ShaderDefine>(), test.shader, {}, invalid);
    CHECK(rejected.code == ShaderSourceError::InvalidPath && rejected.line == 1);
}

// 생성 include 메모리 원문(ShaderSystem 16.2절 SSD52): contents가 있으면 트리 대신 그 원문을 읽고,
// 폐포에는 가상 경로와 원문 SHA-256이 남는다. 연결과 원문을 함께 주면 그 줄의 InvalidArgument다.
TEST(ShaderPreprocessor_GeneratedIncludeReadsMemoryContents) {
    Case test;
    REQUIRE(test.Make("generatedmemory"));
    REQUIRE(test.File("Main.rsf", "#include \"/Engine/Generated/Probe/Params.rsh\"\n"
                                  "int b = PARAM_VALUE;\n"));
    constexpr StringView Contents(
        "#pragma once\n#define PARAM_VALUE 11\ncbuffer Params { float4 v; };\n");
    const ShaderGeneratedInclude memory[] = {
        {StringView("/Engine/Generated/Probe/Params.rsh"), StringView(), Contents}};
    const ShaderSourceStatus status =
        PreprocessShader(test.tree, Entry, Span<const ShaderDefine>(), test.shader, {}, memory);
    PrintStatus("generated memory", status);
    REQUIRE(status);
    CHECK(test.shader.text.View().Contains(StringView("int b = 11;")));
    CHECK(test.shader.text.View().Contains(StringView("cbuffer Params")));
    bool found = false;
    for (const ShaderIncludeFile& file : test.shader.includes) {
        if (file.virtualPath.View() == StringView("/Engine/Generated/Probe/Params.rsh")) {
            found = true;
            CHECK(file.sha256 ==
                  Sha256::Hash(Span<const uint8>(reinterpret_cast<const uint8*>(Contents.Data()),
                                                 Contents.ByteCount())));
        }
    }

    CHECK(found);
    // 원문이 없으면 소스 트리 규칙대로 GeneratedSource다.
    const ShaderSourceStatus missing = test.Run();
    CHECK(missing.code == ShaderSourceError::GeneratedSource && missing.line == 1);
    // 연결과 원문을 함께 주면 거부한다.
    const ShaderGeneratedInclude both[] = {{StringView("/Engine/Generated/Probe/Params.rsh"),
                                            StringView("/Plugin/Test/InputA.rsh"), Contents}};
    const ShaderSourceStatus rejected =
        PreprocessShader(test.tree, Entry, Span<const ShaderDefine>(), test.shader, {}, both);
    CHECK(rejected.code == ShaderSourceError::InvalidArgument && rejected.line == 1);
}

namespace {

// 두 전처리 결과가 원문·include 폐포(경로·SHA-256)까지 같은가다.
bool SamePreprocessed(const PreprocessedShader& left, const PreprocessedShader& right) noexcept {
    if (!(left.text.View() == right.text.View()) || left.includes.Size() != right.includes.Size()) {
        return false;
    }

    for (usize index = 0; index < left.includes.Size(); ++index) {
        if (!(left.includes[index].virtualPath.View() ==
              right.includes[index].virtualPath.View()) ||
            !(left.includes[index].sha256 == right.includes[index].sha256)) {
            return false;
        }
    }

    return true;
}

// 두 결과 상태가 같은가다(코드·줄·경로·문구).
bool SameStatus(const ShaderSourceStatus& left, const ShaderSourceStatus& right) noexcept {
    return left.code == right.code && left.line == right.line &&
           strcmp(left.path, right.path) == 0 && strcmp(left.message, right.message) == 0;
}

// 캐시 하나를 여러 스레드가 함께 쓰는 시험의 스레드 수다.
constexpr usize CacheThreadCount = 4;

struct CacheThread {
    ShaderSourceTree* tree = nullptr;
    ShaderSourceCache* cache = nullptr;
    PreprocessedShader* out = nullptr;
    ShaderSourceStatus status;

    static void Run(void* context) noexcept {
        auto& self = *static_cast<CacheThread*>(context);
        self.status = PreprocessShader(*self.tree, Entry, Span<const ShaderDefine>(), *self.out, {},
                                       {}, self.cache);
    }
};

} // namespace

// 원문 캐시(ShaderSystem 16.6절 SSD98): 캐시로 읽은 전처리 결과·오류가 캐시 없이와 같고, 같은
// 파일은 한 번만 읽으며, 여러 스레드가 함께 써도 결과가 같다.
TEST(ShaderPreprocessor_SourceCacheMatchesUncached) {
    Case test;
    REQUIRE(test.Make("sourcecache"));
    REQUIRE(test.File("Main.rsf", "#include \"Lib/Common.rsh\"\n"
                                  "#include \"Lib/Common.rsh\"\n"
                                  "#include \"Lib/Crlf.rsh\"\n"
                                  "int main_;\n"));
    REQUIRE(test.File("Other.rsf", "#include \"Lib/Common.rsh\"\nint other;\n"));
    REQUIRE(
        test.File("Lib/Common.rsh", "#pragma once\nint common;\n#include \"../Shared/Types.h\"\n"));
    REQUIRE(test.File("Shared/Types.h", "int types;\n"));
    REQUIRE(test.File("Lib/Crlf.rsh", "\xEF\xBB\xBFint crlf;\r\nint second;\r\n"));
    REQUIRE(test.File("Missing.rsf", "int before;\n#include \"Lib/Absent.rsh\"\n"));
    REQUIRE(test.File("Lib/ZeroByte.rsh", StringView("int a;\0int b;\n", 14)));
    REQUIRE(test.File("Encoding.rsf", "#include \"Lib/ZeroByte.rsh\"\n"));

    // 캐시 없이와 캐시로 같은 엔트리를 전처리한다.
    REQUIRE(test.Run());
    ShaderSourceCache cache(test.tree, TestAllocator());
    PreprocessedShader cached(TestAllocator());
    const ShaderSourceStatus first =
        PreprocessShader(test.tree, Entry, Span<const ShaderDefine>(), cached, {}, {}, &cache);
    PrintStatus("cached", first);
    REQUIRE(first);
    CHECK(SamePreprocessed(test.shader, cached));
    // 읽은 파일: Platform.rsh, Main.rsf, Common.rsh, Types.h, Crlf.rsh(Common.rsh는 한 번).
    const usize afterMain = cache.EntryCount();
    CHECK(afterMain == 5);

    // 다른 엔트리는 새 파일만 더 읽는다.
    PreprocessedShader other(TestAllocator());
    PreprocessedShader otherCached(TestAllocator());
    REQUIRE(
        PreprocessShader(test.tree, "/Plugin/Test/Other.rsf", Span<const ShaderDefine>(), other));
    REQUIRE(PreprocessShader(test.tree, "/Plugin/Test/Other.rsf", Span<const ShaderDefine>(),
                             otherCached, {}, {}, &cache));
    CHECK(SamePreprocessed(other, otherCached) && cache.EntryCount() == afterMain + 1);

    // 오류도 같다: 없는 include(두 번째는 캐시한 실패), 정규화 실패(NUL).
    for (const char* entry : {"/Plugin/Test/Missing.rsf", "/Plugin/Test/Encoding.rsf"}) {
        PreprocessedShader plain(TestAllocator());
        const ShaderSourceStatus expected =
            PreprocessShader(test.tree, entry, Span<const ShaderDefine>(), plain);
        CHECK(!expected);
        for (int round = 0; round < 2; ++round) {
            PreprocessedShader viaCache(TestAllocator());
            const ShaderSourceStatus actual = PreprocessShader(
                test.tree, entry, Span<const ShaderDefine>(), viaCache, {}, {}, &cache);
            CHECK(SameStatus(expected, actual) && viaCache.text.IsEmpty());
        }
    }

    // 파일 한도가 다르거나 다른 트리의 캐시는 거부한다.
    ShaderSourceCache small(test.tree, TestAllocator(), 1024);
    PreprocessedShader rejected(TestAllocator());
    CHECK(PreprocessShader(test.tree, Entry, Span<const ShaderDefine>(), rejected, {}, {}, &small)
              .code == ShaderSourceError::InvalidArgument);
    ShaderSourceTree otherTree(TestAllocator());
    ShaderSourceCache foreign(otherTree, TestAllocator());
    CHECK(PreprocessShader(test.tree, Entry, Span<const ShaderDefine>(), rejected, {}, {}, &foreign)
              .code == ShaderSourceError::InvalidArgument);

    // 새 캐시 하나를 여러 스레드가 함께 쓴다(같은 파일을 동시에 처음 읽는 경로).
    ShaderSourceCache shared(test.tree, TestAllocator());
    PreprocessedShader outputs[CacheThreadCount] = {
        PreprocessedShader(TestAllocator()), PreprocessedShader(TestAllocator()),
        PreprocessedShader(TestAllocator()), PreprocessedShader(TestAllocator())};
    CacheThread contexts[CacheThreadCount];
    platform::Thread threads[CacheThreadCount];
    for (usize index = 0; index < CacheThreadCount; ++index) {
        contexts[index].tree = &test.tree;
        contexts[index].cache = &shared;
        contexts[index].out = &outputs[index];
        REQUIRE(threads[index].Start(&CacheThread::Run, &contexts[index]) ==
                platform::ThreadStatus::Success);
    }

    for (usize index = 0; index < CacheThreadCount; ++index) {
        REQUIRE(threads[index].Join() == platform::ThreadStatus::Success);
        CHECK(contexts[index].status && SamePreprocessed(test.shader, outputs[index]));
    }

    CHECK(shared.EntryCount() == afterMain);
}

} // namespace shader_preprocessor_tests_file
