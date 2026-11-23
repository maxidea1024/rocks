#include "ShaderSourceTestSupport.h"

#include <Core/Text/String.h>
#include <Base/InitializerList.h>
#include <TestSupport/Test.h>

using namespace rocks;
namespace shader_preprocessor_condition_tests_file {

using namespace rocks::shader;
using namespace rocks::shader_source_test;

// 셰이더 전처리기(Docs/Design/ShaderSystem.ko.md 11.1절 SC3)의 조건식·내장 이름 거부·한도 경계
// 시험이다.
namespace {

// "#if <식>" 하나를 계산해 참·거짓을 돌려준다. 식 앞에 ONE·TWO·FUNC 정의가 있다(#if는 4번째
// 줄이다). 오류면 status에 둔다.
bool Condition(const char* expression, bool& value, ShaderSourceStatus& status) noexcept {
    Case test;
    if (!test.Make("cond")) {
        return false;
    }

    char source[512];
    snprintf(
        source, sizeof(source),
        "#define ONE 1\n#define TWO 2\n#define FUNC(x) ((x) * 2)\n#if %s\nint yes;\n#else\nint "
        "no;\n#endif\n",
        expression);
    if (!test.File("Main.rsf", source)) {
        return false;
    }

    status = test.Run();
    value = test.shader.text.View() == StringView("#line 5 \"/Plugin/Test/Main.rsf\"\nint yes;\n");
    return true;
}

} // namespace

TEST(ShaderPreprocessor_EvaluatesConditions) {
    struct Expectation {
        const char* expression;
        bool value;
    };

    const Expectation cases[] = {
        {"1 + 2 * 3 == 7", true},
        {"(1 + 2) * 3 == 9", true},
        {"1 - 1 - 1 == -1", true},
        {"2 * 3 % 4 == 2", true},
        {"10 / 3 == 3 && 10 % 3 == 1", true},
        {"-7 / 2 == -3 && -7 % 2 == -1", true},
        {"3 > 2 > 1", false},
        {"-1 < 0", true},
        {"-1 < 0u", false},
        {"0xFFFFFFFFFFFFFFFF == -1", true},
        {"18446744073709551615 > 0", true},
        {"9223372036854775807 > 0", true},
        {"1 << 62 > 0", true},
        {"-8 >> 1 == -4", true},
        {"0xF0u >> 4 == 15", true},
        {"~0 == -1", true},
        {"(5 & 3) == 1 && (5 | 3) == 7 && (5 ^ 3) == 6", true},
        {"!0 && !!5", true},
        {"0 || 0", false},
        {"(1 ? 2 : 3) == 2", true},
        {"0 ? 1 : 0", false},
        {"(0 ? 1u : -1) > 0", true},
        {"0 && 1 / 0", false},
        {"1 || 1 % 0", true},
        {"0 ? 1 / 0 : 1", true},
        {"1 ? 1 : 1 << 64", true},
        {"010 == 8 && 0x10 == 16 && 0b101 == 5 && 0 == 0", true},
        {"5ull == 5 && 7L == 7 && 3U == 3 && 2lu == 2", true},
        {"(-9223372036854775807 - 1) / -1 < 0", true},
        {"UNDEFINED_NAME == 0", true},
        {"true && !false", true},
        {"defined(ONE) && defined TWO && !defined(THREE)", true},
        {"ONE + TWO == 3", true},
        {"FUNC(4) == 8", true},
        {"__LINE__ == 4", true},
        {"defined(__FILE__) && defined __LINE__", true},
        {"+1 == 1 && - -1 == 1", true},
    };
    for (const Expectation& expectation : cases) {
        bool value = false;
        ShaderSourceStatus status;
        REQUIRE(Condition(expectation.expression, value, status));
        PrintStatus(expectation.expression, status);
        if (!status || value != expectation.value) {
            fprintf(stderr, "condition '%s' expected %d\n", expectation.expression,
                    expectation.value ? 1 : 0);
        }

        CHECK(status && value == expectation.value);
    }
}

TEST(ShaderPreprocessor_RejectsInvalidConditions) {
    const char* const invalid[] = {
        "",
        "1 +",
        "(1",
        "1 / 0",
        "1 % 0",
        "1 << 64",
        "1 << -1",
        "1 >> 64",
        "'a'",
        "1.5",
        "99999999999999999999",
        "defined",
        "defined(",
        "defined(ONE",
        "1 2",
        "\"s\"",
        "__FILE__",
        "1 ? 2",
        "1u << 70u",
        "0x",
        "08",
        "5x",
    };
    for (const char* expression : invalid) {
        bool value = false;
        ShaderSourceStatus status;
        REQUIRE(Condition(expression, value, status));
        if (status.code != ShaderSourceError::InvalidExpression || status.line != 4) {
            fprintf(stderr, "condition '%s': %s line %u (%s)\n", expression,
                    ShaderSourceErrorName(status.code), status.line, status.message);
        }

        CHECK(status.code == ShaderSourceError::InvalidExpression && status.line == 4);
    }

    // 매크로가 만든 defined는 받지 않는다.
    CHECK(ExpectError("defmacro", "#define D defined(X)\n#if D\n#endif\n",
                      ShaderSourceError::InvalidExpression, 2));
    // 괄호 중첩 상한을 넘으면 오류다(스택 보호).
    String deep(TestAllocator());
    REQUIRE(deep.TryAppend("#if "));
    for (int index = 0; index < 300; ++index) {
        REQUIRE(deep.TryAppend("("));
    }

    REQUIRE(deep.TryAppend("1"));
    for (int index = 0; index < 300; ++index) {
        REQUIRE(deep.TryAppend(")"));
    }

    REQUIRE(deep.TryAppend("\n#endif\n"));
    CHECK(ExpectError("deep", deep.View(), ShaderSourceError::InvalidExpression, 1));
}

TEST(ShaderPreprocessor_RejectsCompilerBuiltinsInConditions) {
    const char* const sources[] = {
        "#if __SHADER_TARGET_MAJOR >= 6\n#endif\n",
        "#ifdef __spirv__\n#endif\n",
        "#ifndef __HLSL_VERSION\n#endif\n",
        "#if defined(__DXC_VERSION_MAJOR)\n#endif\n",
        "#if defined __SHADER_TARGET_STAGE\n#endif\n",
        "#if __has_include(\"x\")\n#endif\n",
        "#if 0\n#elif __SHADER_TARGET_MINOR\n#endif\n",
        "#define V __SHADER_TARGET_MAJOR\n#if V\n#endif\n",
        "#define W(x) x\n#if W(__spirv__)\n#endif\n",
    };
    const uint32 lines[] = {1, 1, 1, 1, 1, 1, 2, 2, 2};
    for (usize index = 0; index < sizeof(sources) / sizeof(sources[0]); ++index) {
        CHECK(ExpectError("builtin", sources[index], ShaderSourceError::CompilerBuiltinInCondition,
                          lines[index]));
    }

    // __LINE__·__FILE__는 받고, 계산하지 않는 갈래의 내장 이름은 보지 않는다.
    CHECK(Expect("allowed",
                 "#if __LINE__ > 0 && defined(__FILE__)\n"
                 "int a;\n"
                 "#endif\n"
                 "#if 1\n"
                 "#elif __SHADER_TARGET_MAJOR\n"
                 "#endif\n"
                 "#if 0\n"
                 "#if __spirv__\n"
                 "#endif\n"
                 "#endif\n",
                 "#line 2 \"/Plugin/Test/Main.rsf\"\nint a;\n"));
}

TEST(ShaderPreprocessor_LimitsIncludeDepth) {
    CHECK(MaxShaderIncludeDepth == 64);
    // 엔트리(깊이 0)가 D1을, Dk가 D(k+1)을 include한다. 깊이 64까지 받고 65는 거부한다.
    for (const uint32 deepest : {MaxShaderIncludeDepth, MaxShaderIncludeDepth + 1}) {
        Case test;
        REQUIRE(test.Make("depth"));
        REQUIRE(test.File("Main.rsf", "#include \"D1.rsh\"\n"));
        for (uint32 depth = 1; depth <= deepest; ++depth) {
            char name[32];
            char text[64];
            snprintf(name, sizeof(name), "D%u.rsh", depth);
            if (depth == deepest) {
                snprintf(text, sizeof(text), "int deepest;\n");
            } else {
                snprintf(text, sizeof(text), "\n#include \"D%u.rsh\"\n", depth + 1);
            }

            REQUIRE(test.File(name, text));
        }

        const ShaderSourceStatus status = test.Run();
        if (deepest == MaxShaderIncludeDepth) {
            PrintStatus("depth 64", status);
            CHECK(status);
            CHECK(test.shader.text.View().Find(StringView("int deepest;"),
                                               StringComparison::CaseSensitive) !=
                  StringView::NotFound);
        } else {
            CHECK(status.code == ShaderSourceError::IncludeDepthExceeded && status.line == 2);
            CHECK(strcmp(status.path, "/Plugin/Test/D64.rsh") == 0);
        }
    }

    // #pragma once 없는 자기 include도 깊이 한도에서 멈춘다.
    CHECK(
        ExpectError("self", "#include \"Main.rsf\"\n", ShaderSourceError::IncludeDepthExceeded, 1));
}

TEST(ShaderPreprocessor_LimitsMacroExpansionDepth) {
    CHECK(MaxShaderMacroExpansionDepth == 256);
    // M1 → M2 → … → Mn → end: 펼치는 중인 매크로가 n개 겹친다. 256개까지 받고 257은 거부한다.
    for (const uint32 count : {MaxShaderMacroExpansionDepth, MaxShaderMacroExpansionDepth + 1}) {
        String source(TestAllocator());
        for (uint32 index = 1; index <= count; ++index) {
            char line[64];
            if (index == count) {
                snprintf(line, sizeof(line), "#define M%u end\n", index);
            } else {
                snprintf(line, sizeof(line), "#define M%u M%u\n", index, index + 1);
            }

            REQUIRE(source.TryAppend(line));
        }

        REQUIRE(source.TryAppend("int M1;\n"));
        Case test;
        REQUIRE(test.Make("macrodepth"));
        REQUIRE(test.File("Main.rsf", source.View()));
        const ShaderSourceStatus status = test.Run();
        if (count == MaxShaderMacroExpansionDepth) {
            PrintStatus("macro depth 256", status);
            CHECK(status);
            CHECK(test.shader.text.View().EndsWith(StringView("int end;\n"),
                                                   StringComparison::CaseSensitive));
        } else {
            CHECK(status.code == ShaderSourceError::MacroDepthExceeded && status.line == count + 1);
        }
    }

    // 인자 미리 전개의 중첩: F(F(…F(x)…))가 256겹이면 받고 257겹이면 거부한다.
    for (const uint32 count : {MaxShaderMacroExpansionDepth, MaxShaderMacroExpansionDepth + 1}) {
        String source(TestAllocator());
        REQUIRE(source.TryAppend("#define F(x) x\nint "));
        for (uint32 index = 0; index < count; ++index) {
            REQUIRE(source.TryAppend("F("));
        }

        REQUIRE(source.TryAppend("v"));
        for (uint32 index = 0; index < count; ++index) {
            REQUIRE(source.TryAppend(")"));
        }

        REQUIRE(source.TryAppend(";\n"));
        Case test;
        REQUIRE(test.Make("argdepth"));
        REQUIRE(test.File("Main.rsf", source.View()));
        const ShaderSourceStatus status = test.Run();
        if (count == MaxShaderMacroExpansionDepth) {
            PrintStatus("argument depth 256", status);
            CHECK(status);
            CHECK(Same(test.shader, "#line 2 \"/Plugin/Test/Main.rsf\"\nint v;\n"));
        } else {
            CHECK(status.code == ShaderSourceError::MacroDepthExceeded && status.line == 2);
        }
    }
}

TEST(ShaderPreprocessor_LimitsFileAndOutputBytes) {
    const ShaderPreprocessLimits defaults;
    CHECK(defaults.includeDepth == MaxShaderIncludeDepth &&
          defaults.macroExpansionDepth == MaxShaderMacroExpansionDepth &&
          defaults.fileBytes == MaxShaderSourceFileBytes &&
          defaults.outputBytes == MaxShaderPreprocessOutputBytes);
    CHECK(MaxShaderPreprocessOutputBytes == usize{64} * 1024 * 1024);

    // 파일 크기: 한도와 같으면 받고 1 byte 작은 한도면 거부한다(include한 파일은 include 줄 위치).
    const char main[] = "#include \"Lib.rsh\"\n";
    const char lib[] = "int lib_variable_with_a_long_name;\n";
    static_assert(sizeof(lib) > sizeof(main), "the included file must be the larger one");
    Case test;
    REQUIRE(test.Make("bytes"));
    REQUIRE(test.File("Main.rsf", main));
    REQUIRE(test.File("Lib.rsh", lib));
    ShaderPreprocessLimits limits;
    limits.fileBytes = sizeof(lib) - 1;
    CHECK(test.Run({}, limits));
    limits.fileBytes = sizeof(lib) - 2;
    ShaderSourceStatus status = test.Run({}, limits);
    CHECK(status.code == ShaderSourceError::FileTooLarge && status.line == 1 &&
          strcmp(status.path, "/Plugin/Test/Main.rsf") == 0);
    limits.fileBytes = sizeof(main) - 2;
    status = test.Run({}, limits);
    CHECK(status.code == ShaderSourceError::FileTooLarge && status.line == 0);

    // 출력: 결과 크기와 같은 한도는 받고 1 byte 작으면 거부한다.
    REQUIRE(test.Run());
    const usize produced = test.shader.text.ByteCount();
    limits = ShaderPreprocessLimits{};
    limits.outputBytes = produced;
    CHECK(test.Run({}, limits));
    CHECK(test.shader.text.ByteCount() == produced);
    limits.outputBytes = produced - 1;
    status = test.Run({}, limits);
    CHECK(status.code == ShaderSourceError::OutputTooLarge);
    CHECK(test.shader.text.IsEmpty());

    // 출력하지 않는 전개(#if 식)도 같은 한도로 막는다.
    String sum(TestAllocator());
    REQUIRE(sum.TryAppend("#define SUM 1"));
    for (int index = 0; index < 100; ++index) {
        REQUIRE(sum.TryAppend(" + 1"));
    }

    REQUIRE(sum.TryAppend("\n#if SUM\n#endif\n"));
    REQUIRE(test.File("Main.rsf", sum.View()));
    limits.outputBytes = 100;
    status = test.Run({}, limits);
    CHECK(status.code == ShaderSourceError::OutputTooLarge && status.line == 2);
}

} // namespace shader_preprocessor_condition_tests_file
