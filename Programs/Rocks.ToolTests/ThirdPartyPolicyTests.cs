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
    internal const string Library = "Core/Source/ThirdParty/Fake";
    internal const string Upstream = Library + "/fake-1.0";

    public static void Register(TestSuite suite, Options options)
    {
        suite.Add("Policy.RepositoryThirdParty", () =>
        {
            NoFailures(PolicyChecks.ThirdParty(options.Root));
            foreach (string library in new[] { "Lz4", "Zstd", "Zlib" })
            {
                var manifest = ThirdPartyManifest.Read(Path.Combine(options.Root, ThirdPartyManifest.ThirdPartyRoot, library));
                Verify.That(manifest.TranslationUnits.Count != 0 && manifest.Licenses[0].File.EndsWith("LICENSE", StringComparison.Ordinal),
                    library + ": manifest must list translation units and a license file");
            }
            // zstd 사전 생성기의 divsufsort(MIT)는 본체(BSD-3-Clause) LICENSE에 없으므로 두 번째 원문으로 고지한다.
            var zstd = ThirdPartyManifest.Read(Path.Combine(options.Root, ThirdPartyManifest.ThirdPartyRoot, "Zstd"));
            Verify.That(zstd.Licenses.Select(license => license.Spdx).SequenceEqual(["BSD-3-Clause", "MIT"]) &&
                        zstd.Licenses[1] is { File: "zstd-1.5.7/lib/dictBuilder/divsufsort.h", BinaryNoticeRequired: true },
                "Zstd: the MIT divsufsort notice must be listed after the BSD-3-Clause license");
        });
        RegisterModifications(suite);
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
        suite.Add("Policy.ThirdPartyDataLibrary", () =>
        {
            // 데이터 전용 라이브러리: 모듈 규칙·번역 단위·지시문 없이 파일 집합·SHA-256·라이선스만 검사한다.
            using (TemporaryDirectory fixture = new())
            {
                WriteDataFixture(fixture, manifest => manifest);
                NoFailures(PolicyChecks.ThirdParty(fixture.Root));
                var manifest = ThirdPartyManifest.Read(fixture.FilePath(DataLibrary));
                Verify.That(manifest.Kind == ThirdPartyKind.Data && manifest.EmbeddedIn.SequenceEqual(["Core"]) &&
                            manifest.Licenses.Count == 1 &&
                            manifest.Licenses[0].LicenseSource?.Url == "https://example.invalid/license.txt",
                    "data manifest fields were not read");
                // 데이터 파일 안의 '#pragma' 글자는 지시문 검사 대상이 아니고, 갱신 도구도 지시문을 기록하지 않는다.
                fixture.Write(DataUpstream + "/table.txt", "#pragma warning(disable : 4100)\n0041;LATIN CAPITAL LETTER A\n");
                WriteDataFixture(fixture, manifest => manifest);
                NoFailures(PolicyChecks.ThirdParty(fixture.Root));
                Verify.Equal(0, ThirdPartyManifestRefresh.Run(fixture.Root, write: false).Count,
                    "refresh must keep a data manifest unchanged (no pragma records)");
            }

            ExpectDataFailure("module rule in data library", fixture => fixture.Write(DataLibrary + "/FakeData.Build.cs", "// rule\n"));
            ExpectDataFailure("data file changed", fixture => File.AppendAllText(fixture.FilePath(DataUpstream + "/table.txt"), "x"));
            ExpectDataFailure("unlisted data file", fixture => fixture.Write(DataUpstream + "/extra.txt", "x\n"));
            foreach ((string label, string from, string to) in new[]
                     {
                         ("translation unit in data library", "\"translationUnits\": []", "\"translationUnits\": [ \"fakedata-1.0/table.txt\" ]"),
                         ("define in data library", "\"defines\": []", "\"defines\": [ \"X=1\" ]"),
                         ("unknown kind", "\"kind\": \"data\"", "\"kind\": \"binary\""),
                         ("invalid embedding module", "\"Core\"", "\"Core Module\""),
                         ("licenseSource without https", "https://example.invalid/license.txt", "http://example.invalid/license.txt"),
                         ("licenseSource uppercase hash", "\"sha256\": \"" + new string('a', 64) + "\"", "\"sha256\": \"" + new string('A', 64) + "\"")
                     })
            {
                ExpectDataFailure(label, fixture => WriteDataFixture(fixture, manifest => Replace(manifest, from, to)));
            }
            // code 라이브러리는 embeddedIn을 쓰지 않는다(모듈 자신이 라이브러리다).
            ExpectFailure("embeddedIn in code library", fixture =>
                WriteManifest(fixture, manifest => Replace(manifest, "\"schema\": 1,", "\"schema\": 1, \"embeddedIn\": [ \"Core\" ],")));
        });
        suite.Add("Policy.ThirdPartyLicenses", () =>
        {
            // 라이선스가 여럿인 code 라이브러리: 하위 구성요소의 원문(최상위와 이름이 같은 LICENSE)과 항목별 licenseSource.
            using (TemporaryDirectory fixture = new())
            {
                WriteMultiLicenseFixture(fixture);
                NoFailures(PolicyChecks.ThirdParty(fixture.Root));
                var manifest = ThirdPartyManifest.Read(fixture.FilePath(Library));
                Verify.Equal(2, manifest.Licenses.Count, "license entry count");
                Verify.That(manifest.Licenses[0] is { Spdx: "BSD-2-Clause", File: "fake-1.0/LICENSE", BinaryNoticeRequired: true, LicenseSource: null },
                    "first license entry was not read");
                Verify.That(manifest.Licenses[1] is { Spdx: "MIT", UpstreamExpression: "MIT (component)", File: "fake-1.0/sub/LICENSE", BinaryNoticeRequired: false } &&
                            manifest.Licenses[1].LicenseSource == new ThirdPartyFileSource("https://example.invalid/sub/LICENSE", new string('b', 64)),
                    "second license entry and its licenseSource were not read");
            }

            object license = DefaultLicense;
            ExpectFailure("empty licenses array", fixture => WriteManifest(fixture, manifest => manifest, Array.Empty<object>()));
            ExpectFailure("licenses is an object", fixture => WriteManifest(fixture, manifest => manifest, license));
            ExpectFailure("unknown license entry field", fixture => WriteManifest(fixture, manifest => manifest, new object[]
            {
                new { spdx = "BSD-2-Clause", upstreamExpression = "BSD-2-Clause", file = "fake-1.0/LICENSE", binaryNoticeRequired = true, url = "https://example.invalid" }
            }));
            ExpectFailure("missing license entry field", fixture => WriteManifest(fixture, manifest => manifest, new object[]
            {
                new { spdx = "BSD-2-Clause", upstreamExpression = "BSD-2-Clause", file = "fake-1.0/LICENSE" }
            }));
            ExpectFailure("duplicate license file", fixture => WriteManifest(fixture, manifest => manifest, new[] { license, license }));
            ExpectFailure("duplicate license file differing only by case", fixture => WriteManifest(fixture, manifest => manifest, new[]
            {
                license, new { spdx = "MIT", upstreamExpression = "MIT", file = "fake-1.0/license", binaryNoticeRequired = true }
            }));
            ExpectFailure("second license file is not a listed upstream file", fixture => WriteManifest(fixture, manifest => manifest, new[]
            {
                license, new { spdx = "MIT", upstreamExpression = "MIT", file = "fake-1.0/sub/LICENSE", binaryNoticeRequired = true }
            }));
            ExpectFailure("license file outside upstream directory", fixture => WriteManifest(fixture, manifest => manifest, new[]
            {
                license, new { spdx = "MIT", upstreamExpression = "MIT", file = "Glue.c", binaryNoticeRequired = true }
            }));
            ExpectFailure("license entry licenseSource without https", fixture =>
            {
                WriteMultiLicenseFixture(fixture);
                string manifestPath = fixture.FilePath(Library + "/ThirdParty.json");
                File.WriteAllText(manifestPath, Replace(File.ReadAllText(manifestPath), "https://example.invalid/sub/LICENSE", "http://example.invalid/sub/LICENSE"));
            });
            ExpectFailure("license entry licenseSource with unknown field", fixture => WriteManifest(fixture, manifest => manifest, new object[]
            {
                new
                {
                    spdx = "BSD-2-Clause", upstreamExpression = "BSD-2-Clause", file = "fake-1.0/LICENSE", binaryNoticeRequired = true,
                    licenseSource = new { url = "https://example.invalid/LICENSE", sha256 = new string('b', 64), size = 1 }
                }
            }));
            // 이전 schema(단일 license 객체·최상위 licenseSource)는 호환하지 않는다.
            ExpectFailure("old single license object", fixture =>
                WriteManifest(fixture, manifest => Replace(manifest, "\"licenses\": {", "\"license\": {"), license));
            ExpectFailure("top-level licenseSource", fixture => WriteManifest(fixture, manifest => Replace(manifest, "\"schema\": 1,",
                "\"schema\": 1, \"licenseSource\": { \"url\": \"https://example.invalid/LICENSE\", \"sha256\": \"" + new string('b', 64) + "\" },")));
        });
        suite.Add("Policy.ThirdPartySdkLicenses", () =>
        {
            // SDK 내보내기의 라이선스 복사: code 라이브러리는 모든 원문을 원본 디렉터리 기준 경로로, data 라이브러리는
            // embeddedIn 모듈이 SDK에 있을 때만 복사한다.
            using TemporaryDirectory fixture = new();
            WriteMultiLicenseFixture(fixture);
            WriteDataFixture(fixture, manifest => manifest);
            NoFailures(PolicyChecks.ThirdParty(fixture.Root));

            string codeOnly = fixture.FilePath("sdk-code");
            IReadOnlyList<string> copied = ThirdPartyManifest.CopySdkLicenses(fixture.Root, codeOnly, ["Fake"]);
            Verify.That(copied.SequenceEqual(["Licenses/Fake/LICENSE", "Licenses/Fake/sub/LICENSE"]),
                "code module licenses: " + string.Join(", ", copied));
            Verify.Equal(File.ReadAllText(fixture.FilePath(Upstream + "/LICENSE")), File.ReadAllText(Path.Combine(codeOnly, "Licenses/Fake/LICENSE")),
                "top-level license content");
            Verify.Equal(File.ReadAllText(fixture.FilePath(Upstream + "/sub/LICENSE")), File.ReadAllText(Path.Combine(codeOnly, "Licenses/Fake/sub/LICENSE")),
                "component license content (same file name must not overwrite)");
            Verify.Equal(2, Directory.GetFiles(codeOnly, "*", SearchOption.AllDirectories).Length, "data license copied without its embedding module");

            string withCore = fixture.FilePath("sdk-core");
            copied = ThirdPartyManifest.CopySdkLicenses(fixture.Root, withCore, ["Core", "Fake"]);
            Verify.That(copied.SequenceEqual(["Licenses/Fake/LICENSE", "Licenses/Fake/sub/LICENSE", "Licenses/FakeData/license.txt"]),
                "code and embedded data licenses: " + string.Join(", ", copied));

            // 모듈 이름은 대소문자를 구분하고, 담지 않은 모듈의 원문은 복사하지 않는다.
            Verify.Equal(0, ThirdPartyManifest.CopySdkLicenses(fixture.Root, fixture.FilePath("sdk-none"), ["fake", "Other"]).Count,
                "license copied for a module the SDK does not contain");
            Verify.That(!Directory.Exists(fixture.FilePath("sdk-none")), "empty license copy created the destination");

            // 시험 전용 데이터(embeddedIn 없음)는 Core를 담아도 배포하지 않는다.
            WriteDataFixture(fixture, manifest => Replace(manifest, "  \"embeddedIn\": [\n    \"Core\"\n  ],\n", ""));
            NoFailures(PolicyChecks.ThirdParty(fixture.Root));
            Verify.Equal(0, ThirdPartyManifest.CopySdkLicenses(fixture.Root, fixture.FilePath("sdk-test-data"), ["Core"]).Count,
                "test-only data license was copied");
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
                {
                    Verify.Equal(0, count, label);
                }
                else
                {
                    Verify.That(count != 0, "language mismatch accepted: " + label);
                }
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

    // ---- 추적 가능한 수정(modifications·patch)과 아키텍처별 정의·라이브러리 컴파일 인자 ----

    private const string PatchedLine = "int FakePatched(void) { return 2; }";
    private const string AddedHeader = "#define FAKE_GENERATED 1\n";

    // fake.c 끝에 한 줄을 더하고 upstream에 없던 generated.h를 추가한 fixture다. patch는 git diff 형식이다.
    private static string FixturePatch()
    {
        int lines = FakeSource.Count(value => value == '\n');
        return $"diff --git a/fake.c b/fake.c\nindex 1111111..2222222 100644\n--- a/fake.c\n+++ b/fake.c\n" +
               $"@@ -{lines},0 +{lines + 1} @@\n+{PatchedLine}\n" +
               "diff --git a/generated.h b/generated.h\nnew file mode 100644\nindex 0000000..3333333\n--- /dev/null\n" +
               "+++ b/generated.h\n@@ -0,0 +1 @@\n+#define FAKE_GENERATED 1\n";
    }

    private static string ModificationFields(string fakeUpstreamSha) =>
        "\"modifications\": [" +
        $"{{\"file\": \"fake.c\", \"upstreamSha256\": \"{fakeUpstreamSha}\", \"reason\": \"fixture warning fix\"}}, " +
        "{\"file\": \"generated.h\", \"upstreamSha256\": null, \"reason\": \"pre-generated header\"}], \"patch\": \"Rocks.patch\", ";

    internal static void WriteModifiedFixture(TemporaryDirectory fixture, string? patch = null, string? upstreamSha = null)
    {
        WriteFixture(fixture);
        fixture.Write(Upstream + "/fake.c", FakeSource + PatchedLine + "\n");
        fixture.Write(Upstream + "/generated.h", AddedHeader);
        fixture.Write(Library + "/Rocks.patch", patch ?? FixturePatch());
        WriteManifest(fixture, manifest => Replace(manifest, "\"notes\":", ModificationFields(upstreamSha ?? Sha(FakeSource)) + "\"notes\":"));
    }

    internal static void RegisterModifications(TestSuite suite)
    {
        suite.Add("Policy.ThirdPartyModificationsReverseToUpstream", () =>
        {
            using (TemporaryDirectory fixture = new())
            {
                WriteModifiedFixture(fixture);
                NoFailures(PolicyChecks.ThirdParty(fixture.Root));
                var manifest = ThirdPartyManifest.Read(fixture.FilePath(Library));
                Verify.That(manifest.Patch == "Rocks.patch" && manifest.Modifications.Count == 2 &&
                            manifest.Modifications[1] is { File: "generated.h", UpstreamSha256: null }, "modifications parsed");
            }

            void Expect(string label, Action<TemporaryDirectory> inject)
            {
                using TemporaryDirectory fixture = new();
                WriteModifiedFixture(fixture);
                inject(fixture);
                Verify.That(PolicyChecks.ThirdParty(fixture.Root).Count != 0, "modification defect escaped audit: " + label);
            }

            // 현재 파일만 더 바꾸고 files 해시를 맞춘다: patch 역적용만이 잡아야 한다.
            Expect("extra change outside the patch", fixture =>
            {
                fixture.Write(Upstream + "/fake.c", "// extra\n" + FakeSource + PatchedLine + "\n");
                WriteManifest(fixture, manifest => Replace(manifest, "\"notes\":", ModificationFields(Sha(FakeSource)) + "\"notes\":"));
            });
            Expect("wrong upstream hash", fixture =>
                WriteManifest(fixture, manifest => Replace(manifest, "\"notes\":", ModificationFields(new string('a', 64)) + "\"notes\":")));
            Expect("patch touches an unlisted file", fixture =>
                fixture.Write(Library + "/Rocks.patch", FixturePatch() +
                    "diff --git a/fake.h b/fake.h\nindex 1..2 100644\n--- a/fake.h\n+++ b/fake.h\n@@ -1 +1 @@\n-#pragma once\n+#pragma once\n"));
            Expect("added file content differs", fixture => fixture.Write(Upstream + "/generated.h", "#define FAKE_GENERATED 2\n"));
            Expect("patch file missing", fixture => File.Delete(fixture.FilePath(Library + "/Rocks.patch")));
            Expect("modifications without patch", fixture =>
                WriteManifest(fixture, manifest => Replace(manifest, "\"notes\":",
                    $"\"modifications\": [{{\"file\": \"fake.c\", \"upstreamSha256\": \"{Sha(FakeSource)}\", \"reason\": \"x\"}}], \"notes\":")));
            Expect("CRLF patch", fixture => fixture.Write(Library + "/Rocks.patch", FixturePatch().Replace("\n", "\r\n", StringComparison.Ordinal)));
        });
        suite.Add("Policy.ThirdPartyPatchFinalNewline", () =>
        {
            // 원본 "a\nb"(끝 줄바꿈 없음)에 줄바꿈만 더한 수정을 거꾸로 적용하면 원본 byte가 그대로 나와야 한다.
            string patch = "diff --git a/x.h b/x.h\nindex 1..2 100644\n--- a/x.h\n+++ b/x.h\n@@ -1,2 +1,2 @@\n a\n-b\n" +
                           "\\ No newline at end of file\n+b\n";
            ThirdPartyPatch.FilePatch file = ThirdPartyPatch.Parse(patch).Single();
            byte[]? original = ThirdPartyPatch.Reverse(file, Encoding.UTF8.GetBytes("a\nb\n"));
            Verify.That(original is not null && Encoding.UTF8.GetString(original) == "a\nb", "missing final newline restored");
            foreach (string mismatch in new[] { "a\nb", "a\nc\n", "a\n" })
            {
                try
                {
                    ThirdPartyPatch.Reverse(file, Encoding.UTF8.GetBytes(mismatch));
                    throw new InvalidOperationException("mismatching file reversed: " + mismatch.Replace("\n", "\\n", StringComparison.Ordinal));
                }
                catch (FormatException)
                {
                }
            }
        });
        suite.Add("Policy.ThirdPartyArchitectureDefinesAndCompileArguments", () =>
        {
            using (TemporaryDirectory fixture = new())
            {
                WriteFixture(fixture);
                WriteManifest(fixture, manifest => Replace(Replace(manifest, "\"Windows\": [", "\"Android\": [\"FAKE_ANDROID\"], \"Android.X64\": [\"FAKE_X64\"], \"Android.Arm64\": [\"FAKE_ARM64\"], \"Windows\": ["),
                    "\"notes\":", "\"compileArguments\": [\"-fwrapv\", \"-fno-strict-aliasing\"], \"notes\":"));
                NoFailures(PolicyChecks.ThirdParty(fixture.Root));
                var manifest = ThirdPartyManifest.Read(fixture.FilePath(Library));
                Verify.That(manifest.DefinesFor("Android", "X64").SequenceEqual(["FAKE_CONFIG=1", "FAKE_ANDROID", "FAKE_X64"]) &&
                            manifest.DefinesFor("Android", "Arm64").SequenceEqual(["FAKE_CONFIG=1", "FAKE_ANDROID", "FAKE_ARM64"]) &&
                            manifest.DefinesFor("Windows", "X64").SequenceEqual(["FAKE_CONFIG=1", "FAKE_WINDOWS"]), "platform·architecture defines merged");
                Verify.That(manifest.CompileArguments.SequenceEqual(["-fwrapv", "-fno-strict-aliasing"]), "compile arguments parsed");
            }

            ExpectFailure("unknown architecture key", fixture =>
                WriteManifest(fixture, manifest => Replace(manifest, "\"Windows\": [", "\"Android.X86\": [\"FAKE_X86\"], \"Windows\": [")));
            ExpectFailure("architecture define repeats the platform define", fixture =>
                WriteManifest(fixture, manifest => Replace(manifest, "\"Windows\": [", "\"Android\": [\"FAKE_A=1\"], \"Android.X64\": [\"FAKE_A=2\"], \"Windows\": [")));
            ExpectFailure("disallowed compile argument", fixture =>
                WriteManifest(fixture, manifest => Replace(manifest, "\"notes\":", "\"compileArguments\": [\"-Wno-unused-function\"], \"notes\":")));
            ExpectFailure("duplicate compile argument", fixture =>
                WriteManifest(fixture, manifest => Replace(manifest, "\"notes\":", "\"compileArguments\": [\"-fwrapv\", \"-fwrapv\"], \"notes\":")));
        });
    }

    private const string FakeSource = "#include \"fake.h\"\n#ifdef _MSC_VER\n#pragma warning(disable : 4100)\n#endif\n" +
                                      "static __forceinline int Helper(int value) { return value; }\nint Fake(void) { return Helper(1); }\n";

    private const string FakeHeader = "#pragma once\n#pragma clang diagnostic push\n#pragma clang diagnostic ignored \"-Wshadow\"\n" +
                                      "int Fake(void);\n#pragma clang diagnostic pop\n";

    internal static void WriteFixture(TemporaryDirectory fixture)
    {
        fixture.Write(Upstream + "/LICENSE", "Fake license\n");
        fixture.Write(Upstream + "/fake.c", FakeSource);
        fixture.Write(Upstream + "/fake.h", FakeHeader);
        fixture.Write(Library + "/Glue.c", "#include \"fake.c\"\n");
        fixture.Write(Library + "/Fake.Build.cs", "// fixture\n");
        WriteManifest(fixture, manifest => manifest);
    }

    // fixture code 라이브러리의 기본 라이선스 항목이다(원본 디렉터리의 LICENSE).
    private static readonly object DefaultLicense =
        new { spdx = "BSD-2-Clause", upstreamExpression = "BSD-2-Clause", file = "fake-1.0/LICENSE", binaryNoticeRequired = true };

    // 최상위 LICENSE와 파일 이름이 같은 하위 구성요소 원문(sub/LICENSE, MIT, 항목별 licenseSource)을 더한 code 라이브러리 fixture다.
    private static void WriteMultiLicenseFixture(TemporaryDirectory fixture)
    {
        WriteFixture(fixture);
        fixture.Write(Upstream + "/sub/LICENSE", "Fake component license\n");
        WriteManifest(fixture, manifest => manifest, new[]
        {
            DefaultLicense,
            new
            {
                spdx = "MIT", upstreamExpression = "MIT (component)", file = "fake-1.0/sub/LICENSE", binaryNoticeRequired = false,
                licenseSource = new { url = "https://example.invalid/sub/LICENSE", sha256 = new string('b', 64) }
            }
        });
    }

    // 현재 원본 파일에서 해시·지시문 목록을 다시 계산해 manifest를 쓴다. change는 직렬화된 JSON 문자열을 바꾼다.
    // licenses는 manifest licenses 필드에 그대로 직렬화할 값이다(생략하면 DefaultLicense 하나의 배열).
    private static void WriteManifest(TemporaryDirectory fixture, Func<string, string> change, object? licenses = null)
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
            {
                pragmas.Add(new { file = pragma.File, line = pragma.Line, text = pragma.Text });
            }
        }

        var manifest = new Dictionary<string, object>
        {
            ["schema"] = 1,
            ["name"] = "Fake",
            ["version"] = "1.0",
            ["upstreamDirectory"] = "fake-1.0",
            ["source"] = new { url = "https://example.invalid/fake-1.0.tar.gz", sha256 = new string('0', 64), verification = "fixture" },
            ["licenses"] = licenses ?? new[] { DefaultLicense },
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

    private const string DataLibrary = "Core/Source/ThirdParty/FakeData";
    private const string DataUpstream = DataLibrary + "/fakedata-1.0";

    // 데이터 전용 라이브러리 fixture: 원본 파일 두 개(라이선스 포함)와 kind·embeddedIn·항목별 licenseSource가 있는 manifest.
    private static void WriteDataFixture(TemporaryDirectory fixture, Func<string, string> change)
    {
        if (!File.Exists(fixture.FilePath(DataUpstream + "/license.txt")))
        {
            fixture.Write(DataUpstream + "/license.txt", "Fake data license\n");
            fixture.Write(DataUpstream + "/table.txt", "0041;LATIN CAPITAL LETTER A\n");
        }

        string upstream = fixture.FilePath(DataUpstream);
        var files = new SortedDictionary<string, string>(StringComparer.Ordinal);
        foreach (string path in Directory.EnumerateFiles(upstream, "*", SearchOption.AllDirectories).Order(StringComparer.Ordinal))
        {
            files.Add(Path.GetRelativePath(upstream, path).Replace('\\', '/'), Convert.ToHexStringLower(SHA256.HashData(File.ReadAllBytes(path))));
        }

        var manifest = new Dictionary<string, object>
        {
            ["schema"] = 1,
            ["kind"] = "data",
            ["name"] = "FakeData",
            ["version"] = "1.0",
            ["upstreamDirectory"] = "fakedata-1.0",
            ["source"] = new { url = "https://example.invalid/fakedata-1.0.zip", sha256 = new string('0', 64), verification = "fixture" },
            ["licenses"] = new[]
            {
                new
                {
                    spdx = "Unicode-3.0", upstreamExpression = "Unicode-3.0", file = "fakedata-1.0/license.txt", binaryNoticeRequired = true,
                    licenseSource = new { url = "https://example.invalid/license.txt", sha256 = new string('a', 64) }
                }
            },
            ["embeddedIn"] = new[] { "Core" },
            ["translationUnits"] = Array.Empty<string>(),
            ["includeDirectories"] = Array.Empty<string>(),
            ["defines"] = Array.Empty<string>(),
            ["platformDefines"] = new Dictionary<string, string[]>(),
            ["consumerDefines"] = Array.Empty<string>(),
            ["adapterHeaders"] = Array.Empty<string>(),
            ["files"] = files,
            ["pragmaDirectives"] = Array.Empty<object>(),
            ["notes"] = Array.Empty<string>()
        };
        // 갱신 도구(ThirdPartyManifestRefresh)와 같은 서식으로 써서 갱신이 바꾸지 않는지 볼 수 있게 한다.
        string json = System.Text.Json.Nodes.JsonNode.Parse(JsonSerializer.Serialize(manifest))!.ToJsonString(new JsonSerializerOptions
        {
            WriteIndented = true,
            NewLine = "\n",
            Encoder = System.Text.Encodings.Web.JavaScriptEncoder.UnsafeRelaxedJsonEscaping
        }) + "\n";
        fixture.Write(DataLibrary + "/ThirdParty.json", change(json));
    }

    private static void ExpectDataFailure(string label, Action<TemporaryDirectory> inject)
    {
        using TemporaryDirectory fixture = new();
        WriteDataFixture(fixture, manifest => manifest);
        NoFailures(PolicyChecks.ThirdParty(fixture.Root));
        inject(fixture);
        Verify.That(PolicyChecks.ThirdParty(fixture.Root).Count != 0, "third-party data defect escaped audit: " + label);
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
