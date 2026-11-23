using System.Security.Cryptography;
using System.Text;
using System.Text.Json;
using System.Text.Json.Nodes;
using Rocks.Tooling.Checks;
using Rocks.Tooling.Common;

namespace Rocks.ToolTests;

/// <summary>
/// 외부 라이브러리 Engine 루트와 headers·binary 종류의 manifest·정책 시험이다(Docs/Design/ShaderSystem 8절 1–3항).
/// 음성 대조는 정상 fixture에 결함을 하나씩 넣고 리더나 정책 검사가 잡는지 본다.
/// </summary>
internal static class ThirdPartyKindTests
{
    private const string EngineLibrary = "Engine/Source/ThirdParty/FakeEngine";

    private const string HeadersLibrary = "Engine/Source/ThirdParty/FakeHeaders";

    private const string HeadersUpstream = HeadersLibrary + "/fakeheaders-1.0";

    private const string BinaryLibrary = "Engine/Source/ThirdParty/FakeTool";

    private const string BinaryUpstream = BinaryLibrary + "/faketool-2.0";

    private const string SdkLibrary = "Engine/Source/ThirdParty/FakeSdkTool";

    private const string SystemLibrary = "Engine/Source/ThirdParty/FakeSystemTool";

    private static readonly string HashA = new('a', 64);

    private static readonly string HashB = new('b', 64);

    private static readonly string Commit = new('c', 40);

    public static void Register(TestSuite suite)
    {
        suite.Add("Policy.ThirdPartyEngineRoot", EngineRoot);
        suite.Add("Policy.ThirdPartyHeadersLibrary", HeadersKind);
        suite.Add("Policy.ThirdPartyBinaryLibrary", BinaryKind);
        suite.Add("Policy.ThirdPartyBinarySdkAndSystem", SdkAndSystemKinds);
    }

    private static void EngineRoot()
    {
        using TemporaryDirectory fixture = new();
        ThirdPartyPolicyTests.WriteFixture(fixture);
        // 같은 code fixture를 Engine 루트에 다른 이름으로 둔다.
        CopyLibrary(fixture, ThirdPartyPolicyTests.Library, EngineLibrary, "Fake", "FakeEngine");
        NoFailures(PolicyChecks.ThirdParty(fixture.Root));
        Verify.Equal(EngineLibrary, ThirdPartyManifest.FindLibrary(fixture.Root, "FakeEngine"), "Engine library lookup");
        Verify.Equal(ThirdPartyPolicyTests.Library, ThirdPartyManifest.FindLibrary(fixture.Root, "Fake"), "Core library lookup");
        Verify.Throws<FormatException>(() => ThirdPartyManifest.FindLibrary(fixture.Root, "Missing"));
        Verify.That(ThirdPartyManifest.LibraryDirectories(fixture.Root).Select(Path.GetFileName).SequenceEqual(["Fake", "FakeEngine"]),
            "library directories must follow root order");
        // Engine 루트 원본도 lexical 검사에서 빠지고 원본 디렉터리 경계는 manifest로 정해진다.
        NoFailures(PolicyChecks.SourceWarnings(fixture.Root));
        IReadOnlyList<string> owned = PolicyChecks.NativeSources(fixture.Root).Select(path => path.Replace('\\', '/')).ToArray();
        Verify.That(!owned.Any(path => path.EndsWith(EngineLibrary + "/fake-1.0/fake.c", StringComparison.Ordinal)),
            "Engine upstream directory not excluded from owned sources");
        Verify.That(owned.Any(path => path.EndsWith(EngineLibrary + "/Glue.c", StringComparison.Ordinal)),
            "Engine glue .c must remain an owned native source");
        // compile DB 언어 검사: Engine 루트의 .c는 받고 루트 밖의 .c는 거부한다.
        string engineC = "/repo/" + EngineLibrary + "/fake-1.0/fake.c";
        NoFailures(PolicyChecks.LanguageArguments(["clang-cl", "/W4", "/WX", "/clang:-std=c17", "/c", "/TC", engineC], engineC));
        string outside = "/repo/Engine/Source/Runtime/Probe/Private/fake.c";
        Verify.That(PolicyChecks.LanguageArguments(["clang-cl", "/W4", "/WX", "/clang:-std=c17", "/c", "/TC", outside], outside).Count != 0,
            "C source outside third-party roots accepted");
        // SDK 라이선스 복사도 두 루트를 본다.
        Verify.That(ThirdPartyManifest.CopySdkLicenses(fixture.Root, fixture.FilePath("sdk"), ["FakeEngine"]).SequenceEqual(["Licenses/FakeEngine/LICENSE"]),
            "Engine code library license copy");

        // 음성 대조: 두 루트에 같은 이름, Engine 루트의 떠도는 파일.
        using (TemporaryDirectory duplicate = new())
        {
            ThirdPartyPolicyTests.WriteFixture(duplicate);
            CopyLibrary(duplicate, ThirdPartyPolicyTests.Library, "Engine/Source/ThirdParty/Fake", "Fake", "Fake");
            Verify.That(PolicyChecks.ThirdParty(duplicate.Root).Any(failure => failure.Message.Contains("unique", StringComparison.Ordinal)),
                "library name duplicated across roots accepted");
            Verify.Throws<FormatException>(() => ThirdPartyManifest.FindLibrary(duplicate.Root, "Fake"));
        }

        fixture.Write("Engine/Source/ThirdParty/README.txt", "x\n");
        Verify.That(PolicyChecks.ThirdParty(fixture.Root).Count != 0, "stray file under Engine ThirdParty accepted");
    }

    private static void HeadersKind()
    {
        using (TemporaryDirectory fixture = new())
        {
            WriteHeadersFixture(fixture, manifest => manifest);
            NoFailures(PolicyChecks.ThirdParty(fixture.Root));
            ThirdPartyManifest manifest = ThirdPartyManifest.Read(fixture.FilePath(HeadersLibrary));
            Verify.That(manifest is { Kind: ThirdPartyKind.Headers, TranslationUnits.Count: 0 } &&
                        manifest.IncludeDirectories.SequenceEqual(["fakeheaders-1.0/include"]) && manifest.PragmaDirectives.Count == 2,
                "headers manifest fields were not read");
            // 헤더의 지시문은 엔진 번역 단위가 include하므로 기록·검사한다. 갱신 도구도 같은 결과를 낸다.
            Verify.Equal(0, ThirdPartyManifestRefresh.Run(fixture.Root, write: false).Count, "refresh must keep a headers manifest unchanged");
            // 소비 모듈 소스는 adapter 헤더를 include할 수 있다.
            fixture.Write("Programs/Probe/Private/Probe.cpp", "#include <fake/fake_core.h>\n");
            NoFailures(PolicyChecks.ThirdParty(fixture.Root));
        }

        ExpectHeadersFailure("module rule in headers library", fixture => fixture.Write(HeadersLibrary + "/FakeHeaders.Build.cs", "// rule\n"));
        ExpectHeadersFailure("header digest mismatch", fixture => File.AppendAllText(fixture.FilePath(HeadersUpstream + "/include/fake/fake_core.h"), " "));
        ExpectHeadersFailure("unrecorded pragma", fixture =>
        {
            string path = fixture.FilePath(HeadersUpstream + "/include/fake/fake_core.h");
            File.AppendAllText(path, "#pragma clang diagnostic ignored \"-Wshadow\"\n");
            // 해시만 맞추고 지시문 목록은 그대로 둔다.
            WriteHeadersFixture(fixture, manifest => manifest, recordPragmas: false);
        });
        ExpectHeadersFailure("owned source includes non-adapter header", fixture =>
            fixture.Write("Programs/Probe/Private/Probe.cpp", "#include <fake/fake_internal.h>\n"));
        foreach ((string label, string from, string to) in new[]
                 {
                     ("translation unit in headers library", "\"translationUnits\": []", "\"translationUnits\": [ \"fakeheaders-1.0/x.c\" ]"),
                     ("define in headers library", "\"defines\": []", "\"defines\": [ \"X=1\" ]"),
                     ("no include directory", "\"includeDirectories\": [\n    \"fakeheaders-1.0/include\"\n  ]", "\"includeDirectories\": []"),
                     ("compileArguments in headers library", "\"kind\": \"headers\"", "\"kind\": \"headers\", \"compileArguments\": [ \"-fwrapv\" ]"),
                     ("unknown field", "\"kind\": \"headers\"", "\"kind\": \"headers\", \"hosts\": {}"),
                     ("unknown kind", "\"kind\": \"headers\"", "\"kind\": \"header\"")
                 })
        {
            ExpectHeadersFailure(label, fixture => WriteHeadersFixture(fixture, manifest => Replace(manifest, from, to)));
        }
    }

    private static void BinaryKind()
    {
        using (TemporaryDirectory fixture = new())
        {
            WriteBinaryFixture(fixture, manifest => { });
            NoFailures(PolicyChecks.ThirdParty(fixture.Root));
            ThirdPartyManifest manifest = ThirdPartyManifest.Read(fixture.FilePath(BinaryLibrary));
            ThirdPartyBinary binary = manifest.Binary!;
            Verify.That(manifest.Kind == ThirdPartyKind.Binary && binary.Origin == ThirdPartyBinaryOrigin.Build && manifest.HasUpstream,
                "binary manifest kind and origin");
            ThirdPartyHost host = binary.Host("windows-x64");
            Verify.That(host.StoreKey == HashA && host.Archive!.FileName == "FakeTool-2.0-r3-windows-x64.tar.gz" &&
                        host.Files.Select(file => (file.Path, file.Role)).SequenceEqual([("bin/fake.dll", ThirdPartyFileRole.Runtime),
                            ("bin/fake.pdb", ThirdPartyFileRole.Symbols), ("LICENSE", ThirdPartyFileRole.Notice)]),
                "binary host files were not read");
            Verify.That(binary.Build is { Tag: "v2.0", Revision: 3, Dependencies.Count: 1, Scripts.Count: 2 } &&
                        binary.Build.Dependencies[0].Path == "external/dep" &&
                        ThirdPartyToolBuild.ReleaseTag("FakeTool", "2.0", 3) == "third-party-FakeTool-2.0-r3",
                "binary build inputs were not read");
            // 호스트 누락: manifest가 적지 않은 호스트는 다른 호스트 산출물로 대신하지 않고 오류다.
            Verify.Throws<FormatException>(() => binary.Host("macos-arm64"));
            // binary는 SDK가 내보내지 않는다(워커·도구 내보내기는 후속).
            Verify.Equal(0, ThirdPartyManifest.CopySdkLicenses(fixture.Root, fixture.FilePath("sdk"), ["FakeTool"]).Count, "binary license copied to SDK");
            Verify.Equal(0, ThirdPartyManifestRefresh.Run(fixture.Root, write: false).Count, "refresh must keep a binary manifest unchanged");
        }

        // 정책 음성 대조(파일).
        ExpectBinaryFailure("committed header digest mismatch", fixture =>
            File.AppendAllText(fixture.FilePath(BinaryUpstream + "/include/fake_api.h"), " "));
        ExpectBinaryFailure("module rule in binary library", fixture => fixture.Write(BinaryLibrary + "/FakeTool.Build.cs", "// rule\n"));
        ExpectBinaryFailure("unlisted tool build file", fixture => fixture.Write(BinaryLibrary + "/Build/Extra.ps1", "# x\n"));
        ExpectBinaryFailure("missing tool build script", fixture => File.Delete(fixture.FilePath(BinaryLibrary + "/Build/Build.sh")));
        ExpectBinaryFailure("stray file in binary library", fixture => fixture.Write(BinaryLibrary + "/fake.dll", "x"));

        // manifest 음성 대조(리더).
        foreach ((string label, Action<JsonObject> change) in new (string, Action<JsonObject>)[]
                 {
                     ("unknown top-level field", manifest => manifest["translationUnits"] = new JsonArray()),
                     ("unknown host field", manifest => Host(manifest)["extra"] = 1),
                     ("unknown host file field", manifest => HostFile(manifest)["mode"] = "0755"),
                     ("unknown build field", manifest => manifest["build"]!["repository"] = "https://example.invalid/x"),
                     ("unknown host key", manifest => Move(manifest["hosts"]!.AsObject(), "windows-x64", "linux-x64")),
                     ("no hosts", manifest => manifest["hosts"] = new JsonObject()),
                     ("archive asset name breaks the naming rule", manifest =>
                         Host(manifest)["archive"]!["url"] = "https://example.invalid/releases/download/x/fake.tar.gz"),
                     ("archive digest uppercase", manifest => Host(manifest)["archive"]!["sha256"] = HashA.ToUpperInvariant()),
                     ("host file digest malformed", manifest => HostFile(manifest)["sha256"] = "abc"),
                     ("host file without digest", manifest => HostFile(manifest).Remove("sha256")),
                     ("unknown role", manifest => HostFile(manifest)["role"] = "import"),
                     ("duplicate host file differing only by case", manifest =>
                         Host(manifest)["files"]!.AsArray().Add(JsonNode.Parse("{\"path\": \"BIN/fake.dll\", \"sha256\": \"" + HashB + "\", \"size\": 1, \"role\": \"runtime\"}"))),
                     ("negative size", manifest => HostFile(manifest)["size"] = -1),
                     ("http archive", manifest => Host(manifest)["archive"]!["url"] = "http://example.invalid/FakeTool-2.0-r3-windows-x64.tar.gz"),
                     ("short commit", manifest => manifest["build"]!["source"]!["commit"] = "abc123"),
                     ("revision zero", manifest => manifest["build"]!["revision"] = 0),
                     ("script outside Build", manifest => manifest["build"]!["scripts"]![0] = "Build.ps1"),
                     ("missing committed headers", manifest => manifest.Remove("files")),
                     ("both build and sdk", manifest => manifest["sdk"] = JsonNode.Parse("{\"name\": \"Windows SDK\", \"version\": \"10.0\"}")),
                     ("status other than system", manifest => manifest["status"] = "prebuilt"),
                     ("minimumVersion on a built binary", manifest => manifest["minimumVersion"] = "1.0"),
                     ("version with spaces", manifest => manifest["version"] = "2.0 beta")
                 })
        {
            ExpectBinaryFailure(label, fixture => WriteBinaryFixture(fixture, change));
        }
    }

    private static void SdkAndSystemKinds()
    {
        using (TemporaryDirectory fixture = new())
        {
            WriteManifestNode(fixture, SdkLibrary, SdkManifest());
            WriteManifestNode(fixture, SystemLibrary, SystemManifest());
            NoFailures(PolicyChecks.ThirdParty(fixture.Root));
            ThirdPartyManifest sdk = ThirdPartyManifest.Read(fixture.FilePath(SdkLibrary));
            ThirdPartyHost sdkHost = sdk.Binary!.Host("windows-x64");
            Verify.That(sdk.Binary.Origin == ThirdPartyBinaryOrigin.Sdk && !sdk.HasUpstream && sdk.Source is null &&
                        sdk.Binary.Sdk == new ThirdPartySdk("Windows SDK", "10.0.22621.0") && sdkHost.StoreKey == HashA &&
                        sdkHost.Files[0] is { Path: "fake_47.dll", SdkPath: "Redist/D3D/x64/fake_47.dll", Size: 7 },
                "sdk binary fields were not read");
            ThirdPartyManifest system = ThirdPartyManifest.Read(fixture.FilePath(SystemLibrary));
            ThirdPartyHost systemHost = system.Binary!.Host("macos-arm64");
            Verify.That(system.Binary.Origin == ThirdPartyBinaryOrigin.System && system.Binary.MinimumVersion == "32023" &&
                        system.Version == "32023" && systemHost.StoreKey is null &&
                        systemHost.Files.All(file => file.Sha256 is null && file.Role == ThirdPartyFileRole.Tool),
                "system binary fields were not read");
            Verify.Throws<FormatException>(() => system.Binary.Host("windows-x64"));
        }

        foreach ((string label, Action<JsonObject> change) in new (string, Action<JsonObject>)[]
                 {
                     ("sdk binary with two files", manifest => Host(manifest)["files"]!.AsArray().Add(JsonNode.Parse(
                         "{\"path\": \"other.dll\", \"sdkPath\": \"Redist/other.dll\", \"sha256\": \"" + HashB + "\", \"size\": 1, \"role\": \"runtime\"}"))),
                     ("sdk binary without sdkPath", manifest => HostFile(manifest).Remove("sdkPath")),
                     ("sdk binary with an archive", manifest => Host(manifest)["archive"] = JsonNode.Parse(
                         "{\"url\": \"https://example.invalid/a.tar.gz\", \"sha256\": \"" + HashA + "\", \"size\": 1}")),
                     ("sdk binary with committed files", manifest => manifest["licenses"] = new JsonArray()),
                     ("unsupported sdk", manifest => manifest["sdk"]!["name"] = "Other SDK"),
                     ("unknown sdk field", manifest => manifest["sdk"]!["path"] = "Redist")
                 })
        {
            ExpectManifestFailure(label, SdkLibrary, SdkManifest(), change);
        }

        foreach ((string label, Action<JsonObject> change) in new (string, Action<JsonObject>)[]
                 {
                     ("system tool with a digest", manifest => HostFile(manifest, "macos-arm64")["sha256"] = HashA),
                     ("system tool with version", manifest => manifest["version"] = "1.0"),
                     ("system tool without minimumVersion", manifest => manifest.Remove("minimumVersion")),
                     ("system tool with runtime role", manifest => HostFile(manifest, "macos-arm64")["role"] = "runtime"),
                     ("system tool with a path", manifest => HostFile(manifest, "macos-arm64")["path"] = "usr/bin/metal"),
                     ("system tool with build", manifest => manifest["build"] = new JsonObject())
                 })
        {
            ExpectManifestFailure(label, SystemLibrary, SystemManifest(), change);
        }
    }

    // ---- fixture ----

    private static void CopyLibrary(TemporaryDirectory fixture, string from, string to, string oldName, string newName)
    {
        string source = fixture.FilePath(from);
        foreach (string file in Directory.EnumerateFiles(source, "*", SearchOption.AllDirectories))
        {
            string relative = Path.GetRelativePath(source, file).Replace('\\', '/');
            if (relative == oldName + ".Build.cs")
            {
                relative = newName + ".Build.cs";
            }

            string target = fixture.FilePath($"{to}/{relative}");
            Directory.CreateDirectory(Path.GetDirectoryName(target)!);
            File.Copy(file, target);
        }
    }

    private static void WriteHeadersFixture(TemporaryDirectory fixture, Func<string, string> change, bool recordPragmas = true)
    {
        if (!File.Exists(fixture.FilePath(HeadersUpstream + "/LICENSE.md")))
        {
            fixture.Write(HeadersUpstream + "/LICENSE.md", "Fake headers license\n");
            fixture.Write(HeadersUpstream + "/include/fake/fake_core.h",
                "#pragma once\n#include \"fake_platform.h\"\n#ifdef _MSC_VER\n#pragma pack(push, 8)\n#pragma pack(pop)\n#endif\nint FakeCore(void);\n");
            fixture.Write(HeadersUpstream + "/include/fake/fake_platform.h", "#pragma once\n#define FAKE_CALL\n");
            fixture.Write(HeadersUpstream + "/include/fake/fake_internal.h", "#pragma once\n");
        }

        (SortedDictionary<string, string> files, List<object> pragmas) = Hash(fixture.FilePath(HeadersUpstream), recordPragmas);
        var manifest = new Dictionary<string, object>
        {
            ["schema"] = 1,
            ["kind"] = "headers",
            ["name"] = "FakeHeaders",
            ["version"] = "1.0",
            ["upstreamDirectory"] = "fakeheaders-1.0",
            ["source"] = new { url = "https://example.invalid/fakeheaders-1.0.tar.gz", sha256 = HashA, verification = "fixture" },
            ["licenses"] = new[] { new { spdx = "Apache-2.0", upstreamExpression = "Apache-2.0 OR MIT", file = "fakeheaders-1.0/LICENSE.md", binaryNoticeRequired = false } },
            ["translationUnits"] = Array.Empty<string>(),
            ["includeDirectories"] = new[] { "fakeheaders-1.0/include" },
            ["defines"] = Array.Empty<string>(),
            ["platformDefines"] = new Dictionary<string, string[]>(),
            ["consumerDefines"] = new[] { "FAKE_NO_PROTOTYPES" },
            ["adapterHeaders"] = new[] { "fakeheaders-1.0/include/fake/fake_core.h", "fakeheaders-1.0/include/fake/fake_platform.h" },
            ["files"] = files,
            ["pragmaDirectives"] = pragmas,
            ["notes"] = Array.Empty<string>()
        };
        fixture.Write(HeadersLibrary + "/ThirdParty.json", change(Format(JsonSerializer.SerializeToNode(manifest)!)));
    }

    private static void WriteBinaryFixture(TemporaryDirectory fixture, Action<JsonObject> change)
    {
        if (!File.Exists(fixture.FilePath(BinaryUpstream + "/LICENSE")))
        {
            fixture.Write(BinaryUpstream + "/LICENSE", "Fake tool license\n");
            fixture.Write(BinaryUpstream + "/include/fake_api.h", "#pragma once\nint FakeApi(void);\n");
            fixture.Write(BinaryLibrary + "/Build/Build.ps1", "# build\n");
            fixture.Write(BinaryLibrary + "/Build/Build.sh", "# build\n");
            fixture.Write(BinaryLibrary + "/Build/Fix.patch", "diff --git a/x b/x\n");
        }

        (SortedDictionary<string, string> files, List<object> pragmas) = Hash(fixture.FilePath(BinaryUpstream), recordPragmas: true);
        JsonObject manifest = JsonSerializer.SerializeToNode(new Dictionary<string, object>
        {
            ["schema"] = 1,
            ["kind"] = "binary",
            ["name"] = "FakeTool",
            ["version"] = "2.0",
            ["upstreamDirectory"] = "faketool-2.0",
            ["licenses"] = new[] { new { spdx = "MIT", upstreamExpression = "MIT", file = "faketool-2.0/LICENSE", binaryNoticeRequired = true } },
            ["includeDirectories"] = new[] { "faketool-2.0/include" },
            ["consumerDefines"] = Array.Empty<string>(),
            ["adapterHeaders"] = new[] { "faketool-2.0/include/fake_api.h" },
            ["files"] = files,
            ["pragmaDirectives"] = pragmas,
            ["build"] = new
            {
                tag = "v2.0",
                revision = 3,
                source = new { repository = "https://example.invalid/faketool", commit = Commit, url = "https://example.invalid/faketool/archive/c.tar.gz", sha256 = HashB },
                dependencies = new[]
                {
                    new { path = "external/dep", repository = "https://example.invalid/dep", commit = Commit, url = "https://example.invalid/dep/archive/c.tar.gz", sha256 = HashB }
                },
                scripts = new[] { "Build/Build.ps1", "Build/Build.sh" },
                patches = new[] { "Build/Fix.patch" }
            },
            ["hosts"] = new Dictionary<string, object>
            {
                ["windows-x64"] = new
                {
                    archive = new { url = "https://example.invalid/releases/download/third-party-FakeTool-2.0-r3/FakeTool-2.0-r3-windows-x64.tar.gz", sha256 = HashA, size = 100 },
                    files = new[]
                    {
                        new { path = "bin/fake.dll", sha256 = HashB, size = 10, role = "runtime" },
                        new { path = "bin/fake.pdb", sha256 = HashB, size = 20, role = "symbols" },
                        new { path = "LICENSE", sha256 = HashB, size = 18, role = "notice" }
                    }
                }
            },
            ["notes"] = Array.Empty<string>()
        })!.AsObject();
        change(manifest);
        fixture.Write(BinaryLibrary + "/ThirdParty.json", Format(manifest));
    }

    private static JsonObject SdkManifest() => JsonSerializer.SerializeToNode(new Dictionary<string, object>
    {
        ["schema"] = 1,
        ["kind"] = "binary",
        ["name"] = "FakeSdkTool",
        ["version"] = "10.0.22621.2428",
        ["sdk"] = new { name = "Windows SDK", version = "10.0.22621.0" },
        ["hosts"] = new Dictionary<string, object>
        {
            ["windows-x64"] = new
            {
                files = new[] { new { path = "fake_47.dll", sdkPath = "Redist/D3D/x64/fake_47.dll", sha256 = HashA, size = 7, role = "runtime" } }
            }
        },
        ["notes"] = new[] { "fixture" }
    })!.AsObject();

    private static JsonObject SystemManifest() => JsonSerializer.SerializeToNode(new Dictionary<string, object>
    {
        ["schema"] = 1,
        ["kind"] = "binary",
        ["status"] = "system",
        ["name"] = "FakeSystemTool",
        ["minimumVersion"] = "32023",
        ["hosts"] = new Dictionary<string, object>
        {
            ["macos-arm64"] = new { files = new[] { new { path = "metal", role = "tool" }, new { path = "metallib", role = "tool" } } }
        },
        ["notes"] = new[] { "fixture" }
    })!.AsObject();

    private static void WriteManifestNode(TemporaryDirectory fixture, string library, JsonObject manifest) =>
        fixture.Write(library + "/ThirdParty.json", Format(manifest));

    // 원본 디렉터리의 파일 해시와 지시문 목록이다(정책 검사·갱신 도구와 같은 스캐너).
    private static (SortedDictionary<string, string> Files, List<object> Pragmas) Hash(string upstream, bool recordPragmas)
    {
        var files = new SortedDictionary<string, string>(StringComparer.Ordinal);
        var pragmas = new List<object>();
        foreach (string path in Directory.EnumerateFiles(upstream, "*", SearchOption.AllDirectories).Order(StringComparer.Ordinal))
        {
            string relative = Path.GetRelativePath(upstream, path).Replace('\\', '/');
            byte[] bytes = File.ReadAllBytes(path);
            files.Add(relative, Convert.ToHexStringLower(SHA256.HashData(bytes)));
            if (recordPragmas)
            {
                pragmas.AddRange(ThirdPartyManifest.ScanPragmaDirectives(relative, bytes)
                    .Select(pragma => (object)new { file = pragma.File, line = pragma.Line, text = pragma.Text }));
            }
        }

        return (files, pragmas);
    }

    // 갱신 도구(ThirdPartyManifestRefresh)와 같은 서식이다.
    private static string Format(JsonNode node) => node.ToJsonString(new JsonSerializerOptions
    {
        WriteIndented = true,
        NewLine = "\n",
        Encoder = System.Text.Encodings.Web.JavaScriptEncoder.UnsafeRelaxedJsonEscaping
    }) + "\n";

    private static JsonObject Host(JsonObject manifest, string key = "windows-x64") => manifest["hosts"]![key]!.AsObject();

    private static JsonObject HostFile(JsonObject manifest, string key = "windows-x64") => Host(manifest, key)["files"]![0]!.AsObject();

    private static void Move(JsonObject parent, string from, string to)
    {
        JsonNode? value = parent[from];
        parent.Remove(from);
        parent[to] = value;
    }

    private static void ExpectHeadersFailure(string label, Action<TemporaryDirectory> inject)
    {
        using TemporaryDirectory fixture = new();
        WriteHeadersFixture(fixture, manifest => manifest);
        NoFailures(PolicyChecks.ThirdParty(fixture.Root));
        inject(fixture);
        Verify.That(PolicyChecks.ThirdParty(fixture.Root).Count != 0, "headers library defect escaped audit: " + label);
    }

    private static void ExpectBinaryFailure(string label, Action<TemporaryDirectory> inject)
    {
        using TemporaryDirectory fixture = new();
        WriteBinaryFixture(fixture, manifest => { });
        NoFailures(PolicyChecks.ThirdParty(fixture.Root));
        inject(fixture);
        Verify.That(PolicyChecks.ThirdParty(fixture.Root).Count != 0, "binary library defect escaped audit: " + label);
    }

    private static void ExpectManifestFailure(string label, string library, JsonObject manifest, Action<JsonObject> change)
    {
        using TemporaryDirectory fixture = new();
        WriteManifestNode(fixture, library, manifest);
        NoFailures(PolicyChecks.ThirdParty(fixture.Root));
        change(manifest);
        WriteManifestNode(fixture, library, manifest);
        Verify.That(PolicyChecks.ThirdParty(fixture.Root).Count != 0, "binary manifest defect escaped audit: " + label);
    }

    private static string Replace(string text, string oldValue, string newValue)
    {
        Verify.That(text.Contains(oldValue, StringComparison.Ordinal), "fixture replacement target missing: " + oldValue);
        return text.Replace(oldValue, newValue, StringComparison.Ordinal);
    }

    private static void NoFailures(IReadOnlyList<CheckFailure> failures) => Verify.That(failures.Count == 0, string.Join('\n', failures));
}
