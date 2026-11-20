namespace Rocks.ToolTests;

// 로그 Release 문자열 제외 검사다(Docs/Design/LoggingSystem.ko.md 7절, LG5). `log-release` 명령으로만 돈다.
// Release LogBench 링크 결과에 컴파일 타임 하한 아래(Debug) 템플릿 문자열은 없고 Info 템플릿은 있어야 한다.
// .obj는 ThinLTO bitcode라 최종 실행 파일을 본다. 한 구성·한 linker의 사실이며 일반 보장이 아니다
// (최적화가 참조 없는 리터럴을 버리는 데 기댄다).
//
// 사용 예(CI Release 단계 뒤와 같다):
//   dotnet msbuild Rocks.proj -t:Build -p:Configuration=Release -p:RocksTarget=LogBench -p:NativeOutput=out/ci-logbench-release
//   dotnet out/dotnet/bin/Rocks.ToolTests/Debug/net10.0/Rocks.ToolTests.dll log-release --root . \
//       --native-executable out/ci-logbench-release/bin/LogBench.exe
//
// 실행 파일은 인자로 받고 없으면 실패한다. 정해진 출력 경로를 찾아 없으면 건너뛰던 방식은 CI에서 한 번도 돌지
// 않았고, 로컬에서는 예전에 빌드한 실행 파일을 검사할 수 있었다.
internal static class LogReleaseTests
{
    // LogBench가 Release에서 부르는 두 템플릿이다(Programs/LogBench/Private/Main.cpp).
    private const string CompiledOutMarker = "compiled out {Index} {Ratio}";

    private const string EnabledMarker = "enabled {Index} {Ratio}";

    public static void Register(TestSuite suite, Options options)
    {
        string binary = Path.GetFullPath(options.Required("native-executable"));
        suite.Add("Log.ReleaseBinaryExcludesCompiledOutTemplates", () =>
        {
            Verify.That(File.Exists(binary), $"Release LogBench is missing: {binary}");
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
