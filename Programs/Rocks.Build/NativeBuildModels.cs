namespace Rocks.Build;

// Android는 Windows·macOS host에서 NDK로 교차 빌드한다(그 밖의 host/target 조합은 거부).
public enum NativePlatform { Windows, MacOS, Android }

/// <summary>
/// 대상 CPU 아키텍처다. 컴파일러가 보고한 실제 대상 triple(<see cref="NativeToolchain.TargetTriple"/>)에서 정한다.
/// 규칙은 <c>BuildContext.Architecture</c>로 아키텍처별 소스·정의를 고른다(예: 외부 라이브러리의 SIMD 경로 정의).
/// </summary>
public enum NativeArchitecture { X64, Arm64 }

public static class NativeArchitectures
{
    /// <summary>
    /// 대상 triple의 첫 조각으로 아키텍처를 정한다. <c>x86_64</c>·<c>amd64</c>는 X64, <c>aarch64</c>·<c>arm64</c>(<c>arm64e</c> 포함)는
    /// Arm64다. 빈 값이나 지원하지 않는 아키텍처(32 bit 포함)는 <see cref="ArgumentException"/>이다.
    /// </summary>
    public static NativeArchitecture FromTriple(string triple)
    {
        string first = triple.Split('-')[0];
        return first switch
        {
            "x86_64" or "amd64" => NativeArchitecture.X64,
            "aarch64" or "arm64" or "arm64e" => NativeArchitecture.Arm64,
            _ => throw new ArgumentException($"Unsupported target architecture in triple '{triple}' (Rocks supports x64 and ARM64).",
                nameof(triple))
        };
    }
}

/// <summary>
/// 빌드 구성이다(UE5.8 Debug·Development·Shipping 대응).
/// <list type="bullet">
/// <item>Debug: 최적화 없음, 디버그 CRT(Windows /MDd), 디버그 정보. 단계 실행·변수 확인용.</item>
/// <item>Development: 최적화(-O2)와 디버그 정보를 함께 쓰고 검사(Fatal·ROCKS_BUILD_DEVELOPMENT 분기)를
/// 유지한다. 일상 개발·성능 확인용.</item>
/// <item>Release: 최적화와 ThinLTO, NDEBUG. 배포용(UE Shipping). UE Shipping처럼 디버그 정보(PDB)를 만들어
/// 배포본 크래시 덤프를 심볼화한다(PDB는 배포 파일과 분리해 보관한다).</item>
/// <item>Asan: Debug와 같은 코드 생성에 AddressSanitizer를 더한다. 릴리스 CRT를 쓴다.</item>
/// <item>Coverage: Debug와 같은 코드 생성에 clang source-based coverage 계측(<c>-fprofile-instr-generate
/// -fcoverage-mapping</c>, 링크는 clang_rt.profile)을 더한다. 릴리스 CRT를 쓴다(Asan과 같다). 측정 절차는
/// Build/MeasureCoverage.ps1이다(Docs/Design/AssetSystem.ko.md 14.1절).</item>
/// </list>
/// 각 구성은 정확히 하나의 ROCKS_BUILD_DEBUG·ROCKS_BUILD_DEVELOPMENT·ROCKS_BUILD_RELEASE를 1로 정의한다
/// (Asan·Coverage는 ROCKS_BUILD_DEBUG).
/// </summary>
public enum NativeConfiguration { Debug, Development, Release, Asan, Coverage }

/// <summary>
/// 구성 매크로다. 컴파일과, 생성 헤더를 따로 컴파일하는 도구(MetaCompiler 검사 시험)가 같은 정의를 쓴다.
/// </summary>
public static class NativeConfigurations
{
    /// <summary>
    /// 구성의 매크로 정의(<c>NAME=1</c>)다. Asan·Coverage는 Debug 계열이다.
    /// </summary>
    public static string BuildMacro(NativeConfiguration configuration) => configuration switch
    {
        NativeConfiguration.Debug or NativeConfiguration.Asan or NativeConfiguration.Coverage => "ROCKS_BUILD_DEBUG=1",
        NativeConfiguration.Development => "ROCKS_BUILD_DEVELOPMENT=1",
        NativeConfiguration.Release => "ROCKS_BUILD_RELEASE=1",
        _ => throw new ArgumentException($"Invalid native configuration: {configuration}")
    };

    /// <summary>
    /// 대상 OS의 매크로 정의(<c>NAME=1</c>)다. <c>Base/Compiler.h</c>가 정확히 하나만 1인지, 컴파일 대상과 맞는지
    /// 검증하므로 Core 헤더를 컴파일하는 모든 경로(일반 컴파일·MetaCompiler 검사 시험)가 같은 값을 넘긴다.
    /// </summary>
    public static string PlatformMacro(NativePlatform platform) => platform switch
    {
        NativePlatform.Windows => "ROCKS_PLATFORM_WINDOWS=1",
        NativePlatform.MacOS => "ROCKS_PLATFORM_MAC=1",
        NativePlatform.Android => "ROCKS_PLATFORM_ANDROID=1",
        _ => throw new ArgumentException($"Invalid native platform: {platform}")
    };
}

/// <summary>
/// 로그 레벨이다. 값은 C++ <c>rocks::LogLevel</c>(Core/Diagnostics/LogRecord.h)과 같은 순서·숫자다(Trace=0 … Fatal=5).
/// 타깃 규칙의 <c>TargetRules.LogCompiledMinimum</c>으로 컴파일에 남길 최소 레벨을 고를 때 쓴다.
/// </summary>
public enum LogLevel { Trace, Debug, Info, Warning, Error, Fatal }

/// <summary>
/// 로그 컴파일 하한이다(Docs/Design/LoggingSystem.ko.md 6.4절, UE COMPILED_IN_MINIMUM_VERBOSITY 대응). Rocks.Build가 타깃의 모든
/// 모듈·번역 단위에 같은 <c>ROCKS_LOG_COMPILED_MINIMUM</c>을 정의한다. 모듈마다 다르면 Log.h의 inline 함수 본문이 TU마다 달라져
/// ODR 위반이므로 모듈은 이 매크로를 정의하지 못한다(InputValidation이 거부한다).
/// </summary>
public static class LogCompiledMinimums
{
    /// <summary>매크로 이름이다. 값은 <see cref="LogLevel"/>의 숫자다.</summary>
    public const string MacroName = "ROCKS_LOG_COMPILED_MINIMUM";

    /// <summary>구성 기본값이다: Debug·Asan·Coverage는 Trace, Development는 Debug, Release는 Info.</summary>
    public static LogLevel Default(NativeConfiguration configuration) => configuration switch
    {
        NativeConfiguration.Debug or NativeConfiguration.Asan or NativeConfiguration.Coverage => LogLevel.Trace,
        NativeConfiguration.Development => LogLevel.Debug,
        NativeConfiguration.Release => LogLevel.Info,
        _ => throw new ArgumentException($"Invalid native configuration: {configuration}")
    };

    /// <summary>
    /// 매크로 정의(<c>ROCKS_LOG_COMPILED_MINIMUM=n</c>)다. <paramref name="level"/>이 null이면 구성 기본값을 쓴다. 정의되지 않은
    /// 레벨 값은 ArgumentException이다.
    /// </summary>
    public static string Macro(NativeConfiguration configuration, LogLevel? level)
    {
        LogLevel value = level ?? Default(configuration);
        if (!Enum.IsDefined(value))
        {
            throw new ArgumentException($"Invalid log compiled minimum: {(int)value}");
        }

        return MacroName + "=" + ((int)value).ToString(System.Globalization.CultureInfo.InvariantCulture);
    }
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

    // 대상 아키텍처다(TargetTriple에서 정한다). discovery를 거치지 않아 TargetTriple이 빈 툴체인은 ArgumentException이다.
    public NativeArchitecture Architecture => NativeArchitectures.FromTriple(TargetTriple);

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

    // false이면 Release에서도 이 모듈 소스를 ThinLTO bitcode로 만들지 않는다(최적화는 Optimization을 따른다). upstream이
    // LTO 없이 검증한 외부 라이브러리(libsodium 등, ThirdParty.json linkTimeOptimization)에 쓴다. dependency로 전파하지
    // 않으며 링크는 bitcode와 일반 object를 함께 받는다.
    public bool LinkTimeOptimization { get; init; } = true;

    // false이면 Asan 구성에서도 이 모듈 소스에 sanitizer 계측(-fsanitize=address)을 넣지 않는다. 계측한 모듈과 함께 링크되며
    // runtime은 그대로다. sanitizer 계측을 지원하지 않는다고 밝힌 외부 라이브러리(libsodium, ThirdParty.json
    // sanitizerInstrumentation)에 쓴다. dependency로 전파하지 않는다.
    public bool SanitizerInstrumentation { get; init; } = true;

    // 리플렉션 메타데이터 헤더의 include 철자다(이 모듈의 public 또는 private include 디렉터리 기준,
    // Docs/Design/ReflectionFoundations.ko.md 1.1절). 비어 있지 않으면 ModuleDirectory가 필요하다.
    // 빌드는 생성 include 디렉터리(<out>/Generated/<Name>/Public·Private)를 이 모듈에 더한다.
    public IReadOnlyList<string> Metadata { get; init; } = [];

    // 프로젝트 루트 기준 모듈 폴더다. identity 명세 <ModuleDirectory>/<Name>.rocksids.json의 위치를 정한다.
    public string? ModuleDirectory { get; init; }

    // 설치 SDK 모듈 전용: 미리 생성된 descriptor와 identity 명세다. 다시 생성하지 않고 import만 한다.
    // Metadata와 함께 쓸 수 없다.
    public ImportedMetadata? ImportedMetadata { get; init; }

    // 리플렉션 생성 소스(MetaCompiler가 낸 번역 단위)의 경로다(Docs/Design/ReflectionGeneratedSources.ko.md 6.3절). Metadata가 있는 모듈에
    // Rocks.Build.Cli의 MetadataLayout이 <out>/Generated/<Name>/Source/<Name>.module.generated.cpp(모듈의 타입 등록 함수 정의)를 넣는다.
    // 규칙이 직접 채우지 않는다. 파일은 메타 생성이 게시한 뒤, 액션 그래프를 세우기 전에 있어야 한다. 액션 그래프는 Sources와 같은
    // include·정의·프로필로 컴파일해 모듈의 archive(실행 파일 모듈이면 그 실행 파일)에 넣는다. 그래서 손으로 쓴 소스가 없는 리플렉션
    // 모듈도 정적 라이브러리가 되어 ModuleOutputs에 실린다.
    // Sources와 따로 두는 이유: 모듈 진입점 판정(ModuleTables.EntryModules)에 넣지 않는다 — 리플렉션 생성 소스만 있는 모듈은 지금처럼
    // ROCKS_IMPLEMENT_MODULE이 없어도 된다(GSD14, 진입점 모듈로 바꾸는 일은 모듈 로더 ML2). 설치 SDK 모듈은 이 목록이 비어 있다(object가
    // SDK 라이브러리에 들어 있다 — ReflectionOnlyLibrary).
    public IReadOnlyList<string> ReflectionSources { get; init; } = [];

    // 리플렉션 생성 소스의 compile action이 실패했을 때 컴파일러 출력 뒤에 붙이는 안내문이다(비어 있으면 붙이지 않는다). 본문 계약 위반
    // (생성·소멸 noexcept, 접근 권한, 정적 검사)은 메타 단계가 아니라 이 compile action이 잡으므로 사용자가 생성 코드의 오류를 원본
    // 선언으로 되짚을 길을 알려 준다(Docs/Design/ReflectionGeneratedSources.ko.md 7절). 표시 전용이다 — action key에 넣지 않으므로 문구를
    // 바꿔도 다시 컴파일하지 않는다. Rocks.Build.Cli의 MetadataLayout이 채운다.
    public string ReflectionSourceFailureNote { get; init; } = "";

    // 설치 SDK 모듈 전용 표지: PrebuiltLibraries가 리플렉션 생성 소스의 object만 담는다(SDK를 만들 때 손으로 쓴 소스와 규칙 생성 소스가
    // 없던 리플렉션 모듈 — CoreTypeBindings 등). 모듈 진입점이 없으므로 ModuleTables.EntryModules가 이 모듈의 미리 빌드한 라이브러리를
    // 모듈 코드로 세지 않는다(소스 빌드에서 ReflectionSources를 세지 않는 것과 같은 판정이 SDK를 거쳐도 유지된다). 자기 소스
    // (Sources·GeneratedSources·ReflectionSources)가 있는 모듈에 켜면 입력 오류다.
    public bool ReflectionOnlyLibrary { get; init; }

    // 이 모듈이 Core/Build/BuildInfo.h(rocks::build::ToolchainFingerprint·ToolBuildId·CurrentEngineVersion 선언)를 제공한다는 표지다. 이 모듈에
    // 닿는 실행 파일마다 빌드가 정의 TU(<out>/BuildInfo/<실행 파일>.BuildInfo.cpp)를 만들어 그 실행 파일 소스에 더한다
    // (Docs/Design/ReflectionFoundations.ko.md 8.1절). 엔진 Core 규칙이 켜고 설치 SDK가 함께 나른다. 이름 "Core"로 판정하지
    // 않으므로 엔진 밖 프로젝트의 같은 이름 모듈은 대상이 아니다.
    public bool ProvidesBuildInfo { get; init; }

    // 이 모듈이 Core/Module/ModuleEntry.h(모듈 진입점 매크로 ROCKS_IMPLEMENT_MODULE과 모듈 표 형식)를 제공한다는 표지다(Docs/Design/ModuleLoader.ko.md
    // 3.2·3.9절). 이 모듈에 닿는 실행 파일마다 빌드가 모듈 표 TU(<out>/GeneratedSources/<실행 파일>/<실행 파일>.ModuleTable.cpp)를 만들어 그
    // 실행 파일 소스에 더하고, 이 모듈에 닿는 C++ 모듈(서드파티·header-only 제외)의 소스에 ROCKS_MODULE_NAME=<이름>을 정의한다. 엔진 Core
    // 규칙이 켜고 설치 SDK가 함께 나른다. 이름 "Core"로 판정하지 않는다.
    public bool ProvidesModuleTable { get; init; }

    // 서드파티 라이브러리 모듈 표지다(ThirdPartyModuleRules가 켠다). C++ 모듈 코드가 없으므로 모듈 표에 들지 않고 ROCKS_MODULE_NAME을 받지
    // 않는다(Docs/Design/ModuleLoader.ko.md 3.1절 면제). 설치 SDK가 함께 나른다.
    public bool ThirdParty { get; init; }

    // 모듈 표 행에 싣는 로딩 단계다(Docs/Design/ModuleLoader.ko.md 3.3절). 기술자 모듈(게임·플러그인)은 기술자의 loadingPhase를
    // DescriptorModuleSet.Apply가 넣고, 엔진 모듈은 기본 Default다(엔진 모듈 규칙의 단계는 ML1).
    public Descriptors.LoadingPhase LoadingPhase { get; init; } = Descriptors.LoadingPhase.Default;

    // 규칙이 저장소 입력(예: Engine 루트 ThirdParty manifest)에서 만든 생성 번역 단위다(Docs/Design/ShaderSystem.ko.md 9절 도구 표).
    // 빌드가 출력 잠금 아래에서 <out>/GeneratedSources/<모듈>/<FileName>에 내용이 바뀔 때만 쓰고(BuildInfo 생성 TU와 같은 방식) 이
    // 모듈 소스로 컴파일한다. 내용은 호스트 경로를 담지 않아야 한다(도구 빌드 ID가 호스트 무관이다). 규칙은 이 목록을 매 실행 다시
    // 만들므로 입력 manifest가 바뀌면 다음 빌드에서 내용과 compile key가 함께 바뀐다.
    public IReadOnlyList<NativeGeneratedSource> GeneratedSources { get; init; } = [];
}

/// <summary>
/// 규칙이 만든 생성 번역 단위 하나다(<see cref="NativeModuleInput.GeneratedSources"/>). <see cref="FileName"/>은 경로 구분자 없는
/// <c>.cpp</c> 또는 <c>.c</c> 파일 이름이고 모듈 안에서 겹치지 않아야 한다. <see cref="Content"/>는 UTF-8로 쓴다.
/// </summary>
public sealed record NativeGeneratedSource(string FileName, string Content);

/// <summary>설치 SDK가 동봉한 모듈 메타데이터다(절대 경로).</summary>
public sealed record ImportedMetadata(string Descriptor, string IdentityManifest);

public sealed record NativeBuildRequest
{
    public required NativeToolchain Toolchain { get; init; }

    public required string OutputDirectory { get; init; }

    public required IReadOnlyList<NativeModuleInput> Modules { get; init; }

    public string RootDirectory { get; init; } = Directory.GetCurrentDirectory();

    public NativeConfiguration Configuration { get; init; } = NativeConfiguration.Debug;

    // 로그 컴파일 하한의 타깃 재정의다(TargetRules.LogCompiledMinimum). null이면 구성 기본값이다. 모든 compile action이 같은 값을
    // 받는다(LogCompiledMinimums).
    public LogLevel? LogCompiledMinimum { get; init; }

    // 빈 목록은 모든 모듈을 뜻한다. 지정하면 도달하는 dependency만 빌드한다.
    public IReadOnlyList<string> Targets { get; init; } = [];

    public string ContextFingerprint { get; init; } = "";

    public int MaxParallelism { get; init; } = 1;

    // 설치 SDK 게임 빌드의 SDK 루트다(그 밖에는 null). 도구 빌드 ID의 경로 키 `sdk:`의 기준이다.
    public string? SdkDirectory { get; init; }

    // 엔진 버전이다(Docs/Design/ReflectionFoundations.ko.md 8.2절). Rocks.Build.Cli가 build·test·sdk에서는 Build/Build.version을 한 번
    // 읽어, game에서는 SDK manifest의 EngineVersion을 넘긴다. BuildInfo 생성 TU(rocks::build::CurrentEngineVersion)와 manifest의
    // `engine-version:` 줄에만 쓰고 action key·ContextFingerprint·compile 정의에는 넣지 않는다 — 바꾸면 BuildInfo 대상 실행 파일의
    // 생성 TU와 link만 다시 돈다. BuildInfo 대상 실행 파일이 있는데 null이거나 빈 값(0.0.x)이면 ArgumentException이다. 대상이 없으면
    // 쓰지 않는다.
    public EngineVersion? EngineVersion { get; init; }

    // response file 사용 방식이다(NativeCommandLine). 기본 Auto이며 시험만 Always·Never로 경로를 강제한다. key에 들어가지 않는다.
    internal ResponseFileMode ResponseFiles { get; init; } = ResponseFileMode.Auto;
}

public sealed record NativeActionResult(string Id, string Module, string Kind, bool Executed,
    bool Succeeded, int ExitCode, string StdOut, string StdErr, string OutputPath)
{
    // 이 action을 실행하는 데 걸린 벽시계 시간(ms)이다(Docs/Design/ReflectionGeneratedSources.ko.md GSD12). cache 검사 뒤부터 출력 게시와
    // 임시 디렉터리 정리까지를 잰다(실행 슬롯을 기다린 시간은 빼고, 병렬 action끼리는 겹친다). 실행하지 않은 action(cache hit, 선행 action
    // 실패)은 0이다. 측정값이라 action key·도구 빌드 ID·compile_commands.json에는 들어가지 않고 action-report.json에만 실린다.
    public long DurationMilliseconds { get; init; }
}

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
