namespace Rocks.Build;

// Android는 Windows·macOS host에서 NDK로 교차 빌드한다(그 밖의 host/target 조합은 거부).
public enum NativePlatform { Windows, MacOS, Android }
/// <summary>
/// 빌드 구성이다(UE5.6 Debug·Development·Shipping 대응).
/// <list type="bullet">
/// <item>Debug: 최적화 없음, 디버그 CRT(Windows /MDd), 디버그 정보. 단계 실행·변수 확인용.</item>
/// <item>Development: 최적화(-O2)와 디버그 정보를 함께 쓰고 검사(Fatal·ROCKS_BUILD_DEVELOPMENT 분기)를
/// 유지한다. 일상 개발·성능 확인용.</item>
/// <item>Release: 최적화와 ThinLTO, NDEBUG. 배포용(UE Shipping). UE Shipping처럼 디버그 정보(PDB)를 만들어
/// 배포본 크래시 덤프를 심볼화한다(PDB는 배포 파일과 분리해 보관한다).</item>
/// <item>Asan: Debug와 같은 코드 생성에 AddressSanitizer를 더한다. 릴리스 CRT를 쓴다.</item>
/// </list>
/// 각 구성은 정확히 하나의 ROCKS_BUILD_DEBUG·ROCKS_BUILD_DEVELOPMENT·ROCKS_BUILD_RELEASE를 1로 정의한다
/// (Asan은 ROCKS_BUILD_DEBUG).
/// </summary>
public enum NativeConfiguration { Debug, Development, Release, Asan }
public enum NativeModuleKind { StaticLibrary, Executable }

/// <summary>
/// 모듈 단위 최적화 재정의다(UE ModuleRules.OptimizeCode 대응).
/// <list type="bullet">
/// <item>Default: 구성을 따른다(Debug·Asan은 끔, Development·Release는 켬).</item>
/// <item>Never: 모든 구성에서 끈다. 최적화된 빌드에서 한 모듈만 단계 실행할 때 쓴다(UE DebugGame과 비슷한
/// 효과). Release에서도 이 모듈은 ThinLTO에서 빠진다.</item>
/// <item>Always: 모든 구성에서 켠다. Debug에서 너무 느려 쓸 수 없는 계산 모듈에 쓴다.</item>
/// </list>
/// CRT·디버그 정보·구성 매크로는 바꾸지 않는다(같은 실행 파일 안에서 ABI가 섞이지 않는다).
/// </summary>
public enum CodeOptimization { Default, Never, Always }

/// <summary>
/// 번역 단위의 언어다. 확장자가 정확히 <c>.c</c>인 소스만 C(C17)로, 나머지는 C++(C++23)로 컴파일한다.
/// C는 고정 버전 외부 라이브러리 원본(Core/Source/ThirdParty)용이며 경고·FP·구성 프로필은 C++와 같다.
/// C++ 전용 플래그(RTTI·예외 끄기, __cplusplus 보고)만 빠진다.
/// </summary>
public enum NativeLanguage { Cpp, C }

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
    // 링크할 플랫폼 시스템 라이브러리 이름이다(UE PublicSystemLibraries 대응). 접두사·확장자 없이 쓴다
    // (예: "user32", "d3d12", "log"). 최종 링크에서 Windows는 "<이름>.lib", 그 밖은 "-l<이름>"이 되며,
    // 의존하는 실행 파일의 링크로 전파된다. 플랫폼마다 다르면 규칙이 BuildContext.Platform으로 고른다.
    public IReadOnlyList<string> SystemLibraries { get; init; } = [];
    // 링크할 Apple framework 이름이다(UE PublicFrameworks 대응, 예: "CoreServices"). Apple toolchain에서만
    // "-framework <이름>"이 되며 다른 toolchain으로 빌드하는 요청에 있으면 입력 오류다. 전파는 SystemLibraries와 같다.
    public IReadOnlyList<string> Frameworks { get; init; } = [];
    public IReadOnlyList<string> PrebuiltLibraries { get; init; } = [];
    // include 디렉터리 밖의 forced include/생성 입력 등은 명시적으로 추가한다.
    public IReadOnlyList<string> AdditionalInputs { get; init; } = [];
    // 이 모듈 소스에만 적용하는 최적화 재정의다. dependency로 전파하지 않는다.
    public CodeOptimization Optimization { get; init; } = CodeOptimization.Default;
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
