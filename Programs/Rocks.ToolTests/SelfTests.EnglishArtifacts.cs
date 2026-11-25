using Rocks.Tooling.Checks;

namespace Rocks.ToolTests;

// self 시험 중 영어 결과물 정책(source-literal-english·build-file-english)이다: 저장소 전체 실행과 양성·음성 대조.
internal static partial class SelfTests
{
    private static void RegisterEnglishArtifacts(TestSuite suite, Options options)
    {
        suite.Add("Policy.RepositoryEnglishSourceLiterals", () => NoFailures(PolicyChecks.EnglishSourceLiterals(options.Root)));
        suite.Add("Policy.RepositoryEnglishBuildFiles", () => NoFailures(PolicyChecks.EnglishBuildFiles(options.Root)));
        suite.Add("Policy.EnglishSourceLiteralControls", EnglishSourceLiteralControls);
        suite.Add("Policy.EnglishBuildFileControls", EnglishBuildFileControls);
    }

    private static void EnglishSourceLiteralControls()
    {
        using TemporaryDirectory temporary = new();

        // 주석 안의 한국어(C++·C#의 //·/* */·///)와 영어 문자열은 허용한다. 문자열 안의 // 와 /* 는 주석이 아니므로 그 뒤의
        // 진짜 주석도 주석으로 읽어야 한다(축자 문자열의 끝 역슬래시, 원시 문자열, 문자 리터럴 포함).
        temporary.Write("Core/Source/Runtime/Core/Private/Probe.cpp", string.Join('\n',
            "// 한국어 주석",
            "/* 여러 줄",
            "   한국어 주석 */",
            "const char* path = \"a//b\"; // 한국어 주석",
            "const char* raw = R\"(x /* y)\"; /* 한국어 */",
            "char quote = '\"'; // 한국어",
            ""));
        temporary.Write("Programs/Rocks.Build/Probe.cs", string.Join('\n',
            "/// <summary>한국어 문서 주석</summary>",
            "string path = @\"C:\\dir\\\"; // 한국어 주석",
            "string raw = \"\"\"a // b\"\"\"; /* 한국어 */",
            "string text = $\"{value} // English\"; // 한국어",
            "char quote = '\"'; // 한국어",
            ""));
        // 편집기 연동(Tools/)의 TypeScript도 주석은 한국어로 둘 수 있다. npm이 받은 node_modules는 외부 원본이라 보지 않는다.
        temporary.Write("Tools/VSCode/probe/src/probe.ts", string.Join('\n',
            "// 한국어 주석",
            "const path = \"a//b\"; /* 한국어 */",
            "const url = `see http://x ${value}`; // 한국어",
            "const lines = `first",
            "second // still text`; // 한국어",
            ""));
        temporary.Write("Tools/VSCode/probe/node_modules/dep/index.ts", "const text = \"한국어\";\n");

        // 시험 소스·C# 시험 프로젝트·시험 fixture 모듈은 유니코드 처리를 시험하려고 한글 문자열을 쓸 수 있다. 빌드 규칙·생성 파일은
        // 파일 전체 규칙(build-file-english)이 따로 본다.
        foreach (string file in new[] { "Core/Source/Runtime/Core/Tests/ProbeTests.cpp", "Engine/Source/Runtime/World/Tests/ProbeTests.cpp",
            "Programs/FoundationTests/Private/Probe.cpp", "Core/Source/Developer/ReflectionTestFixtures/Public/ReflectionTestFixtures/Probe.h",
            "Programs/Rocks.ToolTests/Probe.cs", "Programs/Rocks.Build.Tests/Probe.cs", "Core/Source/Runtime/Probe/Probe.Build.cs",
            "Core/Source/Runtime/Core/Private/Probe.generated.h", "Docs/Probe.cpp" })
        {
            temporary.Write(file, "const char* text = \"한국어 문자열\";\n");
        }

        NoFailures(PolicyChecks.EnglishSourceLiterals(temporary.Root));

        // 시험이 아닌 런타임·개발 도구·프로그램 소스의 한글 문자열은 어디서든 위반이다(Build/ 아래는 파일 전체 규칙이 맡는다. 모듈 Private/Tests/는 모듈에
        // 컴파일되어 출하되므로 시험 소스가 아니다).
        string[] nativeSources = [
            "const char* text = \"한국어 메시지\";", "ROCKS_LOG(Warning, \"설정을 읽지 못했다: %s\", path);",
            "Log::Warning(LogConfig, \"설정을 읽지 못했다: {Path}\", path);", "char16_t c = u'한';",
            "const char* raw = R\"(한국어)\";", "const char* text = \"a//b\" \"한국어\"; // comment"];
        string[] csharpSources = [
            "Console.WriteLine(\"한국어 메시지\");", "string path = @\"C:\\디렉터리\\\";", "string raw = \"\"\"한국어\"\"\";",
            "string text = $\"{value} 한국어\";", "char c = '한';", "throw new InvalidOperationException($@\"{name} 한국어\");"];
        foreach ((string file, string[] sources) in new[] {
            ("Core/Source/Runtime/Core/Private/Probe.cpp", nativeSources), ("Core/Source/Runtime/Core/Private/Tests/Probe.cpp", nativeSources),
            ("Engine/Source/Developer/Probe/Public/Probe.h", nativeSources), ("Programs/Sandbox/Private/Probe.cpp", nativeSources),
            ("Programs/Rocks.Build/Probe.cs", csharpSources), ("Programs/Rocks.MetaCompiler/Probe.cs", csharpSources),
            ("Tools/VSCode/probe/src/probe.ts", new[] { "const text = \"한국어 메시지\";", "const text = 'a//b' + '한국어';", "const text = `see http://x 한국어`;",
                "const text = `${name} 한국어`;" }) })
        {
            foreach (string source in sources)
            {
                temporary.Write(file, "// first line\n" + source + "\n");
                var failures = PolicyChecks.EnglishSourceLiterals(temporary.Root);
                Verify.That(failures.Count == 1 && failures[0].Line == 2 && failures[0].Path == file &&
                    failures[0].ToString().Contains("source-literal-english", StringComparison.Ordinal),
                    $"Korean string escaped audit in {file}: {source}: {string.Join('\n', failures)}");
                temporary.Write(file, "// clean\n");
            }
        }
    }

    private static void EnglishBuildFileControls()
    {
        using TemporaryDirectory temporary = new();

        // 외부 원본(upstreamDirectory)의 한글은 업스트림 byte 그대로 둔다. 원본을 먼저 써야 manifest의 files에 들어간다.
        temporary.Write(ThirdPartyPolicyTests.Upstream + "/NormalizationTest.txt", "AC00;가\n");
        ThirdPartyPolicyTests.WriteFixture(temporary);

        // 문서·외부 원본·빌드 출력 디렉터리의 주석은 허용한다. 시험 소스의 문자열은 파일 전체 규칙 대상이 아니다.
        foreach (string file in new[] { "Docs/Design/Probe.ko.md", "Build/README.md", "Templates/Game/Foundation/README.ko.md",
            "Core/Source/ThirdParty/Fake/README.md", "Docs/Design/Probe.json", "Build/Artifacts/Probe.ps1", "Build/Cache/Probe.sh",
            "Core/Source/Runtime/Core/Tests/ProbeTests.cpp", "Programs/Rocks.ToolTests/Probe.cs", "Tools/VSCode/probe/src/probe.ts",
            "Tools/VSCode/probe/README.md", "Tools/VSCode/probe/node_modules/dep/package.json" })
        {
            temporary.Write(file, "// 한국어 주석\n");
        }

        NoFailures(PolicyChecks.EnglishBuildFiles(temporary.Root));

        // 빌드 설정·생성물은 주석을 포함해 파일 전체가 영어다. 시험 실행 파일·시험 fixture 모듈의 규칙과 시험 디렉터리 안의 생성
        // 파일도 같다.
        foreach (string file in new[] { "Rocks.proj", ".editorconfig", ".gitattributes", ".gitignore", "global.json",
            ".github/workflows/foundation.yml", "Core/Source/Runtime/Probe/Probe.Build.cs", "Programs/Probe/Probe.Target.cs",
            "Programs/FoundationTests/FoundationTests.Build.cs", "Core/Source/Developer/ProbeTestFixtures/ProbeTestFixtures.Build.cs",
            "Programs/Rocks.Probe/Rocks.Probe.csproj", "Programs/Directory.Build.props", "Build/Format/Rocks.Format.slnx",
            "Build/Probe.ps1", "Build/ThirdPartyTools/probe.sh", "Build/Probe.cmake", "Build/Build.version", "Build/Rules/ProbeRules.cs",
            ThirdPartyPolicyTests.Library + "/Glue.c", ThirdPartyPolicyTests.Library + "/Fake.patch",
            "Engine/Source/ThirdParty/Probe/ThirdParty.json", "Engine/Source/ThirdParty/Probe/Build/Build.ps1",
            "Templates/Game/Foundation/FoundationTemplate.rocksproject", "Templates/Game/Foundation/Source/Main.cpp",
            "Programs/Scaffold/Templates/module/Module.h.in", "Engine/Config/ShaderFormats/Probe.json",
            "Core/Source/Runtime/Core/Private/Text/Probe.generated.h", "Core/Source/Runtime/Probe/Tests/Probe.generated.h",
            "Tools/VSCode/probe/package.json", "Tools/VSCode/probe/syntaxes/probe.tmLanguage.json" })
        {
            string? original = File.Exists(temporary.FilePath(file)) ? File.ReadAllText(temporary.FilePath(file)) : null;
            temporary.Write(file, "# first line\n# 한국어 주석\n");
            var failures = PolicyChecks.EnglishBuildFiles(temporary.Root);
            Verify.That(failures.Count == 1 && failures[0].Line == 2 && failures[0].Path == file &&
                failures[0].ToString().Contains("build-file-english", StringComparison.Ordinal),
                $"Korean build file text escaped audit in {file}: {string.Join('\n', failures)}");
            if (original is null)
            {
                File.Delete(temporary.FilePath(file));
            }
            else
            {
                temporary.Write(file, original);
            }
        }

        // ThirdParty.json에 쓴 한국어 notes는 manifest를 읽을 수 있어도 위반이다.
        string manifest = File.ReadAllText(temporary.FilePath(ThirdPartyPolicyTests.Library + "/ThirdParty.json"));
        temporary.Write(ThirdPartyPolicyTests.Library + "/ThirdParty.json",
            manifest.Replace("\"notes\": []", "\"notes\": [\n    \"한국어 설명\"\n  ]", StringComparison.Ordinal));
        var noted = PolicyChecks.EnglishBuildFiles(temporary.Root);
        Verify.That(noted.Count == 1 && noted[0].Path == ThirdPartyPolicyTests.Library + "/ThirdParty.json",
            $"Korean manifest notes escaped audit: {string.Join('\n', noted)}");
    }
}
