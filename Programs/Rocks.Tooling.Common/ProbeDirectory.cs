namespace Rocks.Tooling.Common;

/// <summary>
/// 외부 환경 스크립트·probe를 돌릴 전용 임시 작업 디렉터리다. 만들 때 새 디렉터리를 만들고 Dispose에서 지운다. 스크립트가 작업
/// 디렉터리에 남기는 파일(예: VS vcpkg-init.cmd의 `$null`)이 사용자 디렉터리를 건드리지 않게 한다.
/// </summary>
public sealed class ProbeDirectory : IDisposable
{
    /// <summary>임시 작업 디렉터리의 절대 경로다.</summary>
    public string Path { get; } = Directory.CreateTempSubdirectory("rocks-probe-").FullName;

    /// <summary>디렉터리를 지운다. 자식 프로세스가 아직 잡고 있어 지우지 못하면 임시 디렉터리에 남긴다(OS 임시 정리 대상이다).</summary>
    public void Dispose()
    {
        try
        {
            Directory.Delete(Path, recursive: true);
        }
        catch (IOException)
        {
        }
        catch (UnauthorizedAccessException)
        {
        }
    }
}
