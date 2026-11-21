namespace Rocks.Build;

/// <summary>
/// 논리 compile 명령의 object 경로 인자다(Docs/Design/ProjectFiles.ko.md 7.4·9절). clang-cl은 <c>/Fo&lt;경로&gt;</c> 한 인자, clang 드라이버(macOS·
/// Android)는 <c>-o</c>와 다음 인자 <c>&lt;경로&gt;</c> 두 인자다. object 경로는 유니티·PCH 변형 디렉터리와 파일 하나 컴파일에 따라 달라지므로
/// 낡음 지문·명령 비교는 빼고, 파일 하나 컴파일은 바꾼다. 둘이 같은 규칙을 쓰게 한 곳에 둔다.
/// </summary>
public static class CompileObjectArguments
{
    private const string WindowsPrefix = "/Fo";

    private const string PosixFlag = "-o";

    /// <summary>object 경로 인자(<c>/Fo…</c>, <c>-o</c>와 다음 인자)를 뺀 인자다.</summary>
    public static IEnumerable<string> Without(IReadOnlyList<string> arguments)
    {
        for (int index = 0; index < arguments.Count; ++index)
        {
            if (arguments[index].StartsWith(WindowsPrefix, StringComparison.Ordinal))
            {
                continue;
            }

            if (arguments[index] == PosixFlag)
            {
                ++index;
                continue;
            }

            yield return arguments[index];
        }
    }

    /// <summary>object 경로만 <paramref name="objectPath"/>로 바꾼 인자다. object 경로 인자가 없으면 <see cref="InvalidOperationException"/>이다.</summary>
    public static string[] With(IReadOnlyList<string> arguments, string objectPath)
    {
        var result = new List<string>(arguments.Count);
        bool replaced = false;
        for (int index = 0; index < arguments.Count; ++index)
        {
            if (arguments[index].StartsWith(WindowsPrefix, StringComparison.Ordinal))
            {
                result.Add(WindowsPrefix + objectPath);
                replaced = true;
            }
            else if (arguments[index] == PosixFlag && index + 1 < arguments.Count)
            {
                result.Add(PosixFlag);
                result.Add(objectPath);
                ++index;
                replaced = true;
            }
            else
            {
                result.Add(arguments[index]);
            }
        }

        return replaced ? [.. result] : throw new InvalidOperationException("The logical compile command names no object path (/Fo or -o).");
    }
}
