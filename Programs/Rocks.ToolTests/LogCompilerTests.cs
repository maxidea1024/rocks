using Rocks.Tooling.Common;

namespace Rocks.ToolTests;

// 로그 템플릿·카테고리의 컴파일 타임 검증을 실제 컴파일러로 확인한다(구문 검사만 한다). 올바른 코드는
// 컴파일되고, 잘못된 코드는 실패하며 진단에 영어 표지 함수 이름이나 static_assert 문구가 나와야 한다.
internal static class LogCompilerTests
{
    private const string Prelude = """
        #include <Core/Diagnostics/LogCapture.h>
        struct Point { int x; };
        enum class Mode : unsigned char { Fast = 1 };
        template <> struct rocks::Formatter<Point> {
            static rocks::Result<void> Format(rocks::StringBuilderBase& out, const Point& point,
                                              rocks::StringView) noexcept {
                return out.TryAppendFormat("{}", point.x);
            }
        };
        struct Checked { int x; };
        template <> struct rocks::Formatter<Checked> {
            static rocks::Result<void> Format(rocks::StringBuilderBase& out, const Checked& value,
                                              rocks::StringView) noexcept {
                return out.TryAppendFormat("{}", value.x);
            }

            static constexpr bool IsValidSpec(rocks::StringView spec) noexcept {
                return spec.IsEmpty() || spec == "short";
            }
        };
        struct Shaped { int x; };
        template <> struct rocks::LogStructure<Shaped> {
            static void Write(rocks::LogStructureWriter& out, const Shaped& value) noexcept {
                out.Write("X", value.x);
            }
        };
        struct Plain { int x; };
        enum class Tone : unsigned char { A = 1 };
        template <> struct rocks::LogStructure<Tone> {
            static void Write(rocks::LogStructureWriter& out, const Tone&) noexcept {
                out.Write("A", 1);
            }
        };
        inline constexpr rocks::LogCategory LogValid{"Graphics.D3D12"};
        template <typename... Args>
        void Capture(rocks::Span<rocks::uint8> buffer,
                     rocks::LogTemplate<rocks::TypeIdentityT<Args>...> text, const Args&... args) noexcept {
            (void)buffer;
            (void)text;
            ((void)args, ...);
        }

        """;

    public static void Register(TestSuite suite, Options options, IReadOnlyDictionary<string, string?> environment)
    {
        if (options.Optional("compiler") is not string compiler)
        {
            Console.WriteLine("[SKIP] Native.LogCompilerContracts: --compiler was not supplied");
            return;
        }

        suite.Add("Native.LogCompilerContracts", async () =>
        {
            using TemporaryDirectory temporary = new();
            bool clangCl = Path.GetFileNameWithoutExtension(compiler).Equals("clang-cl", StringComparison.OrdinalIgnoreCase);
            string[] includeDirectories = Directory.GetDirectories(Path.Combine(options.Root, "Core", "Source", "Runtime"))
                .Select(module => Path.Combine(module, "Public")).Where(Directory.Exists).Order(StringComparer.Ordinal).ToArray();

            async Task<ProcessResult> Compile(string name, string body)
            {
                temporary.Write(name + ".cpp", Prelude + body + "\n");
                string source = temporary.FilePath(name + ".cpp");
                List<string> arguments = clangCl
                    ? ["/nologo", "/std:c++latest", "/utf-8", "/W4", "/WX", "/Zs", source]
                    : ["-std=c++23", "-Wall", "-Wextra", "-Wpedantic", "-Werror", "-fsyntax-only", source];
                foreach (string directory in includeDirectories)
                {
                    arguments.Add((clangCl ? "/I" : "-I") + directory);
                }

                arguments.AddRange(options.ClangArguments);
                return await ProcessRunner.RunAsync(new(compiler, arguments, options.Root, environment,
                    Timeout: TimeSpan.FromSeconds(60)));
            }

            static string Call(string arguments)
            {
                return "void Test(rocks::Span<rocks::uint8> buffer) noexcept {\n" +
                    $"    (void)rocks::CaptureLog({{buffer}}, rocks::LogLevel::Info, LogValid, {arguments});\n" +
                    "}";
            }

            static string Category(string literal)
            {
                return $"inline constexpr rocks::LogCategory LogInvalid{{{literal}}};";
            }

            // 위치 자리·이름 자리·사용자 Formatter spec(검사 함수가 없으면 런타임, 있으면 통과하는 spec)·
            // 런타임 템플릿은 컴파일된다.
            string valid = Call("\"{Count} {Ratio:.2f} {P:anything} {M} {C:short}\", 1, 2.0, Point{3}, Mode::Fast, Checked{4}") + "\n" +
                "void Other(rocks::Span<rocks::uint8> buffer) noexcept {\n" +
                "    (void)rocks::CaptureLog({buffer}, rocks::LogLevel::Info, LogValid, \"{} {{x}} {}\", 1, \"x\");\n" +
                "    (void)rocks::CaptureLog({buffer}, rocks::LogLevel::Info, LogValid, rocks::RuntimeTemplate{rocks::StringView(\"{A\")}, 1);\n" +
                "    (void)rocks::CaptureLog({buffer}, rocks::LogLevel::Info, LogValid, \"{@S} and {P}\", Shaped{1}, Point{2});\n" +
                "    (void)rocks::CaptureLog({buffer}, rocks::LogLevel::Info, LogValid, rocks::RuntimeTemplate{rocks::StringView(\"{@S}\")}, Shaped{1});\n" +
                "    Capture(buffer, \"{Name}\", 5);\n" +
                "}";

            (string Name, string Source, string Diagnostic)[] rejected =
            [
                ("count_mismatch", Call("\"{A} {B}\", 1"), "LogTemplateError_ArgumentCountMismatch"),
                ("count_extra", Call("\"{A}\", 1, 2"), "LogTemplateError_ArgumentCountMismatch"),
                ("named_positional", Call("\"{A} {}\", 1, 2"), "LogTemplateError_MixedNamedAndPositional"),
                ("automatic_manual", Call("\"{} {1}\", 1, 2"), "LogTemplateError_MixedAutomaticAndManualIndex"),
                ("unbalanced_open", Call("\"{A\", 1"), "LogTemplateError_UnbalancedBrace"),
                ("unbalanced_close", Call("\"A}\""), "LogTemplateError_UnbalancedBrace"),
                ("bad_spec", Call("\"{A:.2f}\", 1"), "LogTemplateError_InvalidSpec"),
                ("duplicate_name", Call("\"{A} {A}\", 1, 2"), "LogTemplateError_DuplicateHoleName"),
                ("index_range", Call("\"{0} {2}\", 1, 2"), "LogTemplateError_ArgumentIndexOutOfRange"),
                ("hole_name", Call("\"{A-B}\", 1"), "LogTemplateError_InvalidHoleName"),
                ("index_leading_zero", Call("\"{01} {1}\", 1, 2"), "LogTemplateError_InvalidArgumentIndex"),
                ("invalid_utf8", Call("\"\\xff{A}\", 1"), "LogTemplateError_InvalidUtf8"),
                ("formatter_spec_conflict", Call("\"{0} {0:x}\", Point{1}"), "LogTemplateError_ConflictingFormatterSpec"),
                ("formatter_spec_rejected", Call("\"{C:long}\", Checked{1}"), "LogTemplateError_InvalidFormatterSpec"),
                // 구조체 펼치기(LG16): spec 금지, LogStructure 없는 인자, LogStructure만 있는 타입의 일반 자리, '@' 뒤 이름.
                ("structure_spec", Call("\"{@S:x}\", Shaped{1}"), "LogTemplateError_SpecOnStructure"),
                ("structure_formatter_only", Call("\"{@P}\", Point{1}"), "LogTemplateError_StructureWithoutLogStructure"),
                ("structure_builtin", Call("\"{@N}\", 1"), "LogTemplateError_StructureWithoutLogStructure"),
                ("structure_plain_hole", Call("\"{S}\", Shaped{1}"), "LogTemplateError_StructureArgumentWithoutAt"),
                ("structure_positional_hole", Call("\"{}\", Shaped{1}"), "LogTemplateError_StructureArgumentWithoutAt"),
                ("structure_hole_name", Call("\"{@0}\", Shaped{1}"), "LogTemplateError_InvalidHoleName"),
                // Formatter도 LogStructure도 없는 타입은 영어 static_assert로 거부한다(리터럴·런타임 템플릿 모두).
                ("structure_unspecialized", Call("\"{@S}\", Plain{1}"),
                    "Log argument type needs a rocks::Formatter<T> or rocks::LogStructure<T> specialization"),
                ("structure_unspecialized_runtime", Call("rocks::RuntimeTemplate{rocks::StringView(\"{@S}\")}, Plain{1}"),
                    "Log argument type needs a rocks::Formatter<T> or rocks::LogStructure<T> specialization"),
                // LogStructure는 클래스 타입에만 둔다(enum·정수 특수화는 쓰는 곳에서 거부한다).
                ("structure_on_enum", Call("\"{@T}\", Tone::A"),
                    "rocks::LogStructure<T> is only supported for class types"),
                ("structure_on_enum_runtime", Call("rocks::RuntimeTemplate{rocks::StringView(\"{@T}\")}, Tone::A"),
                    "rocks::LogStructure<T> is only supported for class types"),
                ("not_null_terminated",
                    "void Test(rocks::Span<rocks::uint8> buffer) noexcept {\n" +
                    "    static constexpr char text[3] = {'a', 'b', 'c'};\n" +
                    "    (void)rocks::CaptureLog({buffer}, rocks::LogLevel::Info, LogValid, text);\n" +
                    "}",
                    "LogTemplateError_NotNullTerminated"),
                ("generic_parameter", "void Test(rocks::Span<rocks::uint8> buffer) noexcept { Capture(buffer, \"{A} {B}\", 1); }",
                    "LogTemplateError_ArgumentCountMismatch"),
                ("too_many", Call("\"{}{}{}{}{}{}{}{}{}{}{}{}{}{}{}{}{}\", 1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13, 14, 15, 16, 17"),
                    "A log call takes at most MaxLogArguments arguments"),
                ("category_empty", Category("\"\""), "LogCategoryError_EmptyName"),
                ("category_leading_dot", Category("\".A\""), "LogCategoryError_EmptySegment"),
                ("category_double_dot", Category("\"A..B\""), "LogCategoryError_EmptySegment"),
                ("category_trailing_dot", Category("\"A.\""), "LogCategoryError_EmptySegment"),
                ("category_character", Category("\"A-B\""), "LogCategoryError_InvalidCharacter"),
                ("category_depth", Category("\"A.B.C.D.E.F.G.H.I\""), "LogCategoryError_TooManySegments"),
                ("category_length", Category("\"" + new string('L', 64) + "\""), "Log category name exceeds MaxLogCategoryNameBytes"),
            ];

            // 서로 독립인 컴파일이라 함께 돌린다.
            Task<ProcessResult> control = Compile("valid", valid);
            Task<ProcessResult>[] failures = rejected.Select(item => Compile(item.Name, item.Source)).ToArray();
            ProcessResult accepted = await control;
            Verify.Equal(0, accepted.ExitCode, "log compiler/include control: " + accepted.StdOut + accepted.StdErr);
            for (int index = 0; index < rejected.Length; ++index)
            {
                ProcessResult result = await failures[index];
                string output = result.StdOut + result.StdErr;
                Verify.That(result.ExitCode != 0, rejected[index].Name + " compiled but must be rejected");
                Verify.Contains(rejected[index].Diagnostic, output, rejected[index].Name + " diagnostic");
            }
        });
    }
}
