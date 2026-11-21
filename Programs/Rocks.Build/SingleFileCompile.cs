using System.Text.Json;
using Rocks.Tooling.Common;

namespace Rocks.Build;

/// <summary>
/// 파일 하나 컴파일의 결과다. Arguments는 실행한 인자(논리 명령에서 object 경로만 바꾼 것, response file을 쓰기 전)이다.
/// </summary>
public sealed record SingleFileResult(string Source, string Module, string Object, IReadOnlyList<string> Arguments, ProcessResult Process);

/// <summary>
/// 파일 하나 컴파일이다(Docs/Design/ProjectFiles.ko.md 7.4절, Visual Studio Ctrl+F7). 요청을 IDE 계획 모드로 계획해 고른 소스의 논리 명령
/// (IntelliSense 원천과 같은 명령)을 찾고, object 경로만 <c>&lt;출력&gt;/SingleFile/&lt;해시&gt;.obj</c>로 바꿔 실행한다. 유니티 묶음·PCH
/// 바이너리·작업 파일 분리와 무관하고(논리 명령은 PCH 헤더를 강제 include한다) 증분 기록·action-report·평소 object를 바꾸지 않는다.
/// 메타 생성은 호출자가 먼저 최신으로 맞춘다.
/// </summary>
public static class SingleFileCompile
{
    /// <summary>object와 기록을 두는 출력 아래 디렉터리 이름이다.</summary>
    public const string DirectoryName = "SingleFile";

    // 저장소 기준 경로 SHA-256 16진 앞 글자 수다(object 이름).
    private const int NameHashLength = 16;

    // 헤더로 보는 확장자다(헤더 단독 컴파일은 PublicHeaderChecks가 맡는다).
    private static readonly string[] HeaderExtensions = [".h", ".hh", ".hpp", ".hxx", ".inl", ".inc", ".ipp", ".tpp"];

    /// <summary>소스의 object 경로다(<c>&lt;출력&gt;/SingleFile/&lt;저장소 기준 경로 해시&gt;.obj</c>, macOS·Android는 <c>.o</c>).</summary>
    public static string ObjectPath(NativeBuildRequest request, string source)
    {
        string relative = Path.GetRelativePath(Path.GetFullPath(request.RootDirectory), Path.GetFullPath(source)).Replace('\\', '/');
        string name = Hashing.Sha256Text(relative)[..NameHashLength];
        return Path.Combine(Path.GetFullPath(request.OutputDirectory), DirectoryName,
            name + (request.Toolchain.Platform == NativePlatform.Windows ? ".obj" : ".o"));
    }

    /// <summary>
    /// 고른 소스를 차례로 컴파일한다. 헤더이거나 이 요청이 컴파일하지 않는 소스가 있으면 아무것도 컴파일하기 전에
    /// <see cref="ArgumentException"/>이다. 컴파일러가 실패해도 다음 소스를 계속하고 결과에 담는다.
    /// </summary>
    public static async Task<IReadOnlyList<SingleFileResult>> RunAsync(NativeBuildRequest request, IReadOnlyList<string> sources,
        CancellationToken cancellationToken)
    {
        if (sources.Count == 0)
        {
            throw new ArgumentException("Select at least one source file to compile.");
        }

        IdePlan plan = IdePlanner.Plan(request);
        var selected = new List<(string Source, IdeCompile Compile)>();
        foreach (string source in sources.Select(Path.GetFullPath))
        {
            if (HeaderExtensions.Contains(Path.GetExtension(source), StringComparer.OrdinalIgnoreCase))
            {
                throw new ArgumentException($"{source} is a header; compile a source file (PublicHeaderChecks compiles headers on their own).");
            }

            IdeCompile compile = plan.Compiles.Where(value => NativeToolchains.PathComparer.Equals(Path.GetFullPath(value.Source), source))
                .OrderBy(value => value.Module, StringComparer.Ordinal).FirstOrDefault()
                ?? throw new ArgumentException($"{source} is not compiled by this target and configuration.");
            selected.Add((source, compile));
        }

        var results = new List<SingleFileResult>();
        foreach ((string source, IdeCompile compile) in selected)
        {
            cancellationToken.ThrowIfCancellationRequested();
            // 강제 include하는 생성 파일(정의 헤더)은 계획이 만든 내용으로 맞춘다 — 평소 빌드와 같은 byte라 증분 기록을 건드리지 않는다.
            foreach (string forced in compile.ForcedIncludes)
            {
                if (plan.GeneratedFiles.TryGetValue(forced, out string? content))
                {
                    Directory.CreateDirectory(Path.GetDirectoryName(forced)!);
                    AtomicFile.WriteIfChanged(forced, content);
                }
            }

            string objectPath = ObjectPath(request, source);
            Directory.CreateDirectory(Path.GetDirectoryName(objectPath)!);
            string[] arguments = CompileObjectArguments.With(compile.Arguments, objectPath);
            AtomicFile.WriteIfChanged(Path.ChangeExtension(objectPath, ".json"), JsonSerializer.Serialize(new
            {
                source,
                module = compile.Module,
                @object = objectPath,
                arguments
            }, new JsonSerializerOptions { WriteIndented = true }) + "\n");
            IReadOnlyList<string> processArguments = arguments;
            if (NativeCommandLine.ExceedsHostLimit(plan.Compiler, arguments))
            {
                string responseFile = Path.ChangeExtension(objectPath, ".rsp");
                File.WriteAllBytes(responseFile, ResponseFile.Encode(arguments,
                    request.Toolchain.Platform == NativePlatform.Windows ? ResponseFileQuoting.Windows : ResponseFileQuoting.Gnu));
                processArguments = ["@" + responseFile];
            }

            ProcessResult process = await ProcessRunner.RunAsync(new ProcessSpec(plan.Compiler, processArguments, Path.GetFullPath(request.RootDirectory),
                request.Toolchain.Environment), cancellationToken);
            results.Add(new(source, compile.Module, objectPath, arguments, process));
        }

        return results;
    }
}
