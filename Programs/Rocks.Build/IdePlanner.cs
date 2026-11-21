namespace Rocks.Build;

/// <summary>계획 방식이다(<see cref="NativeBuildRequest.PlanMode"/>).</summary>
public enum NativePlanMode
{
    /// <summary>빌드 계획이다. 생성물이 있어야 하고 action을 세운다.</summary>
    Build,

    /// <summary>
    /// IDE 프로젝트 파일 생성용 계획이다(Docs/Design/ProjectFiles.ko.md 8.1절). 손으로 쓴 소스의 논리 compile 명령과 출력 경로만 만들고,
    /// 출력에 쓰지 않으며 생성물(리플렉션 생성 소스·생성 include 디렉터리·prebuilt·ASan 런타임)의 존재를 요구하지 않는다.
    /// </summary>
    Ide
}

/// <summary>
/// 손으로 쓴 소스 하나의 논리 compile 명령이다(compile_commands.json 항목과 같은 인자). Arguments는 펼친 인자이고 첫 칸은 컴파일러가
/// 아니다(<see cref="IdePlan.Compiler"/>). ForcedIncludes는 강제 include하는 파일(정의 헤더·PCH 헤더)이고, PchHeader는 그중 PCH 헤더다
/// (PCH를 쓰지 않으면 null).
/// </summary>
public sealed record IdeCompile(string Module, string Source, IReadOnlyList<string> Arguments, IReadOnlyList<string> ForcedIncludes,
    string? PchHeader = null);

/// <summary>IDE 계획에 든 모듈 하나의 헤더 위치다(프로젝트 파일 목록에 헤더를 모을 때 쓴다).</summary>
public sealed record IdeModule(string Name, IReadOnlyList<string> IncludeDirectories, IReadOnlyList<string> SourceDirectories);

/// <summary>
/// IDE 계획 결과다. Compiles는 계획 순서의 논리 명령, ModuleOutputs는 모듈 이름 → 최종 산출물 경로(실행 파일은 .exe 등),
/// GeneratedFiles는 계획이 만들 생성 파일(정의 헤더 등)의 경로 → 내용이다. 이 계획은 아무 파일도 쓰지 않았다.
/// </summary>
public sealed record IdePlan(string Compiler, string OutputDirectory, IReadOnlyList<IdeCompile> Compiles,
    IReadOnlyDictionary<string, string> ModuleOutputs, IReadOnlyDictionary<string, string> GeneratedFiles, IReadOnlyList<IdeModule> Modules);

/// <summary>IDE 계획 진입점이다(Docs/Design/ProjectFiles.ko.md 8.1절, PJD32).</summary>
public static class IdePlanner
{
    /// <summary>
    /// <paramref name="request"/>를 IDE 계획 모드로 계획한다. 유니티·작업 파일 분리·시간 측정은 끄고(논리 명령과 무관하다) PCH는 켠다(PCH
    /// 헤더가 강제 include 목록에 나온다). 모듈 표 TU 경로는 빌드처럼 더하지만 규칙 생성 TU를 쓰지 않고, BuildInfo를 준비하지 않으며, git을
    /// 부르지 않는다. 출력 디렉터리는 만들지도 쓰지도 않는다.
    /// </summary>
    /// <exception cref="ArgumentException">요청 검증 실패(빌드와 같은 규칙).</exception>
    public static IdePlan Plan(NativeBuildRequest request)
    {
        InputValidation.RequireAbsoluteRoots(request);
        request = request with
        {
            RootDirectory = Path.GetFullPath(request.RootDirectory),
            OutputDirectory = Path.GetFullPath(request.OutputDirectory),
            SdkDirectory = request.SdkDirectory is null ? null : Path.GetFullPath(request.SdkDirectory),
            PlanMode = NativePlanMode.Ide,
            Unity = false,
            Adaptive = false,
            Pch = true,
            Timing = false,
            PlanOnly = false,
            WorkingSet = null
        };
        InputValidation.Validate(request);
        request = PathSpelling.OnDisk(request);
        request = ModuleTables.Add(request);
        NativePlan plan = ActionGraph.Create(request);
        string output = request.OutputDirectory;
        var compiles = plan.LogicalCompiles
            .Where(compile => !Fingerprints.IsUnder(compile.Source, output))
            .Select(compile => new IdeCompile(compile.Module, compile.Source, NativeAction.Resolve(compile.Arguments, [compile.Object]),
                compile.ForcedIncludes, compile.PchHeader))
            .ToArray();
        var generated = new Dictionary<string, string>(NativeToolchains.PathComparer);
        foreach (PlannedFile file in plan.GeneratedFiles)
        {
            generated[file.Path] = file.Content;
        }

        string Absolute(string path) => Path.GetFullPath(path, request.RootDirectory);
        var modules = request.Modules.Select(module => new IdeModule(module.Name,
                module.PublicIncludeDirectories.Concat(module.PrivateIncludeDirectories).Select(Absolute)
                    .Distinct(NativeToolchains.PathComparer).ToArray(),
                module.Sources.Select(Absolute).Where(source => !Fingerprints.IsUnder(source, output))
                    .Select(source => Path.GetDirectoryName(source)!).Distinct(NativeToolchains.PathComparer).ToArray()))
            .ToArray();
        return new(request.Toolchain.CompilerPath, output, compiles, plan.ModuleOutputs, generated, modules);
    }
}
