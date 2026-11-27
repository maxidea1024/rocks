using System.Text;
using Rocks.Tooling.Common;

namespace Rocks.MetaCompiler;

/// <summary>
/// MetaCompiler가 띄우는 clang 실행은 모두 여기를 거친다(Docs/Design/BuildReuse 2.4절). 실행마다 depfile 인자(clang-cl
/// <c>/clang:-MD /clang:-MF&lt;파일&gt;</c>, clang++ <c>-MD -MF &lt;파일&gt;</c>)를 붙이고, 실행 뒤 depfile의 prerequisite를
/// <see cref="MetaRunRecorder"/>에 적는다. 탐침은 실패를 판정에 쓰므로 실패한 실행이 연 파일도 의존이다 — clang은 보통의 오류로 실패한 실행에도
/// depfile을 쓴다(실측). 다만 치명 오류(include한 헤더가 없음 등)로 멈춘 실행은 depfile을 쓰지 않는다(실측 — 탐침이 이 모듈의 출력 루트를 뺀
/// 순서로 컴파일할 때 생긴다). 그 실행은 실패로 다루지 않고(탐침의 판정 그대로) 이 실행의 의존 기록만 쓸 수 없다고 알린다
/// (<see cref="MetaRunRecorder.NoteUnrecordable"/> — 빌드는 그 모듈을 기록하지 않는다). 성공한 실행에 depfile이 없거나 읽을 수 없으면 RMC3200이다.
/// </summary>
internal static class ClangInvocation
{
    /// <summary>depfile을 얻지 못한 clang 실행의 진단 코드다.</summary>
    public const string MissingDependenciesCode = "RMC3200";

    /// <summary>
    /// spec(컴파일러·인자·작업 디렉터리·환경·표준 입력)으로 clang을 한 번 실행한다. 종료 코드와 출력은 그대로 돌려준다 — 실패 판정은 호출자의
    /// 몫이다.
    /// </summary>
    /// <param name="spec">clang 실행. 인자 끝에 depfile 인자를 더하고 자원 측정을 켠다.</param>
    /// <param name="clangCl">clang-cl 드라이버면 참이다(depfile 인자 철자가 다르다).</param>
    /// <exception cref="MetadataException">depfile이 없거나 깨졌다(RMC3200).</exception>
    public static async Task<ProcessResult> RunAsync(ProcessSpec spec, bool clangCl)
    {
        string depfile = Path.Combine(Path.GetTempPath(), "rocks-meta-" + Guid.NewGuid().ToString("N") + ".d");
        string[] dependencyArguments = clangCl ? ["/clang:-MD", "/clang:-MF" + depfile] : ["-MD", "-MF", depfile];
        try
        {
            ProcessResult result = await ProcessRunner.RunAsync(spec with { Arguments = [.. spec.Arguments, .. dependencyArguments], MeasureUsage = true });
            IReadOnlyList<string> opened;
            try
            {
                opened = Depfile.Parse(File.ReadAllText(depfile, new UTF8Encoding(false, true)), spec.WorkingDirectory);
            }
            catch (Exception error) when (result.ExitCode != 0 && error is FileNotFoundException)
            {
                MetaRunRecorder.Current?.NoteClangRunWithoutDependencies(result.Usage,
                    $"a failing Clang run (exit code {result.ExitCode}) wrote no dependency file");
                return result;
            }
            catch (Exception error) when (error is IOException or UnauthorizedAccessException or InvalidDataException or DecoderFallbackException)
            {
                throw new MetadataException($"Clang did not write a readable dependency file ({error.Message}); exit code {result.ExitCode}:\n" + result.StdErr,
                    MissingDependenciesCode, "Use the supported Clang toolchain; every Clang run of the metadata compiler must report the files it opened.");
            }

            MetaRunRecorder.Current?.NoteClangRun(opened, result.Usage);
            return result;
        }
        finally
        {
            if (File.Exists(depfile))
            {
                File.Delete(depfile);
            }
        }
    }
}
