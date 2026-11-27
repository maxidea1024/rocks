using System.Text;

namespace Rocks.Tooling.Common;

/// <summary>
/// 컴파일러가 낸 depfile(Make 문법)을 읽는다(Docs/Design/UnityBuildPch.ko.md 4.1절). clang의 쓰기 규칙(DependencyFile.cpp의
/// PrintFilename)에 맞춘다. native 빌드(Rocks.Build Executor)와 MetaCompiler(Docs/Design/BuildReuse 2.4절)가 함께 쓴다.
/// <list type="bullet">
/// <item>줄 끝의 <c>\</c>는 줄 이음이다(<c>\r\n</c> 포함).</item>
/// <item>공백은 <c>\ </c>로 escape된다. 공백 바로 앞의 역슬래시가 2N+1개면 역슬래시 N개와 공백 하나다(clang은 경로의 공백 앞 역슬래시를
/// 두 배로 쓰고 escape 하나를 더한다). 그 밖의 역슬래시는 Windows 경로 구분자 그대로다.</item>
/// <item><c>\#</c>는 <c>#</c>, <c>$$</c>는 <c>$</c>다.</item>
/// <item>첫 토큰(<c>:</c>로 끝난다)은 target이고 나머지가 prerequisite다. Windows 드라이브 문자의 <c>:</c>는 토큰 안이라 구분자가 아니다.</item>
/// <item>상대 경로는 컴파일러의 작업 디렉터리(빌드 루트) 기준이고 절대 경로와 섞여 나온다.</item>
/// </list>
/// </summary>
public static class Depfile
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

        var seen = new HashSet<string>(OperatingSystem.IsWindows() ? StringComparer.OrdinalIgnoreCase : StringComparer.Ordinal);
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
