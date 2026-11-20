using Rocks.Tooling.Common;

namespace Rocks.Build;

/// <summary>
/// 작업 파일 집합이다(Docs/Design/UnityBuildPch.ko.md 7.1절). 편집 중인 소스를 유니티 묶음에서 빼서 단독 번역 단위로 돌리는 데 쓴다 —
/// 그 파일을 고칠 때 묶음 전체가 아니라 그 파일만 다시 컴파일된다. git으로만 판별한다: 작업 사본 루트에서
/// <c>git --no-optional-locks status --porcelain=v1 -z --untracked-files=all</c>을 한 번 실행하고, 상태 두 열(X·Y) 중 하나라도 공백이
/// 아닌 항목(수정 <c>M</c>, 추가 <c>A</c>, 형식 변경 <c>T</c>, 충돌 <c>U</c>, 미추적 <c>??</c>, 이름 변경 <c>R</c>·복사 <c>C</c>의 새 경로)을
/// 작업 파일로 본다. 결과가 나올 때까지 기다리고(시간으로 포기하지 않는다 — 같은 입력에서 다른 묶음 구성이 나오지 않게), git이
/// 실패하면 빌드 오류다. git 작업 사본이 아닌 트리는 빈 집합이다.
/// </summary>
public static class SourceWorkingSet
{
    // ProcessRunner는 유한한 제한만 받는다. 받는 가장 긴 값(약 49일)을 넘겨 사실상 제한 없이 기다린다.
    private static readonly TimeSpan NoTimeout = TimeSpan.FromMilliseconds(uint.MaxValue - 2);

    /// <summary>
    /// <c>status --porcelain=v1 -z</c> 출력에서 작업 파일의 경로(작업 사본 루트 기준, <c>/</c> 구분)를 순서대로 돌려준다. 항목은
    /// <c>XY 경로 NUL</c>이고 이름 변경·복사는 <c>XY 새경로 NUL 원래경로 NUL</c>이다(원래 경로는 작업 파일이 아니다).
    /// </summary>
    /// <exception cref="InvalidDataException">형식이 맞지 않는다.</exception>
    public static IReadOnlyList<string> ParsePorcelain(string output)
    {
        var result = new List<string>();
        string[] entries = output.Split('\0');
        for (int index = 0; index < entries.Length; ++index)
        {
            string entry = entries[index];
            if (entry.Length == 0)
            {
                continue;
            }

            // "XY " 뒤에 경로가 온다(상태 두 글자, 공백 하나).
            const int PathOffset = 3;
            if (entry.Length <= PathOffset || entry[2] != ' ')
            {
                throw new InvalidDataException("Unexpected git status entry: " + entry);
            }

            char staged = entry[0];
            char unstaged = entry[1];
            if (staged is 'R' or 'C' || unstaged is 'R' or 'C')
            {
                // 다음 항목은 원래 경로다.
                ++index;
            }

            if (staged != ' ' || unstaged != ' ')
            {
                result.Add(entry[PathOffset..]);
            }
        }

        return result;
    }

    /// <summary>
    /// directory가 속한 git 작업 사본의 작업 파일을 정규 절대 경로 집합으로 돌려준다. 작업 사본이 아니면 빈 집합이고 notice로 한 번
    /// 알린다. git이 없거나 status가 실패하면 <see cref="InvalidOperationException"/>이다.
    /// </summary>
    public static async Task<IReadOnlySet<string>> QueryAsync(string directory, IReadOnlyDictionary<string, string?>? environment,
        CancellationToken cancellationToken, Action<string>? notice = null)
    {
        var files = new HashSet<string>(NativeToolchains.PathComparer);
        ProcessResult top;
        try
        {
            top = await ProcessRunner.RunAsync(new("git", ["-C", directory, "rev-parse", "--show-cdup"], directory, environment, Timeout: NoTimeout),
                cancellationToken);
        }
        catch (Exception error) when (error is System.ComponentModel.Win32Exception or FileNotFoundException)
        {
            throw new InvalidOperationException("Cannot run git to find the working files (pass --adaptive off to build without it): " + error.Message, error);
        }

        if (top.ExitCode != 0)
        {
            (notice ?? Console.WriteLine)($"Adaptive unity: {directory} is not a git working copy; no source is separated from its batch.");
            return files;
        }

        // 작업 사본 루트는 directory에서 올라갈 상대 경로(--show-cdup, 루트면 빈 줄)로 정한다. --show-toplevel은 symlink·junction을
        // 푼 실제 경로를 돌려주므로(macOS 임시 폴더 /var → /private/var, Windows junction) 빌드가 쓰는 철자의 소스 경로와 맞지 않아
        // 어떤 파일도 작업 파일로 잡히지 않는다. directory 기준으로 만들면 소스와 같은 철자다.
        string up = top.StdOut.Trim();
        string root = Path.GetFullPath(up.Length == 0 ? "." : up, Path.GetFullPath(directory));
        ProcessResult status = await ProcessRunner.RunAsync(new("git",
            ["--no-optional-locks", "-C", root, "status", "--porcelain=v1", "-z", "--untracked-files=all"], directory, environment, Timeout: NoTimeout),
            cancellationToken);
        if (status.ExitCode != 0)
        {
            throw new InvalidOperationException($"git status failed in {root} (exit code {status.ExitCode}); the working files are unknown. " +
                "Fix the repository state or pass --adaptive off.\n" + status.StdErr);
        }

        foreach (string relative in ParsePorcelain(status.StdOut))
        {
            files.Add(Path.GetFullPath(relative, root));
        }

        return files;
    }
}
