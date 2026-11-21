namespace Rocks.Build.Cli;

/// <summary>
/// 링크 판정이다(Docs/Design/ProjectFiles.ko.md 10.6절). symlink·junction처럼 다른 경로를 가리키는 항목만 링크로 본다. OneDrive Files On-Demand
/// 자리 표시자, 데이터 중복 제거 파일 같은 다른 reparse point는 그 자리의 보통 파일·디렉터리라 링크가 아니다(Windows 11 기본 문서·바탕 화면
/// 폴더가 OneDrive 아래인 경우가 많다).
/// </summary>
internal static class FileLinks
{
    /// <summary>경로가 다른 곳을 가리키는 링크인가. 없는 경로는 링크가 아니다.</summary>
    public static bool IsLink(string path)
    {
        if (Directory.Exists(path))
        {
            return new DirectoryInfo(path).LinkTarget is not null;
        }

        return File.Exists(path) && new FileInfo(path).LinkTarget is not null;
    }
}
