using System.Text;
using Rocks.Tooling.Common;

namespace Rocks.Build;

/// <summary>도구가 쓰는 의존 파일의 문법이다. Make는 clang depfile(<see cref="Depfile"/>), Lines는 ISPC <c>-MMM</c>(<see cref="IspcTools.ParseDependencies"/>)다.</summary>
internal enum DependencyFileFormat { Make, Lines }

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
