using Rocks.Build;
using Rocks.Tooling.Common;

/// <summary>
/// 정밀 헤더 의존(Docs/Design/UnityBuildPch.ko.md 4절, UB0)을 검사한다.
/// <list type="bullet">
/// <item>depfile 파서: 공백·한글·괄호·<c>#</c>·<c>$</c> 경로, 공백 앞 역슬래시, 줄 이음(LF·CRLF), 상대·절대 경로, 깨진 입력.</item>
/// <item>2단 key: 헤더 하나를 바꾸면 그 헤더를 실제로 연 번역 단위만 다시 컴파일한다. 새 shadow header와 <c>__has_include</c> 반례는
/// 헤더 이름 목록으로 다시 빌드된다. header 디렉터리 밖으로 벗어난 include도 의존 기록이 잡는다.</item>
/// <item>의존 기록: 툴체인 소유 헤더가 없고, 손상되거나 없어지면 다시 실행한다.</item>
/// <item>compile identity: 모듈에 소스를 더해도 다른 번역 단위는 다시 컴파일하지 않는다.</item>
/// <item>출력 검증 memo: 내용이 같은 출력을 다시 써도(수정 시각만 바뀜) hit이고, 내용이 바뀌면 miss다.</item>
/// </list>
/// </summary>
internal static class DependencyTests
{
    public static async Task RunAsync(NativeBuildRequest request, string root, Action<string> pass)
    {
        ParsesDepfiles(root);
        pass("depfile parser: spaces, Korean, parentheses, #, $, backslashes before a space, continuations, relative and absolute paths");

        string fixture = Directory.CreateDirectory(Path.Combine(root, "dependency fixture (정밀)")).FullName;
        string Write(string relative, string content)
        {
            string path = Path.GetFullPath(Path.Combine(fixture, relative));
            Directory.CreateDirectory(Path.GetDirectoryName(path)!);
            File.WriteAllText(path, content);
            return path;
        }

        string first = Directory.CreateDirectory(Path.Combine(fixture, "include first")).FullName;
        string second = Directory.CreateDirectory(Path.Combine(fixture, "include second")).FullName;
        string alphaHeader = Write("include second/alpha.h", "#pragma once\ninline int Alpha() noexcept { return 1; }\n");
        string betaHeader = Write("include second/beta.h", "#pragma once\ninline int Beta() noexcept { return 2; }\n");
        string outsideHeader = Write("outside/outside.h", "#pragma once\ninline int Outside() noexcept { return 4; }\n");
        string alphaSource = Write("src/alpha.cpp", "#include <stddef.h>\n#include <alpha.h>\n#include \"../outside/outside.h\"\nint AlphaValue() noexcept { return Alpha() + Outside(); }\n");
        string betaSource = Write("src/beta.cpp", "#include <beta.h>\n#if __has_include(<optional.h>)\n#include <optional.h>\n#else\n" +
            "#define OPTIONAL_VALUE 0\n#endif\nint BetaValue() noexcept { return Beta() + OPTIONAL_VALUE; }\n");
        string mainSource = Write("app/main.cpp", "int AlphaValue() noexcept;\nint BetaValue() noexcept;\nint main() { return AlphaValue() * 10 + BetaValue(); }\n");
        NativeModuleInput library = new() { Name = "DepLib", Sources = [alphaSource, betaSource], PrivateIncludeDirectories = [first, second] };
        NativeModuleInput application = new() { Name = "DepApp", Kind = NativeModuleKind.Executable, Sources = [mainSource], PublicDependencies = ["DepLib"] };
        NativeBuildRequest build = request with { OutputDirectory = Path.Combine(fixture, "out"), Modules = [library, application], Targets = ["DepApp"] };
        var builder = new NativeBuildEngine();
        async Task<NativeBuildResult> Build(NativeBuildRequest? input = null)
        {
            NativeBuildResult result = await builder.BuildAsync(input ?? build);
            Check(result.Succeeded, "dependency fixture build failed: " + result.Failure + "\n" +
                string.Join("\n", result.Actions.Where(action => !action.Succeeded).Select(action => action.StdOut + action.StdErr)));
            return result;
        }

        async Task<int> Run(NativeBuildResult result) =>
            request.Toolchain.Platform == NativePlatform.Android ? -1
                : (await ProcessRunner.RunAsync(new(result.ModuleOutputs["DepApp"], [], fixture, request.Toolchain.Environment))).ExitCode;

        NativePlan plan = ActionGraph.Create(build with { RootDirectory = Path.GetFullPath(build.RootDirectory) });
        string CompileId(string source) => plan.Actions.Single(action => action.Kind == "Compile" && action.Source == source).Id;
        string[] Compiled(NativeBuildResult result) => result.Actions.Where(action => action.Kind == "Compile" && action.Executed)
            .Select(action => action.Id).Order(StringComparer.Ordinal).ToArray();
        string alphaId = CompileId(alphaSource);
        string betaId = CompileId(betaSource);

        NativeBuildResult cold = await Build();
        Check(cold.ExecutedActions == 5 && (await Run(cold)) is 52 or -1, "cold dependency fixture build");
        Check((await Build()).ExecutedActions == 0, "an unchanged build must execute zero actions");

        // 의존 기록: 컴파일러가 연 파일(소스, 두 include 디렉터리의 헤더, 디렉터리 밖 헤더)을 담고 툴체인 소유 헤더는 담지 않는다.
        NativeAction alphaAction = plan.Actions.Single(action => action.Id == alphaId);
        Check(alphaAction.DependencyRecord is not null && alphaAction.Outputs.Contains(alphaAction.DependencyRecord),
            "a compile action must declare its dependency record as an output");
        IReadOnlyList<DependencyEntry> recorded = DependencyRecord.Read(alphaAction.DependencyRecord!) ??
            throw new InvalidOperationException("the dependency record is unreadable");
        string[] recordedPaths = recorded.Select(entry => entry.Path).ToArray();
        // 모듈 정의 헤더도 강제 include된 프로젝트 파일이다(모든 C++ 모듈이 API 매크로를 받는다 — Docs/Design/BuildTargets.ko.md 3.8절 BS8a).
        string libDefinitions = ModuleDefinitions.PathOf(build.OutputDirectory, "DepLib");
        Check(recordedPaths.Order(NativeToolchains.PathComparer).SequenceEqual(new[] { alphaSource, alphaHeader, outsideHeader, libDefinitions }.Order(NativeToolchains.PathComparer),
                NativeToolchains.PathComparer),
            "the dependency record must list exactly the opened project files: " + string.Join(", ", recordedPaths));
        Check(recorded.All(entry => entry.Hash == Hashing.Sha256File(entry.Path)), "the dependency record must carry content hashes");
        pass("dependency records list the opened project files with content hashes and no toolchain headers");

        // 헤더 하나: 그 헤더를 연 번역 단위만 다시 컴파일한다(같은 include 디렉터리의 다른 헤더를 쓰는 TU는 그대로다).
        File.AppendAllText(alphaHeader, "// alpha changed\n");
        Check(Compiled(await Build()).SequenceEqual([alphaId]), "a header edit must recompile only the translation units that opened it");
        File.AppendAllText(betaHeader, "// beta changed\n");
        Check(Compiled(await Build()).SequenceEqual([betaId]), "the other header edit must recompile only its translation unit");
        // header 디렉터리 밖으로 벗어난 include도 의존 기록에 있다.
        File.WriteAllText(outsideHeader, "#pragma once\ninline int Outside() noexcept { return 5; }\n");
        NativeBuildResult outside = await Build();
        Check(Compiled(outside).SequenceEqual([alphaId]) && (await Run(outside)) is 62 or -1,
            "an include outside the header directories must be tracked by the dependency record");
        pass("a header edit recompiles only the translation units that opened it, including an include outside the header directories");

        // __has_include 반례: 없던 헤더가 생기면 이름 목록이 바뀌어 그 디렉터리를 보는 TU가 다시 컴파일되고 결과가 바뀐다.
        string optional = Write("include second/optional.h", "#pragma once\n#define OPTIONAL_VALUE 30\n");
        NativeBuildResult withOptional = await Build();
        Check(Compiled(withOptional).SequenceEqual(new[] { alphaId, betaId }.Order(StringComparer.Ordinal)) && (await Run(withOptional)) is 92 or -1,
            "a new header must rebuild the consumers of that include directory (__has_include)");
        File.Delete(optional);
        NativeBuildResult withoutOptional = await Build();
        Check(Compiled(withoutOptional).Length == 2 && (await Run(withoutOptional)) is 62 or -1, "a deleted header must rebuild its consumers");
        // 새 shadow header: 앞선 include 디렉터리에 같은 철자가 생기면 다시 빌드되어 그 헤더를 쓴다.
        string shadow = Write("include first/alpha.h", "#pragma once\ninline int Alpha() noexcept { return 3; }\n");
        NativeBuildResult shadowed = await Build();
        Check(Compiled(shadowed).Contains(alphaId) && (await Run(shadowed)) is 82 or -1, "a new shadowing header must be picked up");
        File.Delete(shadow);
        Check((await Run(await Build())) is 62 or -1, "removing the shadowing header must restore the original header");
        Check((await Build()).ExecutedActions == 0, "the build must settle after the header set changes");
        pass("header name lists catch __has_include changes, deleted headers and new shadowing headers");

        // 의존 기록이 손상되거나 없어지면 그 action을 다시 실행한다.
        File.WriteAllText(alphaAction.DependencyRecord!, "not a dependency record\n");
        Check(Compiled(await Build()).SequenceEqual([alphaId]), "a corrupt dependency record must rerun its action");
        File.Delete(alphaAction.DependencyRecord!);
        Check(Compiled(await Build()).SequenceEqual([alphaId]), "a missing dependency record must rerun its action");
        Check(DependencyRecord.Read(alphaAction.DependencyRecord!) is not null, "the rerun must publish the dependency record again");
        pass("corrupt and missing dependency records rerun their actions");

        // compile identity는 그 TU의 구성뿐이다: 모듈에 소스를 더해도 기존 TU는 다시 컴파일하지 않는다(새 TU·archive·link만).
        string gammaSource = Write("src/gamma.cpp", "int GammaValue() noexcept { return 7; }\n");
        NativeBuildRequest withGamma = build with { Modules = [library with { Sources = [alphaSource, betaSource, gammaSource] }, application] };
        NativeBuildResult added = await Build(withGamma);
        string[] executed = added.Actions.Where(action => action.Executed).Select(action => action.Module + ":" + action.Kind).Order(StringComparer.Ordinal).ToArray();
        Check(executed.SequenceEqual(["DepApp:Link", "DepLib:Archive", "DepLib:Compile"]),
            "adding a source must compile only the new translation unit: " + string.Join(", ", executed));
        NativeBuildResult removed = await Build();
        Check(Compiled(removed).Length == 0 && removed.Actions.Any(action => action.Kind == "Archive" && action.Executed),
            "removing a source must not recompile the remaining translation units");
        File.Delete(gammaSource);
        pass("adding or removing a module source leaves the other translation units cached");

        // 출력 검증 memo: 같은 내용을 다시 쓰면(수정 시각이 바뀐다) 내용을 다시 해시해 hit이고, 내용이 바뀌면 miss다.
        string library0 = (await Build()).ModuleOutputs["DepLib"];
        byte[] libraryBytes = File.ReadAllBytes(library0);
        File.WriteAllBytes(library0, libraryBytes);
        DateTime restamped = File.GetLastWriteTimeUtc(library0).AddMinutes(1);
        File.SetLastWriteTimeUtc(library0, restamped);
        Check((await Build()).ExecutedActions == 0, "an output rewritten with the same content must stay a hit");
        Check((await Build()).ExecutedActions == 0, "the refreshed output record must stay a hit");
        byte[] tampered = (byte[])libraryBytes.Clone();
        tampered[^1] ^= 0xff;
        File.WriteAllBytes(library0, tampered);
        // 수정 시각이 기록과 달라야 다시 해시한다(셋이 모두 같으면 기록을 믿는 것이 계약이다). 시계에 기대지 않고 명시한다.
        File.SetLastWriteTimeUtc(library0, restamped.AddMinutes(1));
        NativeBuildResult rebuilt = await Build();
        Check(rebuilt.Actions.Any(action => action.Kind == "Archive" && action.Executed), "an output with different content of the same size must rerun its action");
        pass("output verification trusts unchanged file identity, rehashes on metadata change and rejects changed content");
    }

    private static void ParsesDepfiles(string root)
    {
        string working = Path.GetFullPath(Path.Combine(root, "depfile working"));
        string Full(string relative) => Path.GetFullPath(relative, working);
        void Expect(string text, params string[] expected)
        {
            string[] actual = Depfile.Parse(text, working).ToArray();
            Check(actual.SequenceEqual(expected.Select(Full), StringComparer.Ordinal),
                "depfile parse mismatch for\n" + text + "\nactual: " + string.Join(" | ", actual));
        }

        // 한 줄, 줄 이음(LF·CRLF), 상대 경로.
        Expect("out/a.o: src/a.cpp include/a.h\n", "src/a.cpp", "include/a.h");
        Expect("out/a.o: \\\n  src/a.cpp \\\n  include/a.h\n", "src/a.cpp", "include/a.h");
        Expect("out/a.o: \\\r\n  src/a.cpp \\\r\n  include/a.h\r\n", "src/a.cpp", "include/a.h");
        Expect("out/a.o : src/a.cpp", "src/a.cpp");
        // 공백·한글·괄호·#·$ 경로와 중복 제거.
        Expect("out/a.o: src/my\\ file.cpp include/한글\\ 헤더.h include/(paren)/x.h include/sharp\\#1.h include/dollar$$1.h src/my\\ file.cpp\n",
            "src/my file.cpp", "include/한글 헤더.h", "include/(paren)/x.h", "include/sharp#1.h", "include/dollar$1.h");
        if (OperatingSystem.IsWindows())
        {
            // Windows: 구분자 역슬래시는 그대로, 공백 앞 역슬래시는 두 배 + escape 하나(clang의 쓰기 규칙), 드라이브 문자의 ':'는 구분자가 아니다.
            string[] parsed = Depfile.Parse("C:\\out\\a.obj: C:\\src\\a.cpp \\\n  C:\\Program\\ Files\\sdk\\x.h \\\n  C:\\dir\\\\\\ name\\y.h \\\n  rel\\z.h\n", "C:\\work").ToArray();
            Check(parsed.SequenceEqual(["C:\\src\\a.cpp", "C:\\Program Files\\sdk\\x.h", "C:\\dir\\ name\\y.h", "C:\\work\\rel\\z.h"], StringComparer.Ordinal),
                "Windows depfile parse mismatch: " + string.Join(" | ", parsed));
        }

        foreach (string broken in new[] { "", "   \n", "src/a.cpp include/a.h\n" })
        {
            try
            {
                Depfile.Parse(broken, working);
            }
            catch (InvalidDataException)
            {
                continue;
            }

            throw new InvalidOperationException("a depfile without a target was accepted: '" + broken + "'");
        }

        // 의존 기록 형식: 왕복, 그리고 깨진 내용은 null이다.
        string record = Path.Combine(Directory.CreateDirectory(working).FullName, "record.deps");
        DependencyEntry[] entries = [new(Full("include/한글 헤더.h"), new string('a', 64)), new(Full("src/a.cpp"), new string('0', 64))];
        File.WriteAllBytes(record, DependencyRecord.Encode(entries));
        Check(DependencyRecord.Read(record)!.SequenceEqual(entries.OrderBy(entry => entry.Path, StringComparer.Ordinal)), "dependency record round trip");
        foreach (string corrupt in new[] { "", "rocks-dependency-record 1", "rocks-dependency-record 9\n", "rocks-dependency-record 1\nshort path\n",
                     "rocks-dependency-record 1\n" + new string('A', 64) + " path\n", "rocks-dependency-record 1\n" + new string('a', 64) + " path" })
        {
            File.WriteAllText(record, corrupt);
            Check(DependencyRecord.Read(record) is null, "a corrupt dependency record was accepted: '" + corrupt + "'");
        }

        Check(DependencyRecord.Read(Path.Combine(working, "missing.deps")) is null, "a missing dependency record must read as null");
    }

    private static void Check(bool condition, string message)
    {
        if (!condition)
        {
            throw new InvalidOperationException(message);
        }
    }
}
