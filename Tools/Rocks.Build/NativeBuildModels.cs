namespace Rocks.Build;

public enum NativePlatform { Windows, MacOS }
public enum NativeConfiguration { Debug, Release, Asan }
public enum NativeModuleKind { StaticLibrary, Executable }

/// <summary>호스트 실행 도구와 target SDK/ABI 신원을 분리하여 전달한다.</summary>
public sealed record NativeToolchain
{
    public required NativePlatform Platform { get; init; }
    public required string CompilerPath { get; init; }
    public required string ArchiverPath { get; init; }
    public string? LinkerPath { get; init; }
    public required string Identity { get; init; }
    // Installed SDK 호환 판정용. 설치 경로를 제외한 compiler/target/SDK/deployment 계약의 digest다.
    public string AbiIdentity { get; init; } = "";
    public IReadOnlyDictionary<string, string?> Environment { get; init; } = new Dictionary<string, string?>();
    public IReadOnlyList<string> CompileArguments { get; init; } = [];
    public IReadOnlyList<string> LinkArguments { get; init; } = [];
    // SDK/런타임의 내용 변경을 검사할 파일 또는 디렉터리. 선언한 경로가 없으면 오류다.
    public IReadOnlyList<string> FingerprintPaths { get; init; } = [];
    public IReadOnlyList<string> RuntimeFiles { get; init; } = [];
}

/// <summary>입력 경로는 request.RootDirectory 기준이다. dependency의 public 설정만 전파한다.</summary>
public sealed record NativeModuleInput
{
    public required string Name { get; init; }
    public NativeModuleKind Kind { get; init; } = NativeModuleKind.StaticLibrary;
    public IReadOnlyList<string> Sources { get; init; } = [];
    public IReadOnlyList<string> PublicIncludeDirectories { get; init; } = [];
    public IReadOnlyList<string> PrivateIncludeDirectories { get; init; } = [];
    public IReadOnlyList<string> PublicDefines { get; init; } = [];
    public IReadOnlyList<string> PrivateDefines { get; init; } = [];
    public IReadOnlyList<string> Dependencies { get; init; } = [];
    public IReadOnlyList<string> AdditionalCompileArguments { get; init; } = [];
    public IReadOnlyList<string> AdditionalLinkArguments { get; init; } = [];
    public IReadOnlyList<string> PrebuiltLibraries { get; init; } = [];
    // include 디렉터리 밖의 forced include/생성 입력 등은 명시적으로 추가한다.
    public IReadOnlyList<string> AdditionalInputs { get; init; } = [];
}

public sealed record NativeBuildRequest
{
    public required NativeToolchain Toolchain { get; init; }
    public required string OutputDirectory { get; init; }
    public required IReadOnlyList<NativeModuleInput> Modules { get; init; }
    public string RootDirectory { get; init; } = Directory.GetCurrentDirectory();
    public NativeConfiguration Configuration { get; init; } = NativeConfiguration.Debug;
    // 빈 목록은 모든 모듈을 뜻한다. 지정하면 도달하는 dependency만 빌드한다.
    public IReadOnlyList<string> Targets { get; init; } = [];
    public string ContextFingerprint { get; init; } = "";
    public int MaxParallelism { get; init; } = 1;
}

public sealed record NativeActionResult(string Id, string Module, string Kind, bool Executed,
    bool Succeeded, int ExitCode, string StdOut, string StdErr, string OutputPath);

public sealed record NativeBuildResult
{
    public required bool Succeeded { get; init; }
    public required int ExecutedActions { get; init; }
    public required int SkippedActions { get; init; }
    public required IReadOnlyDictionary<string, string> ModuleOutputs { get; init; }
    public required IReadOnlyList<NativeActionResult> Actions { get; init; }
    public required string CompilationDatabasePath { get; init; }
    public string? Failure { get; init; }
}
