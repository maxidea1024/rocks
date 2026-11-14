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

/// <summary>
/// 구성 매크로다. 컴파일과, 생성 헤더를 따로 컴파일하는 도구(MetaCompiler 검사 시험)가 같은 정의를 쓴다.
/// </summary>
public static class NativeConfigurations
{
    /// <summary>
    /// 구성의 매크로 정의(<c>NAME=1</c>)다. Asan은 Debug 계열이다.
    /// </summary>
    public static string BuildMacro(NativeConfiguration configuration) => configuration switch
    {
        NativeConfiguration.Debug or NativeConfiguration.Asan => "ROCKS_BUILD_DEBUG=1",
        NativeConfiguration.Development => "ROCKS_BUILD_DEVELOPMENT=1",
        NativeConfiguration.Release => "ROCKS_BUILD_RELEASE=1",
        _ => throw new ArgumentException($"Invalid native configuration: {configuration}")
    };
}

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

    // 이하 discovery가 채우는 툴체인 신원이다(Docs/Design/ReflectionFoundations.ko.md 8.1절). 수동으로 만든 툴체인은 빈 값이며,
    // BuildInfo 대상 실행 파일(ProvidesBuildInfo 모듈에 닿는 실행 파일)을 빌드하면 ArgumentException이다.

    // `--version` 출력의 첫 줄에서 경로·URL을 담은 괄호 묶음을 commit hash만 남기고 지운 컴파일러 식별·버전이다.
    // 예: "clang version 21.1.6", "clang version 21.1.6 (a832a5222e48...)".
    public string CompilerVersion { get; init; } = "";

    // `-print-effective-triple`로 얻은 실제 대상 triple이다(compile 인자의 --target·배포 대상 반영).
    // 예: "x86_64-pc-windows-msvc19.29.30159", "arm64-apple-macosx13.0.0", "aarch64-unknown-linux-android26".
    public string TargetTriple { get; init; } = "";

    // 컴파일러 드라이버 바이너리 내용의 SHA-256(소문자 hex 64자)이다. 같은 버전 줄을 내는 다른 벤더 빌드를 구별한다.
    public string CompilerDigest { get; init; } = "";

    // 최종 링크·ThinLTO 코드 생성을 하는 바이너리 내용의 SHA-256(소문자 hex 64자)이다. Windows는 LinkerPath, Android는
    // NDK ld.lld, macOS는 ld와 libLTO.dylib(두 파일 digest를 이은 텍스트의 SHA-256)이다.
    public string LinkerDigest { get; init; } = "";

    // 이름 붙은 툴체인 소유 디렉터리(이름 → 절대 경로)다. 예: resource(컴파일러 resource dir), runtime(RuntimeFiles의
    // 디렉터리), sysroot(macOS SDK), ndk(NDK 루트). 이 아래 파일은 도구 빌드 ID 입력에서 빠지고 지문이 대신한다.
    public IReadOnlyDictionary<string, string> OwnedRoots { get; init; } = new Dictionary<string, string>(StringComparer.Ordinal);

    // 경로가 없는 ABI 필드의 정렬된 key=value 목록이다(컴파일러 버전·SDK 버전·배포 대상·NDK revision·VCToolsVersion).
    public IReadOnlyList<string> AbiFields { get; init; } = [];

    // 빌드 환경에서 지운 컴파일러 동작 변수 이름이다(NativeEnvironment.CompilerBehaviorVariables, ordinal 정렬).
    public IReadOnlyList<string> RemovedEnvironment { get; init; } = [];
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

    // 리플렉션 메타데이터 헤더의 include 철자다(이 모듈의 public 또는 private include 디렉터리 기준,
    // Docs/Design/ReflectionFoundations.ko.md 1.1절). 비어 있지 않으면 ModuleDirectory가 필요하다.
    // 빌드는 생성 include 디렉터리(<out>/Generated/<Name>/Public·Private)를 이 모듈에 더한다.
    public IReadOnlyList<string> Metadata { get; init; } = [];

    // 프로젝트 루트 기준 모듈 폴더다. identity 명세 <ModuleDirectory>/<Name>.rocksids.json의 위치를 정한다.
    public string? ModuleDirectory { get; init; }

    // 설치 SDK 모듈 전용: 미리 생성된 descriptor와 identity 명세다. 다시 생성하지 않고 import만 한다.
    // Metadata와 함께 쓸 수 없다.
    public ImportedMetadata? ImportedMetadata { get; init; }

    // 이 모듈이 Core/Build/BuildInfo.h(rocks::build::ToolchainFingerprint·ToolBuildId 선언)를 제공한다는 표지다. 이 모듈에
    // 닿는 실행 파일마다 빌드가 정의 TU(<out>/BuildInfo/<실행 파일>.BuildInfo.cpp)를 만들어 그 실행 파일 소스에 더한다
    // (Docs/Design/ReflectionFoundations.ko.md 8.1절). 엔진 Core 규칙이 켜고 설치 SDK가 함께 나른다. 이름 "Core"로 판정하지
    // 않으므로 엔진 밖 프로젝트의 같은 이름 모듈은 대상이 아니다.
    public bool ProvidesBuildInfo { get; init; }
}

/// <summary>설치 SDK가 동봉한 모듈 메타데이터다(절대 경로).</summary>
public sealed record ImportedMetadata(string Descriptor, string IdentityManifest);

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

    // 설치 SDK 게임 빌드의 SDK 루트다(그 밖에는 null). 도구 빌드 ID의 경로 키 `sdk:`의 기준이다.
    public string? SdkDirectory { get; init; }

    // response file 사용 방식이다(NativeCommandLine). 기본 Auto이며 시험만 Always·Never로 경로를 강제한다. key에 들어가지 않는다.
    internal ResponseFileMode ResponseFiles { get; init; } = ResponseFileMode.Auto;
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

    // 이번 빌드의 툴체인 지문(SHA-256 소문자 hex)이다. BuildInfo 대상 실행 파일이 없으면 null이다.
    public string? ToolchainFingerprint { get; init; }

    // 실행 파일 모듈 이름 → 도구 빌드 ID(SHA-256 소문자 hex)다. link가 성공한 BuildInfo 대상 실행 파일만 담는다.
    public IReadOnlyDictionary<string, string> ToolBuildIds { get; init; } = new Dictionary<string, string>(StringComparer.Ordinal);
}
