using System.Text;

namespace Rocks.Tooling.Common;

/// <summary>
/// C++ 소스 텍스트의 어휘 전처리다. 리플렉션 헤더 탐색(Rocks.Build <c>ReflectionHeaderScan</c>)과 메타 컴파일러의 조건 블록 스캔(Docs/Design/
/// BuildTargets 3.4절 BS2c)이 같은 규칙으로 주석·리터럴을 지운다. 사용 예:
/// <code>
/// string code = CppSourceText.WithoutCommentsAndLiterals(File.ReadAllText(header));   // 줄 번호는 그대로다
/// </code>
/// </summary>
public static class CppSourceText
{
    // 주석과 문자열·문자 리터럴의 내용을 공백으로 바꾼 텍스트다(줄바꿈과 따옴표는 남긴다). 리터럴 안의 주석 기호는 주석이 아니고, 리터럴 안
    // 역슬래시 줄 이음 뒤의 철자와 raw string(R"구분자( … )구분자")의 여러 줄 내용도 지운다. // 주석 끝의 역슬래시는 다음 줄도 주석으로
    // 잇는다. 숫자 뒤의 '는 자릿수 구분자(1'000)라 문자 리터럴이 아니다.
    public static string WithoutCommentsAndLiterals(string text)
    {
        var output = new StringBuilder(text.Length);
        int index = 0;
        while (index < text.Length)
        {
            char current = text[index];
            char next = index + 1 < text.Length ? text[index + 1] : '\0';
            if (current == '/' && next == '/')
            {
                // 줄 끝의 역슬래시는 주석을 다음 줄로 잇는다.
                while (index < text.Length)
                {
                    if (text[index] == '\n')
                    {
                        int end = index - 1;
                        while (end >= 0 && text[end] == '\r')
                        {
                            --end;
                        }

                        if (end < 0 || text[end] != '\\')
                        {
                            break;
                        }

                        output.Append('\n');
                        ++index;
                        continue;
                    }

                    output.Append(' ');
                    ++index;
                }
            }
            else if (current == '/' && next == '*')
            {
                output.Append("  ");
                index += 2;
                while (index < text.Length && !(text[index] == '*' && index + 1 < text.Length && text[index + 1] == '/'))
                {
                    output.Append(Blank(text[index]));
                    ++index;
                }

                if (index < text.Length)
                {
                    output.Append("  ");
                    index += 2;
                }
            }
            else if (current == 'R' && next == '"' && (index == 0 || !IsIdentifierChar(text[index - 1]) || IsRawPrefix(text, index)))
            {
                // raw string: R"구분자( … )구분자". 구분자를 읽고 닫는 )구분자"까지 지운다.
                int open = text.IndexOf('(', index + 2);
                if (open < 0)
                {
                    output.Append(current);
                    ++index;
                    continue;
                }

                string close = ")" + text[(index + 2)..open] + "\"";
                int end = text.IndexOf(close, open + 1, StringComparison.Ordinal);
                int stop = end < 0 ? text.Length : end + close.Length;
                output.Append("R\"");
                for (int blank = index + 2; blank < stop; ++blank)
                {
                    output.Append(Blank(text[blank]));
                }

                index = stop;
            }
            else if (current == '"' || (current == '\'' && (index == 0 || !char.IsAsciiDigit(text[index - 1]))))
            {
                output.Append(current);
                ++index;
                while (index < text.Length && text[index] != current && text[index] != '\n')
                {
                    if (text[index] == '\\' && index + 1 < text.Length)
                    {
                        output.Append(' ');
                        ++index;
                    }

                    output.Append(Blank(text[index]));
                    ++index;
                }

                if (index < text.Length && text[index] == current)
                {
                    output.Append(current);
                    ++index;
                }
            }
            else
            {
                output.Append(current);
                ++index;
            }
        }

        return output.ToString();
    }

    // 지운 자리의 문자다(줄바꿈은 남겨 줄 번호를 지킨다).
    private static char Blank(char value) => value == '\n' ? '\n' : ' ';

    private static bool IsIdentifierChar(char value) => char.IsAsciiLetterOrDigit(value) || value == '_';

    // R 앞이 raw string 접두(u8R·uR·UR·LR)인가다.
    private static bool IsRawPrefix(string text, int index)
    {
        int start = index;
        while (start > 0 && IsIdentifierChar(text[start - 1]))
        {
            --start;
        }

        string prefix = text[start..index];
        return prefix is "u8" or "u" or "U" or "L";
    }
}
