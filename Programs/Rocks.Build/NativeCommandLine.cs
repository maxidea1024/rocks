using Rocks.Tooling.Common;

namespace Rocks.Build;

/// <summary>action 도구에 response file을 쓰는 방식이다. Auto가 기본이고 Always·Never는 시험이 경로를 강제할 때 쓴다.</summary>
internal enum ResponseFileMode
{
    // 명령줄이 호스트 한계를 넘고 도구의 response file 규칙을 알 때만 쓴다.
    Auto,

    // 규칙을 아는 도구면 길이와 무관하게 쓴다.
    Always,

    // 쓰지 않는다(한계를 넘는 명령은 프로세스 시작이 실패한다).
    Never
}

/// <summary>
/// compile·archive·link 명령을 프로세스로 넘기는 방식을 정한다. 명령줄이 호스트 한계를 넘으면 펼친 인자 전체를 action 임시
/// 디렉터리의 response file에 쓰고 도구를 <c>@파일</c> 인자 하나로 실행한다(Executor).
/// <para>
/// 결정성: action key·compile_commands.json·BuildInfo manifest는 모두 <see cref="NativeAction.Arguments"/>(펼친 인자, 출력은
/// <c>{OUTPUTn}</c> 자리 표시)로 만든다. response file 경로와 임시 디렉터리 이름은 그 어디에도 들어가지 않으므로 response file을
/// 쓰든 안 쓰든 key와 산출물 신원이 같다.
/// </para>
/// </summary>
internal static class NativeCommandLine
{
    /// <summary>
    /// POSIX 호스트의 기준(인자 영역 byte)이다. macOS ARG_MAX(1 MiB, 환경 포함)보다 훨씬 작게 잡아 환경 크기와 무관하게 안전하다.
    /// 이보다 짧은 명령은 지금처럼 직접 넘긴다 — macOS는 CI에서만 검증하므로 실제로 필요한 경우에만 경로를 바꾼다.
    /// </summary>
    public const long PosixArgumentLimit = 128 * 1024;

    /// <summary>
    /// action 임시 디렉터리 안 response file 이름이다. 같은 디렉터리의 staged 출력 이름(16진 object, 모듈 이름 기반 lib·exe·pdb —
    /// 모듈 이름은 영숫자·<c>_ - .</c>만)과 겹칠 수 없게 <c>+</c>를 넣는다.
    /// </summary>
    public const string ResponseFileName = "arguments+.rsp";

    /// <summary>
    /// 그 action 도구가 읽는 response file 규칙이다. 모르는 도구면 null이다(response file을 쓰지 않는다). 판정은 toolchain의 도구
    /// 경로와 대상 플랫폼으로 한다: Windows 대상 컴파일러는 clang-cl(Windows 규칙), 그 밖의 컴파일러는 clang 드라이버(GNU 규칙),
    /// Windows 대상 archiver는 llvm-lib·lib.exe(Windows 규칙), llvm-ar는 호스트 기본(Windows 호스트는 Windows, 그 밖은 GNU)이다.
    /// Apple <c>/usr/bin/ar</c>는 response file을 읽지 않는다고 보고 null이다.
    /// </summary>
    public static ResponseFileQuoting? QuotingOf(NativeToolchain toolchain, NativeAction action)
    {
        if (action.Kind is "Compile" or "Link" && NativeToolchains.PathComparer.Equals(action.Tool, toolchain.CompilerPath))
        {
            return toolchain.Platform == NativePlatform.Windows ? ResponseFileQuoting.Windows : ResponseFileQuoting.Gnu;
        }

        if (action.Kind == "Archive" && NativeToolchains.PathComparer.Equals(action.Tool, toolchain.ArchiverPath))
        {
            if (toolchain.Platform == NativePlatform.Windows)
            {
                return ResponseFileQuoting.Windows;
            }

            if (Path.GetFileNameWithoutExtension(action.Tool).Equals("llvm-ar", StringComparison.OrdinalIgnoreCase))
            {
                return OperatingSystem.IsWindows() ? ResponseFileQuoting.Windows : ResponseFileQuoting.Gnu;
            }
        }

        return null;
    }

    /// <summary>
    /// Windows 배치 파일 도구(<c>.cmd</c>·<c>.bat</c> — <c>--compiler</c>나 PATH의 wrapper)의 기준이다. CreateProcess가 배치 파일을
    /// <c>cmd.exe /c</c>로 실행하므로 cmd.exe의 명령줄 한계 8,191자가 적용된다. cmd.exe 경로와 <c>/c</c> 몫으로 512자를 남긴다.
    /// </summary>
    public const int WindowsBatchCommandLineLimit = 8191 - 512;

    /// <summary>
    /// 이 호스트에서 그대로는 프로세스를 시작할 수 없는(또는 기준을 넘는) 명령이면 참이다. Windows는 .NET이 만드는 명령줄이
    /// <see cref="ResponseFile.WindowsCommandLineLimit"/>(끝 NUL 포함, 배치 파일 도구는 <see cref="WindowsBatchCommandLineLimit"/>)를
    /// 넘을 때, POSIX는 인자 영역이 <see cref="PosixArgumentLimit"/>를 넘을 때다.
    /// </summary>
    public static bool ExceedsHostLimit(string tool, IReadOnlyList<string> arguments)
    {
        if (!OperatingSystem.IsWindows())
        {
            return ResponseFile.PosixArgumentBytes(tool, arguments) > PosixArgumentLimit;
        }

        int limit = IsBatchFile(tool) ? WindowsBatchCommandLineLimit : ResponseFile.WindowsCommandLineLimit;
        return ResponseFile.WindowsCommandLineLength(tool, arguments) + 1 > limit;
    }

    /// <summary>이 호스트의 기준을 진단 문구로 돌려준다(예: "limit 32767 including NUL").</summary>
    public static string LimitText(string tool) => !OperatingSystem.IsWindows()
        ? $"response file threshold {PosixArgumentLimit} argument bytes"
        : IsBatchFile(tool)
            ? $"limit {WindowsBatchCommandLineLimit} for a batch file run by cmd.exe"
            : $"limit {ResponseFile.WindowsCommandLineLimit} including NUL";

    private static bool IsBatchFile(string tool) =>
        Path.GetExtension(tool).Equals(".cmd", StringComparison.OrdinalIgnoreCase) ||
        Path.GetExtension(tool).Equals(".bat", StringComparison.OrdinalIgnoreCase);

    /// <summary>
    /// response file로 넘길 때 쓸 규칙이다. 넘기지 않으면 null이다 — 규칙을 모르는 도구(quoting이 null)는 어느 방식에서도 넘기지 않는다.
    /// </summary>
    public static ResponseFileQuoting? ResponseFileSyntax(ResponseFileMode mode, ResponseFileQuoting? quoting, string tool,
        IReadOnlyList<string> arguments)
    {
        bool use = quoting is not null && mode switch
        {
            ResponseFileMode.Always => true,
            ResponseFileMode.Never => false,
            _ => ExceedsHostLimit(tool, arguments)
        };
        return use ? quoting : null;
    }

    /// <summary>
    /// 진단에 보이는 명령줄이다(호스트 규칙으로 인용 — Windows는 .NET 명령줄과 같은 규칙, 그 밖은 GNU). response file을 쓴 action이
    /// 실패하면 <c>@파일</c> 대신 이 펼친 명령을(출력은 compile_commands.json처럼 최종 경로로) 보여 준다.
    /// </summary>
    public static string Display(string tool, IReadOnlyList<string> arguments)
    {
        ResponseFileQuoting quoting = OperatingSystem.IsWindows() ? ResponseFileQuoting.Windows : ResponseFileQuoting.Gnu;
        return string.Join(' ', new[] { tool }.Concat(arguments).Select(value => value.Length == 0 ? "\"\"" : ResponseFile.Quote(value, quoting)));
    }
}
