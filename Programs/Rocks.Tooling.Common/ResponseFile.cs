using System.Text;

namespace Rocks.Tooling.Common;

/// <summary>response file(<c>@파일</c>)을 읽는 도구의 인자 분리 규칙이다. 같은 인자라도 규칙에 따라 다르게 써야 한다.</summary>
public enum ResponseFileQuoting
{
    /// <summary>
    /// Windows 규칙(CommandLineToArgvW, LLVM <c>TokenizeWindowsCommandLine</c>)이다. 역슬래시는 큰따옴표 앞에서만 특별하고 작은따옴표는
    /// 글자다. clang-cl·llvm-lib·lib.exe가 쓰고, llvm-ar도 Windows 호스트에서는 이 규칙이 기본이다(실측).
    /// </summary>
    Windows,

    /// <summary>
    /// GNU 규칙(LLVM <c>TokenizeGNUCommandLine</c>, GCC libiberty)이다. 역슬래시는 다음 글자를 escape하고 작은따옴표도 묶는다.
    /// clang-cl이 아닌 clang 드라이버(clang++, NDK clang++)는 Windows 호스트에서도 이 규칙이다(실측). macOS의 llvm-ar도 이 규칙이다.
    /// </summary>
    Gnu
}

/// <summary>
/// response file의 내용과 명령줄 길이를 결정적으로 만든다. 같은 인자 목록과 규칙이면 byte 단위로 같은 파일이 나온다(파일 경로·시각을
/// 담지 않는다). 파일을 어디에 두고 언제 지우는지는 호출자가 정한다.
/// <para>
/// 예: <c>File.WriteAllBytes(path, ResponseFile.Encode(arguments, ResponseFileQuoting.Windows))</c>로 쓰고 도구를
/// <c>[ "@" + path ]</c> 인자 하나로 실행한다.
/// </para>
/// </summary>
public static class ResponseFile
{
    /// <summary>
    /// Windows CreateProcessW의 명령줄 한계(끝 NUL 포함 UTF-16 32,767 단위)다. 프로그램 경로를 포함한 전체 명령줄
    /// (<see cref="WindowsCommandLineLength"/>)이 이 값보다 짧아야 프로세스를 시작할 수 있다.
    /// </summary>
    public const int WindowsCommandLineLimit = 32767;

    /// <summary>
    /// 인자 하나를 규칙에 맞게 인용한다. 공백·인용 부호가 없는 인자는 그대로 둔다.
    /// Windows: 공백·큰따옴표가 있거나 비었으면 큰따옴표로 감싸고, 큰따옴표 앞과 끝의 역슬래시를 두 배로 하고 큰따옴표는 <c>\"</c>로 쓴다.
    /// Gnu: 공백·역슬래시·큰따옴표·작은따옴표가 있으면 큰따옴표로 감싸고 역슬래시와 큰따옴표 앞에 역슬래시를 붙인다.
    /// argument에 NUL이 있거나(프로세스 인자로 넘길 수 없다) Gnu에서 비었으면(LLVM GNU 토크나이저는 빈 토큰을 버려 인자가 사라진다)
    /// ArgumentException이다.
    /// </summary>
    public static string Quote(string argument, ResponseFileQuoting quoting)
    {
        ArgumentNullException.ThrowIfNull(argument);
        if (argument.Contains('\0'))
        {
            throw new ArgumentException("Response file arguments cannot contain NUL characters.", nameof(argument));
        }

        if (argument.Length == 0 && quoting == ResponseFileQuoting.Gnu)
        {
            throw new ArgumentException("A GNU-style response file cannot carry an empty argument (LLVM drops empty tokens).", nameof(argument));
        }

        return quoting switch
        {
            ResponseFileQuoting.Windows => QuoteWindows(argument),
            ResponseFileQuoting.Gnu => QuoteGnu(argument),
            _ => throw new ArgumentOutOfRangeException(nameof(quoting), quoting, "Unknown response file quoting.")
        };
    }

    /// <summary>
    /// response file 본문이다. Windows는 인자를 공백 하나로 이은 한 줄(줄바꿈 없음)이다 — clang-cl은 response file의 줄 끝에서
    /// <c>/link</c> 뒤 인자 묶음을 끊으므로 여러 줄이면 <c>/link</c> 뒤 인자가 컴파일러 입력이 된다(실측). Gnu는 인자마다 한 줄(LF)이다.
    /// 옮길 수 없는 인자(<see cref="Quote"/> 참고 — NUL, Gnu의 빈 인자)가 있으면 ArgumentException이다.
    /// </summary>
    public static string Format(IReadOnlyList<string> arguments, ResponseFileQuoting quoting)
    {
        ArgumentNullException.ThrowIfNull(arguments);
        var text = new StringBuilder();
        for (int index = 0; index < arguments.Count; ++index)
        {
            if (quoting == ResponseFileQuoting.Windows && index != 0)
            {
                text.Append(' ');
            }

            text.Append(Quote(arguments[index], quoting));
            if (quoting == ResponseFileQuoting.Gnu)
            {
                text.Append('\n');
            }
        }

        return text.ToString();
    }

    /// <summary>
    /// 파일에 쓸 byte다. Windows는 BOM 있는 UTF-16LE(LLVM 도구는 UTF-8로 바꿔 읽고, MSVC lib.exe도 읽는 유일한 비ASCII 형식),
    /// Gnu는 BOM 없는 UTF-8(LLVM 도구가 읽는 형식)이다. 비ASCII 경로(한글 등)도 그대로 전달된다. 예외는 <see cref="Format"/>과 같다.
    /// </summary>
    public static byte[] Encode(IReadOnlyList<string> arguments, ResponseFileQuoting quoting)
    {
        string text = Format(arguments, quoting);
        return quoting == ResponseFileQuoting.Windows
            ? [.. Encoding.Unicode.GetPreamble(), .. Encoding.Unicode.GetBytes(text)]
            : new UTF8Encoding(false).GetBytes(text);
    }

    /// <summary>
    /// .NET <c>Process.Start</c>가 Windows에서 만드는 명령줄의 UTF-16 길이(끝 NUL 제외)다: 큰따옴표로 감싼 프로그램 경로, 그다음
    /// 인자마다 공백 하나와 <see cref="ResponseFileQuoting.Windows"/> 인용 결과. <see cref="WindowsCommandLineLimit"/>와 비교한다.
    /// </summary>
    public static int WindowsCommandLineLength(string fileName, IReadOnlyList<string> arguments)
    {
        ArgumentNullException.ThrowIfNull(fileName);
        ArgumentNullException.ThrowIfNull(arguments);
        // .NET은 앞뒤 공백을 뗀 경로가 큰따옴표로 시작하고 끝나지 않으면 큰따옴표로 감싼다.
        string trimmed = fileName.Trim();
        int length = trimmed.Length + (trimmed.StartsWith('"') && trimmed.EndsWith('"') ? 0 : 2);
        foreach (string argument in arguments)
        {
            length += 1 + QuoteWindows(argument).Length;
        }

        return length;
    }

    /// <summary>
    /// POSIX exec의 인자 영역 크기(byte)다: 프로그램 경로와 각 인자의 UTF-8 길이에 끝 NUL 하나씩을 더한다. 환경 변수 영역은 넣지 않는다.
    /// </summary>
    public static long PosixArgumentBytes(string fileName, IReadOnlyList<string> arguments)
    {
        ArgumentNullException.ThrowIfNull(fileName);
        ArgumentNullException.ThrowIfNull(arguments);
        long length = Encoding.UTF8.GetByteCount(fileName) + 1;
        foreach (string argument in arguments)
        {
            length += Encoding.UTF8.GetByteCount(argument) + 1;
        }

        return length;
    }

    // .NET PasteArguments(Windows)와 같은 규칙이다 — 그래서 WindowsCommandLineLength가 실제 명령줄 길이와 같다.
    private static string QuoteWindows(string argument)
    {
        if (argument.Length != 0 && !argument.Any(value => char.IsWhiteSpace(value) || value == '"'))
        {
            return argument;
        }

        var text = new StringBuilder(argument.Length + 2);
        text.Append('"');
        int index = 0;
        while (index < argument.Length)
        {
            char value = argument[index++];
            if (value == '\\')
            {
                int backslashes = 1;
                while (index < argument.Length && argument[index] == '\\')
                {
                    ++index;
                    ++backslashes;
                }

                if (index == argument.Length)
                {
                    // 닫는 큰따옴표 앞이므로 두 배로 쓴다.
                    text.Append('\\', backslashes * 2);
                }
                else if (argument[index] == '"')
                {
                    text.Append('\\', backslashes * 2 + 1).Append('"');
                    ++index;
                }
                else
                {
                    text.Append('\\', backslashes);
                }

                continue;
            }

            if (value == '"')
            {
                text.Append('\\').Append('"');
                continue;
            }

            text.Append(value);
        }

        text.Append('"');
        return text.ToString();
    }

    private static string QuoteGnu(string argument)
    {
        if (argument.Length != 0 && !argument.Any(value => char.IsWhiteSpace(value) || value is '\\' or '"' or '\''))
        {
            return argument;
        }

        var text = new StringBuilder(argument.Length + 2);
        text.Append('"');
        foreach (char value in argument)
        {
            if (value is '\\' or '"')
            {
                text.Append('\\');
            }

            text.Append(value);
        }

        text.Append('"');
        return text.ToString();
    }
}
