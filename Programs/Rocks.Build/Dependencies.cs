using System.Text;

namespace Rocks.Build;

/// <summary>
/// 컴파일러가 낸 depfile(Make 문법)을 읽는다(Docs/Design/UnityBuildPch.ko.md 4.1절). clang의 쓰기 규칙(DependencyFile.cpp의
/// PrintFilename)에 맞춘다.
/// <list type="bullet">
/// <item>줄 끝의 <c>\</c>는 줄 이음이다(<c>\r\n</c> 포함).</item>
/// <item>공백은 <c>\ </c>로 escape된다. 공백 바로 앞의 역슬래시가 2N+1개면 역슬래시 N개와 공백 하나다(clang은 경로의 공백 앞 역슬래시를
/// 두 배로 쓰고 escape 하나를 더한다). 그 밖의 역슬래시는 Windows 경로 구분자 그대로다.</item>
/// <item><c>\#</c>는 <c>#</c>, <c>$$</c>는 <c>$</c>다.</item>
/// <item>첫 토큰(<c>:</c>로 끝난다)은 target이고 나머지가 prerequisite다. Windows 드라이브 문자의 <c>:</c>는 토큰 안이라 구분자가 아니다.</item>
/// <item>상대 경로는 컴파일러의 작업 디렉터리(빌드 루트) 기준이고 절대 경로와 섞여 나온다.</item>
/// </list>
/// </summary>
internal static class Depfile
{
    /// <summary>
    /// prerequisite 경로를 정규 절대 경로로, 처음 나온 순서대로 중복 없이 돌려준다. target이 없거나 문법이 깨졌으면
    /// <see cref="InvalidDataException"/>이다.
    /// </summary>
    public static IReadOnlyList<string> Parse(string text, string workingDirectory)
    {
        List<string> tokens = Tokens(text);
        if (tokens.Count == 0)
        {
            throw new InvalidDataException("The dependency file has no target.");
        }

        // target과 ':'가 붙어 있거나(clang) 떨어져 있다("target :").
        int first;
        if (tokens[0].Length > 1 && tokens[0].EndsWith(':'))
        {
            first = 1;
        }
        else if (tokens.Count > 1 && tokens[1] == ":")
        {
            first = 2;
        }
        else
        {
            throw new InvalidDataException("The dependency file does not start with 'target:'.");
        }

        var seen = new HashSet<string>(NativeToolchains.PathComparer);
        var result = new List<string>();
        for (int index = first; index < tokens.Count; ++index)
        {
            string full = Path.GetFullPath(tokens[index], workingDirectory);
            if (seen.Add(full))
            {
                result.Add(full);
            }
        }

        return result;
    }

    private static List<string> Tokens(string text)
    {
        var tokens = new List<string>();
        var current = new StringBuilder();
        void Flush()
        {
            if (current.Length != 0)
            {
                tokens.Add(current.ToString());
                current.Clear();
            }
        }

        int index = 0;
        while (index < text.Length)
        {
            char value = text[index];
            if (value == '\\')
            {
                int run = 0;
                while (index + run < text.Length && text[index + run] == '\\')
                {
                    ++run;
                }

                char next = index + run < text.Length ? text[index + run] : '\0';
                if (next == '\n' || (next == '\r' && index + run + 1 < text.Length && text[index + run + 1] == '\n'))
                {
                    // 줄 이음: 마지막 역슬래시 하나가 줄 이음이고 그 앞의 역슬래시는 글자다(경로가 역슬래시로 끝나는 일은 없다).
                    current.Append('\\', run - 1);
                    Flush();
                    index += run + (next == '\r' ? 2 : 1);
                }
                else if (next == ' ' && run % 2 == 1)
                {
                    current.Append('\\', run / 2);
                    current.Append(' ');
                    index += run + 1;
                }
                else if (next == '#')
                {
                    current.Append('\\', run - 1);
                    current.Append('#');
                    index += run + 1;
                }
                else
                {
                    current.Append('\\', run);
                    index += run;
                }
            }
            else if (value == '$' && index + 1 < text.Length && text[index + 1] == '$')
            {
                current.Append('$');
                index += 2;
            }
            else if (value is ' ' or '\t' or '\r' or '\n')
            {
                Flush();
                ++index;
            }
            else
            {
                current.Append(value);
                ++index;
            }
        }

        Flush();
        return tokens;
    }
}

/// <summary>의존 기록의 항목 하나다: 컴파일러가 연 파일의 정규 절대 경로와 그 내용의 SHA-256(소문자 hex)이다.</summary>
internal sealed record DependencyEntry(string Path, string Hash);

/// <summary>
/// compile action의 의존 기록 파일이다(Docs/Design/UnityBuildPch.ko.md 4.1절 2단 key의 둘째 단). 지난 실행에서 컴파일러가 실제로 연
/// 파일(툴체인 소유 헤더 제외)과 각 내용 해시를 담는다. action의 출력으로 object와 함께 게시되고(실패·취소한 실행은 남기지 않는다),
/// 다음 실행은 선언 입력 key가 같을 때 이 기록의 모든 해시를 현재 파일과 대조한다. 형식은 머리말 한 줄과 항목마다
/// <c>&lt;해시&gt; &lt;경로&gt;</c> 한 줄(경로 ordinal 순, UTF-8, LF)이다. 경로는 이 호스트의 절대 경로다(증분 key와 같이 호스트 전용이다).
/// </summary>
internal static class DependencyRecord
{
    public const string Extension = ".deps";

    private const string Header = "rocks-dependency-record 1";

    // SHA-256 소문자 hex의 글자 수다.
    private const int HashLength = 64;

    public static byte[] Encode(IEnumerable<DependencyEntry> entries)
    {
        var text = new StringBuilder(Header).Append('\n');
        foreach (DependencyEntry entry in entries.OrderBy(entry => entry.Path, StringComparer.Ordinal))
        {
            if (entry.Path.IndexOfAny(['\r', '\n']) >= 0)
            {
                throw new InvalidDataException("A dependency path contains a line break: " + entry.Path);
            }

            text.Append(entry.Hash).Append(' ').Append(entry.Path).Append('\n');
        }

        return new UTF8Encoding(false).GetBytes(text.ToString());
    }

    /// <summary>기록을 읽는다. 파일이 없거나 형식이 깨졌으면 null이다(호출자는 다시 실행한다).</summary>
    public static IReadOnlyList<DependencyEntry>? Read(string path)
    {
        string text;
        try
        {
            text = File.ReadAllText(path, new UTF8Encoding(false, true));
        }
        catch (Exception error) when (error is IOException or UnauthorizedAccessException or DecoderFallbackException)
        {
            return null;
        }

        string[] lines = text.Split('\n');
        if (lines.Length < 2 || lines[0] != Header || lines[^1].Length != 0)
        {
            return null;
        }

        var entries = new List<DependencyEntry>(lines.Length - 2);
        for (int index = 1; index < lines.Length - 1; ++index)
        {
            string line = lines[index];
            if (line.Length <= HashLength + 1 || line[HashLength] != ' ' || !IsHash(line.AsSpan(0, HashLength)))
            {
                return null;
            }

            entries.Add(new(line[(HashLength + 1)..], line[..HashLength]));
        }

        return entries;
    }

    private static bool IsHash(ReadOnlySpan<char> text)
    {
        foreach (char value in text)
        {
            if (!char.IsAsciiDigit(value) && value is not (>= 'a' and <= 'f'))
            {
                return false;
            }
        }

        return true;
    }
}
