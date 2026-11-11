using Rocks.Tooling.Common;

namespace Rocks.ToolTests;

// 실제 컴파일러로 사용 가능한 코드와 금지한 호출을 각각 확인한다.
internal static class AlgorithmsCompilerTests
{
    public static void Register(TestSuite suite, Options options, IReadOnlyDictionary<string, string?> environment)
    {
        if (options.Optional("compiler") is not string compiler)
        {
            Console.WriteLine("[SKIP] Native.AlgorithmsCompilerContracts: --compiler was not supplied");
            return;
        }

        suite.Add("Native.AlgorithmsCompilerContracts", async () =>
        {
            using TemporaryDirectory temporary = new();
            bool clangCl = Path.GetFileNameWithoutExtension(compiler).Equals("clang-cl", StringComparison.OrdinalIgnoreCase);
            string[] includeDirectories = Directory.GetDirectories(Path.Combine(options.Root, "Engine"))
                .Select(module => Path.Combine(module, "Public")).Where(Directory.Exists).Order(StringComparer.Ordinal).ToArray();

            async Task<ProcessResult> Compile(string name, string body)
            {
                temporary.Write(name + ".cpp", "#include <Core/Algorithms.h>\n" + body + "\n");
                string source = temporary.FilePath(name + ".cpp");
                List<string> arguments = clangCl
                    ? ["/nologo", "/std:c++latest", "/W4", "/WX", "/Zs", source]
                    : ["-std=c++23", "-Wall", "-Wextra", "-Wpedantic", "-Werror", "-fsyntax-only", source];
                foreach (string directory in includeDirectories)
                    arguments.Add((clangCl ? "/I" : "-I") + directory);
                arguments.AddRange(options.ClangArguments);
                return await ProcessRunner.RunAsync(new(compiler, arguments, options.Root, environment,
                    Timeout: TimeSpan.FromSeconds(60)));
            }

            const string valid = """
                struct Answer { explicit operator bool() const noexcept { return true; } };
                struct Compare { Answer operator()(int, int) const noexcept { return {}; } };
                struct Key {};
                struct HeterogeneousCompare {
                    bool operator()(int, const Key&) const noexcept { return false; }
                    bool operator()(const Key&, int) const noexcept { return false; }
                };
                void Test(rocks::Span<int> values) noexcept {
                    Compare compare;
                    rocks::algo::Sort(values, compare);
                    (void)rocks::algo::LowerBound(values, 1, compare);
                    (void)rocks::algo::UpperBound(values, 1, compare);
                    (void)rocks::algo::BinarySearch(values, 1, compare);
                    (void)rocks::algo::BinarySearch(values, Key{}, HeterogeneousCompare{});
                }
                """;
            ProcessResult control = await Compile("valid", valid);
            Verify.Equal(0, control.ExitCode, "algorithm compiler/include control: " + control.StdOut + control.StdErr);

            foreach (string operation in new[] { "Sort", "LowerBound", "UpperBound", "BinarySearch" })
            {
                string arguments = operation == "Sort" ? "values, compare" : "values, 1, compare";
                string diagnostic = operation + " comparator must be noexcept";
                string throwing = "struct Compare { bool operator()(int, int) const noexcept(false) { return true; } };\n" +
                    $"void Test(rocks::Span<int> values) {{ Compare compare; (void)rocks::algo::{operation}({arguments}); }}";
                ProcessResult rejected = await Compile("throwing_" + operation, throwing);
                Verify.That(rejected.ExitCode != 0, operation + " accepted a throwing comparator");
                Verify.Contains(diagnostic, rejected.StdOut + rejected.StdErr, operation + " throwing comparator diagnostic");

                string throwingConversion = "struct Answer { explicit operator bool() const noexcept(false) { return true; } };\n" +
                    "struct Compare { Answer operator()(int, int) const noexcept { return {}; } };\n" +
                    $"void Test(rocks::Span<int> values) {{ Compare compare; (void)rocks::algo::{operation}({arguments}); }}";
                rejected = await Compile("throwing_conversion_" + operation, throwingConversion);
                Verify.That(rejected.ExitCode != 0, operation + " accepted a throwing bool conversion");
                Verify.Contains(diagnostic, rejected.StdOut + rejected.StdErr, operation + " throwing conversion diagnostic");
            }

            // 양방향 비교가 독립적으로 검증되는지 확인한다. 어느 한쪽만 예외 가능하다.
            string leftThrows = """
                struct Key {};
                struct Compare {
                    bool operator()(int, const Key&) const noexcept(false) { return false; }
                    bool operator()(const Key&, int) const noexcept { return false; }
                };
                void Test(rocks::Span<int> values) { (void)rocks::algo::BinarySearch(values, Key{}, Compare{}); }
                """;
            ProcessResult left = await Compile("binary_search_element_key_throws", leftThrows);
            Verify.That(left.ExitCode != 0, "BinarySearch accepted a throwing element-to-key comparator");
            Verify.Contains("LowerBound comparator must be noexcept", left.StdOut + left.StdErr,
                "BinarySearch element-to-key diagnostic");

            string rightThrows = """
                struct Key {};
                struct Compare {
                    bool operator()(int, const Key&) const noexcept { return false; }
                    bool operator()(const Key&, int) const noexcept(false) { return false; }
                };
                void Test(rocks::Span<int> values) { (void)rocks::algo::BinarySearch(values, Key{}, Compare{}); }
                """;
            ProcessResult right = await Compile("binary_search_key_element_throws", rightThrows);
            Verify.That(right.ExitCode != 0, "BinarySearch accepted a throwing key-to-element comparator");
            Verify.Contains("BinarySearch comparator must be noexcept", right.StdOut + right.StdErr,
                "BinarySearch key-to-element diagnostic");

            ProcessResult constSort = await Compile("const_sort", "void Test(rocks::Span<const int> values) { rocks::algo::Sort(values); }");
            Verify.That(constSort.ExitCode != 0, "Sort accepted immutable elements");
            Verify.Contains("Sort requires mutable elements", constSort.StdOut + constSort.StdErr, "const Sort diagnostic");
        });
    }
}
