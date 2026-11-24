using System.Text.Json.Serialization;

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
/// Build/MeasureCoverage.ps1이다(Docs/Design/AssetSystem 14.1절).</item>
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
/// Windows 실행 파일의 subsystem이다(링크 <c>/SUBSYSTEM:CONSOLE</c>·<c>/SUBSYSTEM:WINDOWS</c>).
/// <list type="bullet">
/// <item>Console(기본): 콘솔 프로그램. 진입 기호는 <c>wmain</c>이다(Launch <c>ROCKS_IMPLEMENT_APPLICATION</c>).</item>
/// <item>Windows: GUI 프로그램. 콘솔 창이 붙지 않는다. 진입 기호는 <c>wWinMain</c>이다
/// (Launch <c>ROCKS_IMPLEMENT_WINDOWED_APPLICATION</c>). 매크로와 맞지 않으면 진입 기호가 없어 링크 오류다.</item>
/// </list>
/// 다른 플랫폼 toolchain은 이 값을 쓰지 않는다. 정적 라이브러리에 Windows를 두면 입력 오류다
/// (Docs/Design/CrashHandlingAndLaunch.ko.md 7.4절).
/// </summary>
public enum WindowsSubsystem { Console, Windows }

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
/// <summary>
/// 번역 단위 언어다. <see cref="ObjectiveCpp"/>는 Apple 대상의 <c>.mm</c>(Objective-C++, Docs/Design/MacPlatform.ko.md 6절 MD2)이며
/// C++와 같은 언어 인자에 Objective-C 런타임 인자를 더한다.
/// </summary>
public enum NativeLanguage { Cpp, C, ObjectiveCpp }

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

    // 컴파일러가 검증한 도구 모음 목록(Build/Toolchain.json — Docs/Design/ClangVersionSupport.ko.md 6.2절)에 있는지다. discovery가 채우고,
    // 수동으로 만든 툴체인(시험)은 참이다. 거짓이면 빌드 CLI가 한 줄 알리고 build-evidence에 남긴다.
    public bool ToolchainVerified { get; init; } = true;

    // Windows에서 고른 MSVC STL이 요구하는 최소 Clang 확인 결과다(build-evidence의 stlClangCheck — "requires Clang 22" 또는 "not checked: 이유").
    // Windows 밖과 수동으로 만든 툴체인은 null이다.
    public string? StlClangCheck { get; init; }

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

/// <summary>
/// 모듈을 선언한 플러그인이다(Docs/Design/ModuleLoader 3.15절). <paramref name="Name"/>은 플러그인 이름(C 식별자),
/// <paramref name="Directory"/>는 플러그인 루트(엔진 저장소 <c>Engine/Plugins</c>, 프로젝트 <c>Plugins</c>) 기준 플러그인 폴더다(<c>/</c> 구분,
/// 예 <c>Importers/UnrealExchange</c>).
/// </summary>
public sealed record ModulePlugin(string Name, string Directory);

/// <summary>
/// 소스 묶음이다(Docs/Design/BuildTargets 3.5절 BS1b, BD22 개정). <paramref name="Directory"/>(루트 기준 또는 절대 경로) 아래 소스는 include 소유
/// 검사(BD11)에서 대상 모듈 <paramref name="Target"/>의 세 영역, T의 컴파일 환경에 보이는 모듈의 공개·시험 영역, 추가 모듈
/// <paramref name="Modules"/>의 시험 영역과 그 공개 환경의 공개 영역, <paramref name="Internals"/>의 비공개 영역(시험 접근 헤더 — 명시한
/// 것만), 그리고 묶음을 가진 모듈 자신을 본다. 컴파일 환경은 바꾸지 않는다(모듈 하나의 환경 그대로 — 유니티 묶음만 소스 묶음 경계를 넘지
/// 않는다). 시험 실행 파일 규칙이 시험 대상 모듈마다 하나씩 만든다. 사용 예:
/// <code>new SourceGroup("Engine/Source/Developer/AssetCooker/Tests", "AssetCooker", ["AssetTestFixtures"], ["AssetRuntime"])</code>
/// </summary>
public sealed record SourceGroup(string Directory, string Target, IReadOnlyList<string> Modules, IReadOnlyList<string>? Internals = null)
{
    /// <summary>비공개 영역이 보이는 모듈이다(없으면 빈 목록).</summary>
    public IReadOnlyList<string> InternalModules => Internals ?? [];
}

/// <summary>입력 경로는 request.RootDirectory 기준이다. dependency의 public 설정만 전파한다.</summary>
public sealed record NativeModuleInput
{
    public required string Name { get; init; }

    public NativeModuleKind Kind { get; init; } = NativeModuleKind.StaticLibrary;

    // Windows 실행 파일의 subsystem이다(WindowsSubsystem). 실행 파일에만 뜻이 있고 dependency로 전파하지 않는다.
    public WindowsSubsystem WindowsSubsystem { get; init; } = WindowsSubsystem.Console;

    public IReadOnlyList<string> Sources { get; init; } = [];

    public IReadOnlyList<string> PublicIncludeDirectories { get; init; } = [];

    public IReadOnlyList<string> PrivateIncludeDirectories { get; init; } = [];

    public IReadOnlyList<string> PublicDefines { get; init; } = [];

    // 이 모듈에 의존하는 모듈(컴파일 환경에 이 모듈이 보이는 모듈)만 받는 정의다. 이 모듈 자신의 컴파일에는 넣지 않는다(PublicDefines와
    // 다른 점). 직접 빌드하는 서드파티 라이브러리의 소비자 정의(ThirdParty.json consumerDefines — 예 ZSTD_STATIC_LINKING_ONLY)에 쓴다:
    // 라이브러리 원본이 같은 매크로를 스스로 #define하므로 자기 컴파일에 넣으면 재정의가 된다(Docs/Design/BuildTargets 3.5절 BS3a).
    public IReadOnlyList<string> ConsumerDefines { get; init; } = [];

    public IReadOnlyList<string> PrivateDefines { get; init; } = [];

    // 공개 의존이다(UE PublicDependencyModuleNames, Docs/Design/BuildTargets 3.5절 BD7). 이 모듈의 공개 헤더가 쓰는 모듈이며, 그
    // 모듈의 공개 환경(public include·정의, 그 공개 의존의 공개 환경)이 이 모듈과 소비자에게 보인다. 두 목록에 함께 적으면 입력 오류다.
    public IReadOnlyList<string> PublicDependencies { get; init; } = [];

    // 비공개 의존이다(UE PrivateDependencyModuleNames). 구현만 쓰는 모듈이며, 그 공개 환경은 이 모듈에만 보이고 소비자에게는 넘어가지
    // 않는다. 링크는 공개 의존과 같이 한다.
    public IReadOnlyList<string> PrivateDependencies { get; init; } = [];

    // 두 목록을 공개 다음 비공개 순으로 이은 것이다. 링크·빌드 대상·순환 검사·모듈 표처럼 보임이 아니라 도달을 따라가는 곳이 쓴다
    // (보임은 ModuleVisibility).
    public IEnumerable<string> AllDependencies => PublicDependencies.Concat(PrivateDependencies);

    // 실행 중에 ModuleLoader::Load(이름)으로 적재하는 모듈이다(UE DynamicallyLoadedModuleNames 대응, Docs/Design/ModuleLoader 3.8절
    // MD11). 이 모듈의 컴파일에 그 모듈의 include·정의를 넣지 않는다(쓰는 쪽은 별도 모듈의 interface와 Find<T>(이름)로 쓴다). 빌드는
    // 그 모듈과 링크 폐포를 타깃에 넣어 빌드하고, 이 모듈에 닿는 실행 파일이 함께 링크하며(Monolithic) 실행 파일 모듈 표에 행을 싣는다.
    // 순환 검사의 참조 사슬에는 넣지 않는다(UE와 같다 — Engine이 적재하는 모듈이 Engine에 링크 의존해도 된다). 두 의존 목록과 겹치거나
    // 자기 자신·실행 파일·모르는 이름이면 입력 오류다(InputValidation). 사용 예(엔진 모듈 규칙):
    // protected override string[] DynamicModules => ["RendererPlugin"];
    public IReadOnlyList<string> DynamicModules { get; init; } = [];

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

    // true이면 이 모듈의 C++ 번역 단위에 RTTI를 켠다(Windows clang-cl /GR을 /GR- 뒤에, 그 밖 -frtti를 -fno-rtti 뒤에). 서드파티
    // 모듈만 가질 수 있고(ThirdParty.json rtti — 자기 클래스 계층에서 dynamic_cast·typeid를 쓰는 ICU4C), 아니면 입력 오류다
    // (Docs/Design/Compression.ko.md 3.3절 C++ 서드파티). C 번역 단위와 dependency에는 영향이 없다. 모듈 인자로 RTTI를 켜는 것은
    // 계속 거부한다(InputValidation).
    public bool RuntimeTypeInformation { get; init; }

    // 리플렉션 메타데이터 헤더의 include 철자다(이 모듈의 public 또는 private include 디렉터리 기준,
    // Docs/Design/ReflectionFoundations.ko.md 1.1·1.10절). 모듈 규칙은 쓰지 않는다 — MetadataLayout이 ModuleDirectory 안 include
    // 디렉터리의 표지(ROCKS_CLASS·ROCKS_STRUCT·ROCKS_ENUM) 헤더를 찾아 채우고(규칙이 채우면 거부한다), 생성 include 디렉터리
    // (<out>/Generated/<Name>/Public·Private)를 이 모듈에 더한다.
    public IReadOnlyList<string> Metadata { get; init; } = [];

    // 프로젝트 루트 기준 모듈 폴더다. identity 명세 <ModuleDirectory>/<Name>.rocksids.json의 위치를 정한다.
    public string? ModuleDirectory { get; init; }

    // 모듈 종류다(Docs/Design/BuildTargets 3.9절 BD38). 엔진 모듈은 규칙의 Group 폴더(SourceModuleRules), 플러그인 모듈은 기술자
    // (PluginModuleRules)가 정한다. Release 배제 정책이 Developer 모듈을 이 칸으로 찾는다. 실행 파일·서드파티는 Runtime이다.
    public Descriptors.ModuleType Type { get; init; } = Descriptors.ModuleType.Runtime;

    // 이 실행 파일에 링크할 켠 플러그인 모듈이다(Docs/Design/BuildTargets 3.9절 BD36, UE AddModuleToBinary). 빌드가 타깃의 주 실행
    // 파일(TargetRules.Modules[0])에 채우고 규칙은 채우지 않는다(RuleLoader가 거부). 의존이 아니므로 include·정의를 받지 않고, 링크 폐포 뒤에
    // 그 모듈과 링크 폐포를 링크하며 모듈 표에 보통 행(기술자 단계로 시작)으로 싣는다. 실행 파일만 가질 수 있다.
    public IReadOnlyList<string> PluginModules { get; init; } = [];

    // 소스 묶음이다(SourceGroup — Docs/Design/BuildTargets 3.5절 BS1b). 비면 모든 소스가 이 모듈의 보임을 쓴다. 묶음 대상 모듈의 두 의존 목록은
    // 규칙 로더가 이 모듈의 비공개 의존에 더한다(SourceGroups.WithTargetDependencies — 컴파일 환경은 묶음 대상 환경의 합집합이다).
    public IReadOnlyList<SourceGroup> SourceGroups { get; init; } = [];

    // 공개 헤더 검사 모듈이면 검사 대상 모듈 이름이다(Docs/Design/BuildTargets 3.5절 BS1a·BS1b). 빌드가 그 검사의 의존 기록으로 대상 모듈의
    // 쓰지 않는 공개 의존을 알린다. 규칙은 채우지 않는다.
    public string? PublicHeaderCheckOf { get; init; }

    // 이 모듈을 선언한 플러그인이다(Docs/Design/ModuleLoader 3.15절 MD23). 기술자 모듈 처리(DescriptorModuleSet.Apply)가 플러그인
    // 모듈에 채우고 규칙은 채우지 않는다(RuleLoader가 거부). Modular에서 그 공유 라이브러리와 플러그인 manifest의 폴더를 정한다
    // (LinkTypes.SharedLibraryDirectory). 엔진·프로젝트 모듈은 null이다.
    public ModulePlugin? Plugin { get; init; }

    // 설치 SDK 모듈 전용: 미리 생성된 descriptor와 identity 명세다. 다시 생성하지 않고 import만 한다.
    // 이 모듈은 리플렉션 헤더를 찾지 않는다(미리 생성된 descriptor를 쓴다).
    public ImportedMetadata? ImportedMetadata { get; init; }

    // 리플렉션 생성 소스(MetaCompiler가 낸 번역 단위)의 경로다(Docs/Design/ReflectionGeneratedSources.ko.md 6.3절). Metadata가 있는 모듈에
    // Rocks.Build.Cli의 MetadataLayout이 <out>/Generated/<Name>/Source/<Name>.module.generated.cpp(모듈의 타입 등록 함수 정의)를 넣는다.
    // 규칙이 직접 채우지 않는다. 파일은 메타 생성이 게시한 뒤, 액션 그래프를 세우기 전에 있어야 한다. 액션 그래프는 Sources와 같은
    // include·정의·프로필로 컴파일해 모듈의 archive(실행 파일 모듈이면 그 실행 파일)에 넣는다. 그래서 손으로 쓴 소스가 없는 리플렉션
    // 모듈도 정적 라이브러리가 되어 ModuleOutputs에 실린다.
    // Sources와 따로 두는 이유: 진입점 모듈의 자기 타입 표지(ROCKS_MODULE_TYPES=1 — 모듈 생성 소스가 RegisterModuleOwnTypes를 정의한다)를
    // 이 목록으로 정한다(Docs/Design/ModuleLoader 3.10절). 모듈 진입점 판정(ModuleTables.EntryModules)에서는 모듈 코드로 센다 —
    // 손으로 쓴 소스가 없는 메타데이터 모듈은 MetadataLayout이 진입점 TU를 GeneratedSources로 더한다. 설치 SDK 모듈은 이 목록이 비어
    // 있다(object가 SDK 라이브러리에 들어 있다).
    public IReadOnlyList<string> ReflectionSources { get; init; } = [];

    // 리플렉션 생성 소스의 compile action이 실패했을 때 컴파일러 출력 뒤에 붙이는 안내문이다(비어 있으면 붙이지 않는다). 본문 계약 위반
    // (생성·소멸 noexcept, 접근 권한, 정적 검사)은 메타 단계가 아니라 이 compile action이 잡으므로 사용자가 생성 코드의 오류를 원본
    // 선언으로 되짚을 길을 알려 준다(Docs/Design/ReflectionGeneratedSources.ko.md 7절). 표시 전용이다 — action key에 넣지 않으므로 문구를
    // 바꿔도 다시 컴파일하지 않는다. Rocks.Build.Cli의 MetadataLayout이 채운다.
    public string ReflectionSourceFailureNote { get; init; } = "";

    // 이 모듈이 Core/Build/BuildInfo.h(rocks::build::ToolchainFingerprint·ToolBuildId·CurrentEngineVersion 선언)를 제공한다는 표지다. 이 모듈에
    // 닿는 실행 파일마다 빌드가 정의 TU(<out>/BuildInfo/<실행 파일>.BuildInfo.cpp)를 만들어 그 실행 파일 소스에 더한다
    // (Docs/Design/ReflectionFoundations.ko.md 8.1절). 엔진 Core 규칙이 켜고 설치 SDK가 함께 나른다. 이름 "Core"로 판정하지
    // 않으므로 엔진 밖 프로젝트의 같은 이름 모듈은 대상이 아니다.
    public bool ProvidesBuildInfo { get; init; }

    // 이 모듈이 Core/Module/ModuleEntry.h(모듈 진입점 매크로 ROCKS_IMPLEMENT_MODULE과 모듈 표 형식)를 제공한다는 표지다(Docs/Design/ModuleLoader
    // 3.2·3.9절). 이 모듈에 닿는 실행 파일마다 빌드가 모듈 표 TU(<out>/GeneratedSources/<실행 파일>/<실행 파일>.ModuleTable.cpp)를 만들어 그
    // 실행 파일 소스에 더하고, 이 모듈에 닿는 C++ 모듈(서드파티·header-only 제외)의 소스에 ROCKS_MODULE_NAME=<이름>을 정의한다. 엔진 Core
    // 규칙이 켜고 설치 SDK가 함께 나른다. 이름 "Core"로 판정하지 않는다.
    public bool ProvidesModuleTable { get; init; }

    // 서드파티 라이브러리 모듈 표지다(ThirdPartyModuleRules가 켠다). C++ 모듈 코드가 없으므로 모듈 표에 들지 않고 ROCKS_MODULE_NAME을 받지
    // 않는다(Docs/Design/ModuleLoader 3.1절 면제). 설치 SDK가 함께 나른다.
    public bool ThirdParty { get; init; }

    // 프로세스 전역 상태가 있는 서드파티 모듈 표지다(ThirdPartyModuleRules가 ThirdParty.json globalState로 켠다 — Docs/Design/BuildTargets
    // 3.8절 BD29·BS8c). 켜진 모듈을 직접 의존(두 의존 목록)하는 모듈이 둘 이상이면 입력 오류다(InputValidation): Modular에서 서드파티는
    // 소비 모듈의 공유 라이브러리마다 링크되므로 상태가 두 벌이 된다. 다른 모듈은 그 소비 모듈의 Rocks API를 거친다.
    public bool HasGlobalState { get; init; }

    // 모듈 표 행에 싣는 로딩 단계다(Docs/Design/ModuleLoader 3.3·3.11절). 기술자 모듈(게임·플러그인)은 기술자의 loadingPhase를
    // DescriptorModuleSet.Apply가 넣고, 엔진 모듈은 규칙의 LoadingPhase(기본 Default)다. 모듈 표를 만들 때 행의 의존이 이 단계보다 늦으면
    // 오류다(MD6 — None 의존은 허용).
    public Descriptors.LoadingPhase LoadingPhase { get; init; } = Descriptors.LoadingPhase.Default;

    // 이 모듈이 공통 실행 진입점(Launch — 프로세스 모듈 로더를 소유한다)이라는 표지다(Docs/Design/ModuleLoader 3.9·3.11절). 이 모듈의
    // 링크 폐포에 든 진입점 모듈이 부트 모듈이고, 모듈 표를 만들 때 그 모듈들의 단계가 EarliestPossible이 아니면 오류다. 엔진 Launch 규칙이
    // 켜고 설치 SDK가 함께 나른다. 이름 "Launch"로 판정하지 않는다.
    public bool ProvidesLaunch { get; init; }

    // 유니티 빌드 설정이다(Docs/Design/UnityBuildPch.ko.md 6.1절). 켜면 이 모듈의 손으로 쓴 C++ 소스를 묶음 번역 단위로 컴파일한다.
    // C 소스, BuildInfo·규칙 생성 TU, UnityExclude, 첫 #include 앞에서 #define하는 파일, UnityBatchBytes보다 큰 파일은 언제나 단독
    // 번역 단위다. 켠 모듈은 묶음 충돌 정책 검사(파일 지역 이름·using·매크로·pragma)의 대상이다. dependency로 전파하지 않는다.
    public UnityMode Unity { get; init; } = UnityMode.Default;

    // 묶음 하나의 구성원 바이트 합 상한이다(실제 파일 크기). 구간이 이 값을 넘으면 파일 수로 균등 분할한다.
    public int UnityBatchBytes { get; init; } = UnityBuild.DefaultBatchBytes;

    // 구간의 기대 파일 수다(경로 해시를 이 값으로 나눈 나머지가 0인 파일이 새 구간을 연다).
    public int UnityAnchorFiles { get; init; } = UnityBuild.DefaultAnchorFiles;

    // 묶지 않고 단독 번역 단위로 컴파일할 소스다(Sources의 항목과 같은 경로 — 큰 파일, 특수한 설정이 필요한 파일, object 단위로 검사하는
    // 파일).
    public IReadOnlyList<string> UnityExclude { get; init; } = [];

    // 리플렉션 생성 소스(ReflectionSources)를 자기들끼리 묶는가. 생성 소스는 파일 지역 이름·매크로 없이 나오므로 기본으로 묶는다
    // (Docs/Design/ReflectionGeneratedSources.ko.md GSD9). 손으로 쓴 소스의 Unity 설정과 따로이고, 요청이 유니티를 끄면 묶지 않는다.
    public bool ReflectionUnity { get; init; } = true;

    // 모듈 PCH 헤더의 경로다(Docs/Design/UnityBuildPch.ko.md 8.1절 — 루트 기준 또는 절대 경로). 있으면 이 모듈의 C++ 번역 단위가 이 헤더를
    // 미리 컴파일한 PCH를 쓴다. 소스는 이 헤더를 include하지 않는다 — 빌드가 강제 include하고, PCH 없이도(--pch off) 컴파일되어야 한다.
    // PCH를 쓸 수 있는 손으로 쓴 소스가 PrivatePchMinTus보다 적으면 만들지 않는다.
    public string? PrivatePchHeader { get; init; }

    // PCH 사용 방식이다(8.1절). 외부 라이브러리 모듈은 설정과 무관하게 쓰지 않는다.
    public PchUsage PchUsage { get; init; } = PchUsage.Auto;

    // 모듈 PCH를 만드는 손으로 쓴 소스 수의 하한이다(묶음 구성원을 하나씩 센다. PCH를 쓰지 않는 소스와 리플렉션 생성 소스 제외 —
    // 유니티 설정·IDE 계획과 무관하게 같은 결정이 나온다, Docs/Design/UnityBuildPch.ko.md 8.1절). 이보다 적으면 PCH를 만드는 비용이
    // 절약분보다 크다.
    public int PrivatePchMinTus { get; init; } = PchBuild.DefaultMinimumTranslationUnits;

    // 공유 PCH 헤더의 철자다(이 모듈의 public include 디렉터리 기준, 8.4절). 있으면 이 모듈이 공유 PCH를 제공한다: 모듈 PCH가 없는
    // 소비 모듈이 자기 인자로 이 헤더의 PCH를 만들어 쓴다(변형마다 하나). 헤더는 이 모듈과 그 의존 모듈의 public 헤더만 include해야
    // 한다.
    public string? SharedPchHeader { get; init; }

    // 작업 파일 분리(7.2절)로 묶음에서 빠진 번역 단위도 PCH를 쓰는가(기본 참).
    public bool AdaptivePch { get; init; } = true;

    // 규칙이 저장소 입력(예: Engine 루트 ThirdParty manifest)에서 만든 생성 번역 단위다(Docs/Design/ShaderSystem 9절 도구 표).
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

/// <summary>
/// 링크 방식이다(Docs/Design/BuildTargets 3.8절 BD17, UE TargetLinkType). Monolithic은 모든 모듈을 정적 라이브러리로 실행 파일 하나에
/// 링크하고, Modular는 서드파티가 아닌 라이브러리 모듈마다 공유 라이브러리 하나를 만든다.
/// </summary>
public enum NativeLinkType { Monolithic, Modular }

/// <summary>링크 방식 매크로와 모듈 API 매크로·공유 라이브러리 이름 규칙이다(Docs/Design/BuildTargets 3.8절 BD17·BD27).</summary>
public static class LinkTypes
{
    /// <summary>링크 방식 매크로 이름이다(UE IS_MONOLITHIC). 모든 compile이 0·1 중 하나를 받고 Base/Compiler.h가 기본값 없이 검사한다.</summary>
    public const string MonolithicMacroName = "ROCKS_MONOLITHIC";

    /// <summary>compile 인자로 넘기는 링크 방식 정의(<c>ROCKS_MONOLITHIC=1</c> 또는 <c>=0</c>)다.</summary>
    public static string MonolithicDefine(NativeLinkType linkType) => MonolithicMacroName + "=" + (linkType == NativeLinkType.Monolithic ? "1" : "0");

    /// <summary>모듈 API 매크로 이름이다(UE &lt;MODULE&gt;_API): <c>ROCKS_&lt;대문자 모듈 이름&gt;_API</c>.</summary>
    public static string ApiMacro(string module) => "ROCKS_" + module.ToUpperInvariant() + "_API";

    /// <summary>빌드가 정의하는 모듈 API 매크로 철자인가(<c>ROCKS_</c>로 시작해 <c>_API</c>로 끝나는 이름 — 규칙은 정의할 수 없다).</summary>
    public static bool IsApiMacro(string name) =>
        name.Length > "ROCKS__API".Length && name.StartsWith("ROCKS_", StringComparison.Ordinal) && name.EndsWith("_API", StringComparison.Ordinal);

    /// <summary>export 철자다(Base/Compiler.h의 ROCKS_DLLEXPORT와 같다). 정의 헤더가 Base 헤더 없이도 쓸 수 있게 플랫폼 철자를 직접 쓴다.</summary>
    public static string ExportSpelling(NativePlatform platform) =>
        platform == NativePlatform.Windows ? "__declspec(dllexport)" : "__attribute__((visibility(\"default\")))";

    /// <summary>import 철자다(Base/Compiler.h의 ROCKS_DLLIMPORT와 같다).</summary>
    public static string ImportSpelling(NativePlatform platform) =>
        platform == NativePlatform.Windows ? "__declspec(dllimport)" : "__attribute__((visibility(\"default\")))";

    /// <summary>
    /// 모듈 하나의 API 매크로 정의(<c>이름=값</c>)다. Monolithic이거나 공유 라이브러리를 만들지 않는 모듈(컴파일할 코드가 없는 모듈 —
    /// header-only, 미리 빌드한 라이브러리만 있는 모듈)이면 빈 값, Modular면 그 모듈 자신의 compile에서는 export, 다른 모듈의 compile에서는
    /// import다(UE UEBuildModule.cs:750-770). 판정은 ActionGraph가 공유 라이브러리를 만드는 조건과 같다.
    /// </summary>
    public static string ApiDefine(NativeModuleInput module, bool own, NativeLinkType linkType, NativePlatform platform)
    {
        bool hasBinary = module.Sources.Count != 0 || module.GeneratedSources.Count != 0 || module.ReflectionSources.Count != 0;
        string value = linkType == NativeLinkType.Monolithic || !hasBinary ? "" : own ? ExportSpelling(platform) : ImportSpelling(platform);
        return ApiMacro(module.Name) + "=" + value;
    }

    /// <summary>
    /// 링크 방식이 그 구성을 빌드할 수 있는가다. Modular는 Debug·Development만이다(배포·sanitizer·coverage 구성은 Monolithic — 입력 검사와
    /// 프로젝트 파일 생성이 같은 판정을 쓴다).
    /// </summary>
    public static bool SupportsConfiguration(NativeLinkType linkType, NativeConfiguration configuration) =>
        linkType == NativeLinkType.Monolithic || configuration is NativeConfiguration.Debug or NativeConfiguration.Development;

    /// <summary>
    /// Modular 호스트 모듈인가(Docs/Design/ModuleLoader 3.14절 ML4b): Modular에서 모듈 표 제공 모듈에 닿는(진입점 모듈인) 실행 파일
    /// 모듈이다. 공유 라이브러리로 링크하고 실행 파일은 빌드 생성 진입 TU와 Launch만 링크한다. 입력 검사가 그런 실행 파일이 Launch에 닿게 한다.
    /// </summary>
    public static bool IsHostModule(NativeModuleInput module, NativeLinkType linkType, bool entryModule) =>
        linkType == NativeLinkType.Modular && module.Kind == NativeModuleKind.Executable && entryModule;

    /// <summary>
    /// 모듈 코드가 공유 라이브러리로 링크되는가: Modular에서 서드파티가 아닌 라이브러리 모듈과 호스트 모듈(<see cref="IsHostModule"/>)이다.
    /// 그 진입점 모듈의 소스가 <c>ROCKS_MODULE_SHARED_LIBRARY=1</c>을 받는다.
    /// </summary>
    public static bool CompilesAsSharedLibrary(NativeModuleInput module, NativeLinkType linkType, bool entryModule) =>
        linkType == NativeLinkType.Modular && !module.ThirdParty &&
        (module.Kind == NativeModuleKind.StaticLibrary || IsHostModule(module, linkType, entryModule));

    /// <summary>
    /// Modular 공유 라이브러리 파일 이름(확장자 앞까지)이다: <c>&lt;타깃&gt;-&lt;모듈&gt;</c>, Development가 아니면
    /// <c>-&lt;플랫폼&gt;-&lt;구성&gt;</c>을 붙이고 macOS·Linux는 <c>lib</c> 접두를 붙인다(UE UEBuildTarget.cs:5400, 5473-5508).
    /// </summary>
    public static string SharedLibraryStem(string target, string module, NativePlatform platform, NativeConfiguration configuration)
    {
        string stem = target + "-" + module + (configuration == NativeConfiguration.Development ? "" : "-" + platform + "-" + configuration);
        return platform == NativePlatform.Windows ? stem : "lib" + stem;
    }

    /// <summary>
    /// 플랫폼 폴더 이름이다(UE Binaries/&lt;Platform&gt;): Windows <c>Win64</c>, macOS <c>Mac</c>, Android <c>Android</c>. 빌드 문맥
    /// (<c>BuildContext.PlatformDirectory</c>)과 플러그인 binaries 폴더가 같은 표를 쓴다. 새 플랫폼은 명시적으로 더한다. 모르는 값은
    /// <see cref="ArgumentOutOfRangeException"/>이고 그 인자 이름은 <paramref name="parameterName"/>이다(호출자의 속성 이름을 진단에 쓴다).
    /// </summary>
    public static string PlatformDirectory(NativePlatform platform, string parameterName = "platform") => platform switch
    {
        NativePlatform.Windows => "Win64",
        NativePlatform.MacOS => "Mac",
        NativePlatform.Android => "Android",
        _ => throw new ArgumentOutOfRangeException(parameterName, platform, "Unsupported native platform.")
    };

    /// <summary>
    /// Modular 공유 라이브러리의 폴더다(Docs/Design/ModuleLoader 3.15절 MD23). 엔진·프로젝트 모듈은 <c>&lt;출력&gt;/bin</c>, 플러그인 모듈
    /// (<see cref="NativeModuleInput.Plugin"/>)은 <c>&lt;출력&gt;/Plugins/&lt;플러그인 폴더&gt;/Binaries/&lt;Platform&gt;</c>이다. 링크 출력·
    /// manifest·export 수 진단이 이 함수 하나를 쓴다. 사용 예: UnrealExchange 플러그인 모듈의 Windows 폴더는
    /// <c>&lt;출력&gt;/Plugins/Importers/UnrealExchange/Binaries/Win64</c>다.
    /// </summary>
    public static string SharedLibraryDirectory(string output, NativeModuleInput module, NativePlatform platform) =>
        module.Plugin is { } plugin
            ? Path.Combine([output, "Plugins", .. plugin.Directory.Split('/'), "Binaries", PlatformDirectory(platform)])
            : Path.Combine(output, "bin");

    /// <summary>공유 라이브러리 확장자다(Windows <c>.dll</c>, macOS <c>.dylib</c>, 그 밖 <c>.so</c>).</summary>
    public static string SharedLibraryExtension(NativePlatform platform) =>
        platform == NativePlatform.Windows ? ".dll" : platform == NativePlatform.MacOS ? ".dylib" : ".so";

    /// <summary>
    /// 공유 라이브러리 파일 이름이다(<see cref="SharedLibraryStem"/>과 확장자). 사용 예:
    /// <c>LinkTypes.SharedLibraryFileName("Editor", "Core", NativePlatform.Windows, NativeConfiguration.Debug)</c>는
    /// <c>"Editor-Core-Windows-Debug.dll"</c>이다.
    /// </summary>
    public static string SharedLibraryFileName(string target, string module, NativePlatform platform, NativeConfiguration configuration) =>
        SharedLibraryStem(target, module, platform, configuration) + SharedLibraryExtension(platform);
}

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

    // 타깃 종류와 Program 속성(Docs/Design/BuildTargets 3.2절 BS2a)이다. 모든 compile action이 그 타깃 매크로 넷을 같은 값으로 받고, 빌드
    // 정보 TU가 build::CurrentTargetType()을 이것으로 만든다. 기본은 Program 기본(Game과 같은 매크로 조합)이다.
    public TargetKind Target { get; init; } = TargetKind.Default;

    // 링크 방식이다(TargetRules.LinkType — Docs/Design/BuildTargets 3.8절). Modular는 Android와 Asan·Coverage·Release 구성에서
    // 입력 오류다.
    public NativeLinkType LinkType { get; init; } = NativeLinkType.Monolithic;

    // Modular 공유 라이브러리 이름의 타깃 부분이다(TargetRules.Name). null이면 대상 목록이 하나일 때 그 이름이다.
    public string? TargetName { get; init; }

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

    // 유니티 묶기를 쓰는가(명령 옵션 --unity on|off, Docs/Design/UnityBuildPch.ko.md 11절). false면 모든 소스가 단독 번역 단위다(검증
    // 변형). object 변형 디렉터리 이름(obj/<모듈>/<변형>)에 반영되어 일반 빌드와 검증 빌드가 서로의 object를 덮어쓰지 않는다. 도구 빌드
    // ID와 compile_commands.json에는 반영되지 않는다.
    public bool Unity { get; init; } = true;

    // PCH를 쓰는가(명령 옵션 --pch on|off). Unity와 같이 object 변형 디렉터리 이름에 반영된다.
    public bool Pch { get; init; } = true;

    // 작업 파일 분리를 쓰는가(명령 옵션 --adaptive on|off, 7절). true면 엔진이 git으로 작업 파일 집합을 구해(SourceWorkingSet) 그
    // 소스를 유니티 묶음에서 빼 단독 번역 단위로 돌린다. 라이브러리 기본은 false이고 명령줄 도구의 기본은 on이다(CI는 off를 명시한다).
    // 도구 빌드 ID와 compile_commands.json에는 반영되지 않는다.
    public bool Adaptive { get; init; }

    // 작업 파일 집합(정규 절대 경로)이다. null이면 Adaptive에 따라 엔진이 구한다. 시험이 git 없이 집합을 주입할 때 쓴다.
    internal IReadOnlySet<string>? WorkingSet { get; init; }

    // compile·PCH action에 clang -ftime-trace를 붙이고 빌드가 끝나면 집계하는가(명령 옵션 --timing on, Docs/Design/UnityBuildPch.ko.md
    // 12절). 집계는 <out>/Timing/<TimingLabel>.timing.json(번역 단위별 시간, 헤더별 누적 파싱 시간·템플릿 인스턴스화 상위 목록)이다. 인자가
    // 달라지므로 켜고 끌 때 번역 단위가 다시 컴파일된다(측정용이다).
    public bool Timing { get; init; }

    // 집계 파일 이름에 쓰는 이름이다(보통 타깃 이름). null이면 "Build"다.
    public string? TimingLabel { get; init; }

    // 실행한 action의 프로세스마다 CPU 시간과 최대 메모리를 재는가(Docs/Design/UnityBuildPch.ko.md 12절 "묶음 크기 실측" — 명령
    // unity-profile이 켠다). 값은 NativeActionResult의 CpuMilliseconds·PeakMemoryBytes·PeakCommitBytes·ProcessMeasured에 실린다. 측정값이라 action
    // key·도구 빌드 ID·compile_commands.json에는 들어가지 않는다(켜고 꺼도 cache가 그대로 맞는다).
    public bool MeasureProcesses { get; init; }

    // 계획만 세우는가(명령 옵션 --plan-only on). true면 생성 TU·묶음 파일·compile_commands.json까지 쓰고 action은 하나도 실행하지 않는다
    // (증분 기록도 건드리지 않는다). clangd 입력이나 묶음 구성을 빌드 없이 얻을 때 쓴다. 결과의 Actions는 비어 있고 PlanOnly가 참이다.
    public bool PlanOnly { get; init; }

    // 계획 방식이다(Docs/Design/ProjectFiles.ko.md 8.1절). Ide는 IDE 프로젝트 파일 생성용 계획으로, 출력에 아무것도 쓰지 않고 생성물의
    // 존재를 요구하지 않는다. BuildAsync는 받지 않는다(IdePlanner.Plan만 쓴다).
    public NativePlanMode PlanMode { get; init; } = NativePlanMode.Build;

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

    // 이 action이 실행한 프로세스의 CPU 시간(ms, 사용자 + 커널)과 최대 working set·최대 commit(byte)이다.
    // NativeBuildRequest.MeasureProcesses를 켜고 실행했고 호스트가 값을 줄 때만 ProcessMeasured가 참이고, 그 밖에는(cache hit 포함) 모두
    // 기본값이다. 기본값이면 action-report.json에 쓰지 않는다(평소 빌드의 보고서는 이 필드가 없다).
    [JsonIgnore(Condition = JsonIgnoreCondition.WhenWritingDefault)]
    public long CpuMilliseconds { get; init; }

    [JsonIgnore(Condition = JsonIgnoreCondition.WhenWritingDefault)]
    public long PeakMemoryBytes { get; init; }

    [JsonIgnore(Condition = JsonIgnoreCondition.WhenWritingDefault)]
    public long PeakCommitBytes { get; init; }

    [JsonIgnore(Condition = JsonIgnoreCondition.WhenWritingDefault)]
    public bool ProcessMeasured { get; init; }
}

public sealed record NativeBuildResult
{
    public required bool Succeeded { get; init; }

    // IDE 대상 논리 명령(IdePlanner.Select — 출력 아래 소스를 뺀 손으로 쓴 소스)이다. 빌드 CLI가 Visual Studio 프로젝트 파일의 낡음
    // 지문(Docs/Design/ProjectFiles.ko.md 9절)을 계산한다. 보고서(action-report.json)에는 싣지 않는다.
    [JsonIgnore]
    public IReadOnlyList<IdeCompile> IdeCompiles { get; init; } = [];

    public required int ExecutedActions { get; init; }

    public required int SkippedActions { get; init; }

    public required IReadOnlyDictionary<string, string> ModuleOutputs { get; init; }

    public required IReadOnlyList<NativeActionResult> Actions { get; init; }

    public required string CompilationDatabasePath { get; init; }

    // 유니티 묶기에서 자동으로 뺀 소스다: 첫 #include 앞에서 #define하는 파일(Docs/Design/UnityBuildPch.ko.md 6.1절 — 단독 번역 단위로
    // 컴파일하고 PCH도 쓰지 않는다). 경로 ordinal 순이다.
    public IReadOnlyList<string> UnityDefineExcluded { get; init; } = [];

    // 이 빌드의 묶음 번역 단위 수다.
    public int UnityBatches { get; init; }

    // 이 빌드의 PCH action 수다(모듈 PCH와 공유 PCH 변형).
    public int PchActions { get; init; }

    // 작업 파일 분리로 묶음에서 빠진 소스 수다.
    public int AdaptiveSeparated { get; init; }

    // 빌드가 남기는 알림이다(공유 PCH를 쓰지 못한 이유 등). 실패가 아니다.
    public IReadOnlyList<string> Notices { get; init; } = [];

    // 계획만 세운 결과인가(NativeBuildRequest.PlanOnly). 참이면 아무 action도 실행하지 않았고 산출물은 확인되지 않았다.
    public bool PlanOnly { get; init; }

    public string? Failure { get; init; }

    // 이번 빌드의 툴체인 지문(SHA-256 소문자 hex)이다. BuildInfo 대상 실행 파일이 없으면 null이다.
    public string? ToolchainFingerprint { get; init; }

    // 실행 파일 모듈 이름 → 도구 빌드 ID(SHA-256 소문자 hex)다. link가 성공한 BuildInfo 대상 실행 파일만 담는다.
    public IReadOnlyDictionary<string, string> ToolBuildIds { get; init; } = new Dictionary<string, string>(StringComparer.Ordinal);

    // Modular 타깃의 모듈 manifest 경로다(Docs/Design/BuildTargets 3.8절 BS8d1 — ModuleManifests). Monolithic·모듈 표 제공 모듈에 닿지
    // 않는 빌드·실패한 빌드는 null이다.
    public string? ModuleManifestPath { get; init; }
}
