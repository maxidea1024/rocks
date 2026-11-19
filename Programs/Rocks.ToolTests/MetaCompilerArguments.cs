namespace Rocks.ToolTests;

// MetaCompiler 모듈 모드의 단일 헤더 probe 인자다(Docs/Design/ReflectionFoundations.ko.md 1.3절). 모듈 이름은 Probe,
// 출력 루트는 <directory>/meta, descriptor는 출력 루트 밖의 <directory>/Probe.rocksmeta.json이다. 생성 헤더는
// <출력 루트>/Public/<철자에서 .h를 .generated.h로 바꾼 경로>, 모듈 생성 소스는 <출력 루트>/Source/Probe.module.generated.cpp다.
// 시험은 빌드 없이 MetaCompiler만 실행하므로 --verify-sources로 생성 소스(등록 함수 정의)도 게시 전에 컴파일하게 한다
// (Docs/Design/ReflectionGeneratedSources.ko.md 7절 — 빌드에서는 그 모듈의 compile action이 한다).
internal static class MetaCompilerArguments
{
    public const string Module = "Probe";

    public static string OutputRoot(string directory) => Path.Combine(directory, "meta");

    public static string Descriptor(string directory) => Path.Combine(directory, Module + ".rocksmeta.json");

    public static string GeneratedPath(string directory, string include) =>
        Path.Combine(OutputRoot(directory), "Public", include[..^2] + ".generated.h");

    public static string AggregatePath(string directory) =>
        Path.Combine(OutputRoot(directory), "Public", Module, Module + ".module.generated.h");

    // 생성 소스도 검증 컴파일하게 하는 MetaCompiler 옵션이다.
    public const string VerifySources = "--verify-sources";

    public static string ModuleSourcePath(string directory, string module = Module) =>
        Path.Combine(OutputRoot(directory), "Source", module + ".module.generated.cpp");

    // module은 생성할 모듈 이름이다(Binds 시험은 CoreTypeBindings로 실행한다). descriptor 경로는 모듈과 무관하게 같다.
    public static string[] Single(string directory, string input, string include, string module = Module) =>
        ["--module", module, "--input", input, "--include", include, "--output-root", OutputRoot(directory), "--descriptor", Descriptor(directory),
            VerifySources];
}
