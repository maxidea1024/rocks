namespace Rocks.ToolTests;

// self 시험 중 로그 Release 문자열 제외 회귀 검사다(Docs/Design/LoggingSystem.ko.md 14절 LG5). Release
// LogBench 링크 결과에 컴파일 타임 하한 아래(Debug) 템플릿 문자열은 없고 Info 템플릿은 있어야 한다.
// .obj는 ThinLTO bitcode라 최종 실행 파일을 본다. 한 구성·한 linker의 사실이며 일반 보장이 아니다
// (최적화가 참조 없는 리터럴을 버리는 데 기댄다).
internal static partial class SelfTests
{
    // LogBench가 Release에서 부르는 두 템플릿이다(Programs/LogBench/Private/Main.cpp).
    private const string CompiledOutMarker = "compiled out {Index} {Ratio}";

    private const string EnabledMarker = "enabled {Index} {Ratio}";

    private static void RegisterLogRelease(TestSuite suite, Options options)
    {
        suite.Add("Log.ReleaseBinaryExcludesCompiledOutTemplates", () =>
        {
            string platform = OperatingSystem.IsWindows() ? "windows" : "macos";
            string binary = Path.Combine(options.Root, "out", $"dotnet-{platform}-logbench-release", "bin",
                OperatingSystem.IsWindows() ? "LogBench.exe" : "LogBench");
            if (!File.Exists(binary))
            {
                // 이 실행에서 Release를 빌드하지 않았다. 검사할 결과물이 없으므로 건너뛰고 이유를 남긴다.
                Console.WriteLine("SKIP Log.ReleaseBinaryExcludesCompiledOutTemplates: Release LogBench was not built " +
                    $"({binary} is missing). Build it with: dotnet msbuild Rocks.proj -t:Build -p:RocksTarget=LogBench " +
                    "-p:Configuration=Release");
                return;
            }

            byte[] bytes = File.ReadAllBytes(binary);
            Verify.That(IndexOf(bytes, CompiledOutMarker) < 0,
                $"Release LogBench still contains the compiled-out Debug template \"{CompiledOutMarker}\"");
            Verify.That(IndexOf(bytes, EnabledMarker) >= 0,
                $"Release LogBench lacks the Info template \"{EnabledMarker}\" (the scan would prove nothing)");
        });
    }

    private static int IndexOf(byte[] bytes, string text)
    {
        byte[] pattern = System.Text.Encoding.ASCII.GetBytes(text);
        return bytes.AsSpan().IndexOf(pattern);
    }
}
