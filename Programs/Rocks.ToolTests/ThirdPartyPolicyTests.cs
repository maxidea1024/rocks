using System.Security.Cryptography;
using System.Text;
using System.Text.Json;
using Rocks.Tooling.Checks;
using Rocks.Tooling.Common;

namespace Rocks.ToolTests;

/// <summary>
/// 외부 라이브러리 정책(third-party manifest, 원본 제외 범위, compile DB 언어 검사)의 저장소 검사와 음성 대조다.
/// 음성 대조는 임시 폴더의 가짜 라이브러리에 결함을 하나씩 주입하고 검사가 정확히 그 결함을 잡는지 확인한다.
/// </summary>
internal static class ThirdPartyPolicyTests
{
    private const string Library = "Core/Source/ThirdParty/Fake";
    private const string Upstream = Library + "/fake-1.0";

    public static void Register(TestSuite suite, Options options)
    {
        suite.Add("Policy.RepositoryThirdParty", () =>
        {
            NoFailures(PolicyChecks.ThirdParty(options.Root));
            foreach (string library in new[] { "Lz4", "Zstd", "Zlib" })
            {
                var manifest = ThirdPartyManifest.Read(Path.Combine(options.Root, ThirdPartyManifest.ThirdPartyRoot, library));
                Verify.That(manifest.TranslationUnits.Count != 0 && manifest.License.File.EndsWith("LICENSE", StringComparison.Ordinal),
                    library + ": manifest must list translation units and a license file");
            }
        });
        suite.Add("Policy.ThirdPartyPositiveFixture", () =>
        {
            using TemporaryDirectory temporary = new();
            WriteFixture(temporary);
            NoFailures(PolicyChecks.ThirdParty(temporary.Root));
            // 원본 디렉터리의 억제·강제 인라인·플랫폼 조건은 manifest 경계로 lexical 검사에서 빠진다.
            NoFailures(PolicyChecks.SourceWarnings(temporary.Root));
            NoFailures(PolicyChecks.OptimizationOverrides(temporary.Root));
            NoFailures(PolicyChecks.PlatformBoundary(temporary.Root));
            Verify.That(PolicyChecks.NativeSources(temporary.Root).Any(path => path.EndsWith("Glue.c", StringComparison.Ordinal)),
                "Rocks glue .c must remain an owned native source");
        });
        suite.Add("Policy.ThirdPartyNegativeControls", () =>
        {
            ExpectFailure("1-byte upstream change", fixture =>
                File.AppendAllText(fixture.FilePath(Upstream + "/fake.c"), " "));
            ExpectFailure("unlisted upstream file", fixture => fixture.Write(Upstream + "/extra.h", "#pragma once\n"));
            ExpectFailure("missing listed file", fixture => File.Delete(fixture.FilePath(Upstream + "/fake.h")));
            ExpectFailure("license removed", fixture => File.Delete(fixture.FilePath(Upstream + "/LICENSE")));
            ExpectFailure("CRLF conversion", fixture =>
            {
                string path = fixture.FilePath(Upstream + "/fake.c");
                File.WriteAllText(path, File.ReadAllText(path).Replace("\n", "\r\n", StringComparison.Ordinal));
            });
            ExpectFailure("new unrecorded pragma with updated hash", fixture =>
            {
                // 해시만 새 내용에 맞추고 지시문 목록은 그대로 둔다: 지시문 대조만이 잡아야 한다.
                string changed = FakeSource + "#pragma clang diagnostic ignored \"-Wunused-variable\"\n";
                fixture.Write(Upstream + "/fake.c", changed);
                string manifestPath = fixture.FilePath(Library + "/ThirdParty.json");
                File.WriteAllText(manifestPath, Replace(File.ReadAllText(manifestPath), Sha(FakeSource), Sha(changed)));
            });
            ExpectFailure("adapter header suppression outside push/pop", fixture =>
            {
                fixture.Write(Upstream + "/fake.h", "#pragma once\n#pragma clang diagnostic ignored \"-Wshadow\"\nint Fake(void);\n");
                WriteManifest(fixture, manifest => manifest);
            });
            ExpectFailure("translation unit outside manifest files", fixture =>
                WriteManifest(fixture, manifest => Replace(manifest, "\"fake-1.0/fake.c\"", "\"fake-1.0/missing.c\"")));
            ExpectFailure("unregistered glue file", fixture => fixture.Write(Library + "/Other.c", "int Other(void) { return 0; }\n"));
            ExpectFailure("lookalike upstream directory", fixture => fixture.Write(Library + "/fake-1.0x/fake.c", "int X;\n"));
            ExpectFailure("duplicate manifest field", fixture =>
                WriteManifest(fixture, manifest => Replace(manifest, "\"schema\": 1,", "\"schema\": 1, \"schema\": 1,")));
            ExpectFailure("unknown manifest field", fixture =>
                WriteManifest(fixture, manifest => Replace(manifest, "\"schema\": 1,", "\"schema\": 1, \"extra\": true,")));
            ExpectFailure("uppercase SHA-256", fixture =>
                WriteManifest(fixture, manifest => manifest.Replace(Sha(FakeSource), Sha(FakeSource).ToUpperInvariant(), StringComparison.Ordinal)));
            ExpectFailure("parent path in manifest", fixture =>
                WriteManifest(fixture, manifest => Replace(manifest, "\"fake-1.0/fake.h\"", "\"fake-1.0/../fake-1.0/fake.h\"")));
            ExpectFailure("stray file under ThirdParty", fixture => fixture.Write("Core/Source/ThirdParty/README.txt", "x\n"));

            // 지시문 변형: 줄 이음·BOM·다음 줄의 괄호·주석이 낀 #pragma·system_header·deprecation define도 기록 대상이다.
            foreach (string variant in new[]
                     {
                         "#pragma \\\nclang diagnostic ignored \"-Wshadow\"\n", "﻿#pragma GCC system_header\n",
                         "#define SILENCE _Pragma\n(\"clang diagnostic ignored \\\"-Wshadow\\\"\")\n",
                         "#/* c */pragma warning(disable : 4100)\n", "#define _CRT_SECURE_NO_DEPRECATE\n",
                         "#define M __pragma(warning(disable : 4100))\n"
                     })
            {
                ExpectFailure("unrecorded directive variant " + variant.Trim(), fixture =>
                {
                    // BOM은 실제처럼 파일 맨 앞에 둔다.
                    string changed = variant.StartsWith('﻿') ? variant + FakeSource : FakeSource + variant;
                    fixture.Write(Upstream + "/fake.c", changed);
                    string manifestPath = fixture.FilePath(Library + "/ThirdParty.json");
                    File.WriteAllText(manifestPath, Replace(File.ReadAllText(manifestPath), Sha(FakeSource), Sha(changed)));
                });
            }
            // #pragma once는 진단과 무관하므로 기록하지 않는다.
            Verify.Equal(0, ThirdPartyManifest.ScanPragmaDirectives("a.h", Encoding.UTF8.GetBytes("#pragma once\n")).Count,
                "pragma once must not be recorded");

            // adapter 헤더의 계열이 섞인 push/pop, pack push 안의 억제, 비 adapter 원본 include.
            ExpectFailure("mixed push/pop families in adapter header", fixture =>
            {
                fixture.Write(Upstream + "/fake.h", "#pragma once\n#pragma clang diagnostic push\n" +
                                                     "#pragma clang diagnostic ignored \"-Wshadow\"\n#pragma warning(pop)\nint Fake(void);\n");
                WriteManifest(fixture, manifest => manifest);
            });
            ExpectFailure("suppression inside pack push in adapter header", fixture =>
            {
                fixture.Write(Upstream + "/fake.h", "#pragma once\n#pragma pack(push, 1)\n" +
                                                     "#pragma clang diagnostic ignored \"-Wshadow\"\n#pragma pack(pop)\nint Fake(void);\n");
                WriteManifest(fixture, manifest => manifest);
            });
            ExpectFailure("adapter header includes non-adapter upstream file", fixture =>
            {
                fixture.Write(Upstream + "/internal.h", "#pragma warning(disable : 4100)\n");
                fixture.Write(Upstream + "/fake.h", "#pragma once\n#include \"internal.h\"\nint Fake(void);\n");
                WriteManifest(fixture, manifest => manifest);
            });
            ExpectFailure("owned source includes non-adapter upstream file", fixture =>
            {
                fixture.Write(Upstream + "/internal.h", "#pragma warning(disable : 4100)\n");
                WriteManifest(fixture, manifest => manifest);
                fixture.Write("Core/Source/Runtime/Probe/Private/Probe.cpp", "#include <internal.h>\n");
            });
            using (TemporaryDirectory fixture = new())
            {
                // adapter 헤더 include와 글루의 원본 .c include는 허용한다.
                WriteFixture(fixture);
                fixture.Write("Core/Source/Runtime/Probe/Private/Probe.cpp", "#include <fake.h>\n");
                NoFailures(PolicyChecks.ThirdParty(fixture.Root));
                Verify.That(PolicyChecks.ThirdParty(fixture.Root).All(failure => failure.Rule == "third-party"), "rule id");
            }

            // 원본이 아닌 곳의 억제·강제 인라인은 계속 lexical 검사 대상이다.
            using (TemporaryDirectory fixture = new())
            {
                WriteFixture(fixture);
                fixture.Write(Library + "/fake-1.0x/fake.c", "#pragma warning(disable: 4100)\n");
                Verify.Equal(1, PolicyChecks.SourceWarnings(fixture.Root).Count, "lookalike upstream directory escaped lexical audit");
            }
            using (TemporaryDirectory fixture = new())
            {
                WriteFixture(fixture);
                fixture.Write(Library + "/Glue.c", "#pragma clang diagnostic ignored \"-Wunused-variable\"\n#include \"fake.c\"\n");
                Verify.Equal(1, PolicyChecks.SourceWarnings(fixture.Root).Count, "glue suppression escaped audit");
                fixture.Write(Library + "/Glue.c", "static __forceinline int F(void) { return 0; }\n#include \"fake.c\"\n");
                Verify.Equal(1, PolicyChecks.OptimizationOverrides(fixture.Root).Count, "glue forced inline escaped audit");
            }
            using (TemporaryDirectory fixture = new())
            {
                // manifest가 깨지면 원본도 제외하지 않는다(경계가 조용히 넓어지지 않는다).
                WriteFixture(fixture);
                fixture.Write(Library + "/ThirdParty.json", "{");
                Verify.That(PolicyChecks.ThirdParty(fixture.Root).Count != 0, "broken manifest accepted");
                Verify.That(PolicyChecks.SourceWarnings(fixture.Root).Count != 0, "broken manifest still excluded upstream sources");
            }
        });
        suite.Add("Policy.LanguageArgumentControls", () =>
        {
            string cFile = "/repo/Core/Source/ThirdParty/Zlib/zlib-1.4/inflate.c";
            NoFailures(PolicyChecks.LanguageArguments(["clang-cl", "/W4", "/WX", "/clang:-std=c17", "/c", "/TC", cFile], cFile));
            NoFailures(PolicyChecks.LanguageArguments(["clang++", "-std=c17", "-Wall", "-Wextra", "-Wpedantic", "-Werror", "-c", "-x", "c", cFile], cFile));
            NoFailures(PolicyChecks.LanguageArguments(["clang-cl", "/W4", "/WX", "/clang:-std=c++23", "/c", "/TP", "a.cpp"], "a.cpp"));
            NoFailures(PolicyChecks.LanguageArguments(["clang++", "-std=c++23", "-Werror", "-c", "a.cpp"], "a.cpp"));
            foreach ((string[] arguments, string file, string label) in new (string[], string, string)[]
                     {
                         (["clang-cl", "/WX", "/clang:-std=c17", "/c", "/TP", cFile], cFile, "C source compiled with /TP"),
                         (["clang-cl", "/WX", "/clang:-std=c17", "/c", cFile], cFile, "C source without /TC"),
                         (["clang-cl", "/WX", "/clang:-std=c++23", "/c", "/TC", cFile], cFile, "C source with C++ standard"),
                         (["clang++", "-std=c17", "-Werror", "-c", cFile], cFile, "POSIX C source without -x c"),
                         (["clang-cl", "/WX", "/clang:-std=c17", "/c", "/TC", "/repo/Engine/a.c"], "/repo/Engine/a.c", "C source outside ThirdParty"),
                         (["clang-cl", "/WX", "/clang:-std=c++23", "/c", "/TC", "a.cpp"], "a.cpp", "C++ source with /TC"),
                         (["clang++", "-std=c++23", "-Werror", "-c", "-x", "c", "a.cpp"], "a.cpp", "C++ source with -x c"),
                         (["clang-cl", "/WX", "/c", "/TP", "a.cpp"], "a.cpp", "C++ source without standard"),
                         (["clang-cl", "/WX", "/clang:-std=c++23", "/c", "/TP", "a.C"], "a.C", "uppercase .C is C++ and passes"),
                         (["clang++", "-std=c17", "-std=gnu11", "-Werror", "-c", "-x", "c", cFile], cFile, "later -std overrides C17"),
                         (["clang++", "-std=c17", "-Werror", "-c", "-xc", cFile], cFile, "joined -xc passes"),
                         (["clang++", "-std=c17", "-Werror", "-c", "-x", "c", "-x", "c++", cFile], cFile, "later -x c++ overrides C"),
                         (["clang-cl", "/WX", "/clang:-std=c17", "/c", "/TC", "/TP", cFile], cFile, "later /TP overrides /TC"),
                         (["clang++", "-std=c++23", "-Werror", "-c", "--language=c", "a.cpp"], "a.cpp", "--language=c on C++ source")
                     })
            {
                int count = PolicyChecks.LanguageArguments(arguments, file).Count;
                if (label.EndsWith("passes", StringComparison.Ordinal))
                    Verify.Equal(0, count, label);
                else
                    Verify.That(count != 0, "language mismatch accepted: " + label);
            }

            // 상대 file은 compile DB directory 기준으로 위치를 판정한다.
            NoFailures(PolicyChecks.LanguageArguments(["clang-cl", "/WX", "/clang:-std=c17", "/c", "/TC", "zlib-1.4/inflate.c"],
                "zlib-1.4/inflate.c", "/repo/Core/Source/ThirdParty/Zlib"));
            Verify.Equal("warning-language", PolicyChecks.LanguageArguments(["clang-cl", "/WX", "/c", "/TP", "a.c"], "a.c")[0].Rule,
                "language rule id");

            // compile DB 경로: C 항목에도 같은 경고 정책을 적용한다.
            using TemporaryDirectory temporary = new();
            string database = temporary.FilePath("compile_commands.json");
            temporary.Write("compile_commands.json", JsonSerializer.Serialize(new[]
            {
                new { file = cFile, arguments = new[] { "clang-cl", "/W4", "/WX", "/clang:-std=c17", "/c", "/TC", cFile } }
            }));
            NoFailures(PolicyChecks.CompilationDatabase(database));
            temporary.Write("compile_commands.json", JsonSerializer.Serialize(new[]
            {
                new { file = cFile, arguments = new[] { "clang-cl", "/W4", "/WX", "/clang:-std=c17", "/clang:-Wno-unused-function", "/c", "/TC", cFile } }
            }));
            Verify.That(PolicyChecks.CompilationDatabase(database).Count != 0, "C entry warning suppression accepted");
            temporary.Write("compile_commands.json", JsonSerializer.Serialize(new[]
            {
                new { file = cFile, arguments = new[] { "clang-cl", "/W4", "/WX", "/clang:-std=c17", "/c", "/TP", cFile } }
            }));
            Verify.That(PolicyChecks.CompilationDatabase(database).Count != 0, "C entry compiled as C++ accepted");
        });
    }

    private const string FakeSource = "#include \"fake.h\"\n#ifdef _MSC_VER\n#pragma warning(disable : 4100)\n#endif\n" +
                                      "static __forceinline int Helper(int value) { return value; }\nint Fake(void) { return Helper(1); }\n";
    private const string FakeHeader = "#pragma once\n#pragma clang diagnostic push\n#pragma clang diagnostic ignored \"-Wshadow\"\n" +
                                      "int Fake(void);\n#pragma clang diagnostic pop\n";

    private static void WriteFixture(TemporaryDirectory fixture)
    {
        fixture.Write(Upstream + "/LICENSE", "Fake license\n");
        fixture.Write(Upstream + "/fake.c", FakeSource);
        fixture.Write(Upstream + "/fake.h", FakeHeader);
        fixture.Write(Library + "/Glue.c", "#include \"fake.c\"\n");
        fixture.Write(Library + "/Fake.Build.cs", "// fixture\n");
        WriteManifest(fixture, manifest => manifest);
    }

    // 현재 원본 파일에서 해시·지시문 목록을 다시 계산해 manifest를 쓴다. change는 직렬화된 JSON 문자열을 바꾼다.
    private static void WriteManifest(TemporaryDirectory fixture, Func<string, string> change)
    {
        string upstream = fixture.FilePath(Upstream);
        var files = new SortedDictionary<string, string>(StringComparer.Ordinal);
        var pragmas = new List<object>();
        foreach (string path in Directory.EnumerateFiles(upstream, "*", SearchOption.AllDirectories).Order(StringComparer.Ordinal))
        {
            string relative = Path.GetRelativePath(upstream, path).Replace('\\', '/');
            byte[] bytes = File.ReadAllBytes(path);
            files.Add(relative, Convert.ToHexStringLower(SHA256.HashData(bytes)));
            foreach (ThirdPartyPragma pragma in ThirdPartyManifest.ScanPragmaDirectives(relative, bytes))
                pragmas.Add(new { file = pragma.File, line = pragma.Line, text = pragma.Text });
        }
        var manifest = new Dictionary<string, object>
        {
            ["schema"] = 1,
            ["name"] = "Fake",
            ["version"] = "1.0",
            ["upstreamDirectory"] = "fake-1.0",
            ["source"] = new { url = "https://example.invalid/fake-1.0.tar.gz", sha256 = new string('0', 64), verification = "fixture" },
            ["license"] = new { spdx = "BSD-2-Clause", upstreamExpression = "BSD-2-Clause", file = "fake-1.0/LICENSE", binaryNoticeRequired = true },
            ["translationUnits"] = new[] { "Glue.c", "fake-1.0/fake.c" },
            ["includeDirectories"] = new[] { "fake-1.0" },
            ["defines"] = new[] { "FAKE_CONFIG=1" },
            ["platformDefines"] = new Dictionary<string, string[]> { ["Windows"] = ["FAKE_WINDOWS"] },
            ["consumerDefines"] = Array.Empty<string>(),
            ["adapterHeaders"] = new[] { "fake-1.0/fake.h" },
            ["files"] = files,
            ["pragmaDirectives"] = pragmas,
            ["notes"] = Array.Empty<string>()
        };
        string json = JsonSerializer.Serialize(manifest, new JsonSerializerOptions { WriteIndented = true });
        fixture.Write(Library + "/ThirdParty.json", change(json));
    }

    private static void ExpectFailure(string label, Action<TemporaryDirectory> inject)
    {
        using TemporaryDirectory fixture = new();
        WriteFixture(fixture);
        NoFailures(PolicyChecks.ThirdParty(fixture.Root));
        inject(fixture);
        Verify.That(PolicyChecks.ThirdParty(fixture.Root).Count != 0, "third-party defect escaped audit: " + label);
    }

    private static string Replace(string text, string oldValue, string newValue)
    {
        Verify.That(text.Contains(oldValue, StringComparison.Ordinal), "fixture replacement target missing: " + oldValue);
        return text.Replace(oldValue, newValue, StringComparison.Ordinal);
    }

    private static string Sha(string text) => Convert.ToHexStringLower(SHA256.HashData(Encoding.UTF8.GetBytes(text)));

    private static void NoFailures(IReadOnlyList<CheckFailure> failures) => Verify.That(failures.Count == 0, string.Join('\n', failures));
}
