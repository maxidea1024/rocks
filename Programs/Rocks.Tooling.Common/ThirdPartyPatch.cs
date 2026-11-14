using System.Text;

namespace Rocks.Tooling.Common;

/// <summary>
/// 외부 라이브러리 원본에 적용한 추적 가능한 수정(git diff 형식 unified patch)이다(Docs/Design/Crypto.ko.md 5.1절,
/// 경고 정책의 "추적 가능한 수정"). 정책 검사(third-party)는 현재 파일에 patch를 거꾸로 적용해 upstream 원본을 복원하고,
/// 그 SHA-256이 manifest의 <c>modifications[].upstreamSha256</c>과 같은지 확인한다. 그래서 manifest·patch·현재 파일이
/// 서로 어긋나면 검사가 실패한다.
/// <list type="bullet">
/// <item>입력은 <c>git diff --no-index</c>가 만드는 형식이다: <c>diff --git a/P b/P</c>, 선택 <c>new file mode</c>·<c>index</c>,
/// <c>--- a/P</c> 또는 <c>--- /dev/null</c>(추가된 파일), <c>+++ b/P</c>, <c>@@ -l,s +l,s @@</c> hunk, hunk 안의
/// <c>' '</c>·<c>'-'</c>·<c>'+'</c> 줄과 <c>\ No newline at end of file</c> 표시.</item>
/// <item>경로는 원본 디렉터리 기준이며 a/·b/ 접두를 뗀 값이 같아야 한다(이름 바꾸기·삭제·binary patch는 거부).</item>
/// <item>거꾸로 적용은 정확해야 한다: hunk 위치·문맥이 한 줄이라도 다르면 <see cref="FormatException"/>이다(fuzz 없음).</item>
/// <item>줄 끝은 LF만 다룬다. CR이 섞인 원본은 patch 대상으로 쓰지 않는다(검사가 거부한다).</item>
/// </list>
/// </summary>
public static class ThirdPartyPatch
{
    public sealed record Line(char Kind, string Text);

    public sealed record Hunk(int OldStart, int OldCount, int NewStart, int NewCount, IReadOnlyList<Line> Lines,
        bool OldMissingFinalNewline, bool NewMissingFinalNewline);

    public sealed record FilePatch(string Path, bool Added, IReadOnlyList<Hunk> Hunks);

    /// <summary>patch 텍스트를 파일별로 나눈다. 같은 경로가 두 번 나오면 오류다.</summary>
    /// <exception cref="FormatException">형식이 위 계약과 다르다.</exception>
    public static IReadOnlyList<FilePatch> Parse(string text)
    {
        if (text.Contains('\r'))
        {
            throw new FormatException("patch: CR line endings are not supported");
        }

        string[] lines = text.Split('\n');
        int count = lines.Length > 0 && lines[^1].Length == 0 ? lines.Length - 1 : lines.Length;
        var files = new List<FilePatch>();
        var seen = new HashSet<string>(StringComparer.Ordinal);
        int index = 0;
        while (index < count)
        {
            string header = lines[index];
            if (!header.StartsWith("diff --git a/", StringComparison.Ordinal))
            {
                throw new FormatException($"patch line {index + 1}: expected 'diff --git a/...'");
            }

            ++index;
            bool added = false;
            while (index < count && !lines[index].StartsWith("--- ", StringComparison.Ordinal))
            {
                string meta = lines[index];
                if (meta.StartsWith("new file mode ", StringComparison.Ordinal))
                {
                    added = true;
                }
                else if (!meta.StartsWith("index ", StringComparison.Ordinal))
                {
                    // 이름 바꾸기·삭제·mode 변경·binary는 원본 추적을 흐리므로 받지 않는다.
                    throw new FormatException($"patch line {index + 1}: unsupported header '{meta}'");
                }

                ++index;
            }

            if (index + 1 >= count || !lines[index + 1].StartsWith("+++ b/", StringComparison.Ordinal))
            {
                throw new FormatException($"patch line {index + 1}: expected '---' and '+++ b/' headers");
            }

            string oldPath = lines[index]["--- ".Length..];
            string path = lines[index + 1]["+++ b/".Length..];
            if (added ? oldPath != "/dev/null" : oldPath != "a/" + path)
            {
                throw new FormatException($"patch line {index + 1}: old path '{oldPath}' does not match new path '{path}'");
            }

            if (header != $"diff --git a/{path} b/{path}")
            {
                throw new FormatException($"patch line {index + 1}: diff header does not match '{path}'");
            }

            if (!seen.Add(path))
            {
                throw new FormatException($"patch: '{path}' appears twice");
            }

            index += 2;
            var hunks = new List<Hunk>();
            while (index < count && lines[index].StartsWith("@@ ", StringComparison.Ordinal))
            {
                (Hunk hunk, int next) = ParseHunk(lines, count, index);
                hunks.Add(hunk);
                index = next;
            }

            if (hunks.Count == 0)
            {
                throw new FormatException($"patch: '{path}' has no hunk");
            }

            if (added && (hunks.Count != 1 || hunks[0].OldCount != 0 || hunks[0].Lines.Any(line => line.Kind != '+')))
            {
                throw new FormatException($"patch: added file '{path}' must be a single all-'+' hunk");
            }

            files.Add(new(path, added, hunks));
        }

        return files;
    }

    /// <summary>
    /// 현재 파일 내용에 patch를 거꾸로 적용해 upstream 원본 내용을 돌려준다. 추가된 파일이면 현재 내용이 patch와 정확히
    /// 같은지 확인하고 null(원본에 없음)을 돌려준다.
    /// </summary>
    /// <exception cref="FormatException">문맥·위치·줄 수·파일 끝 줄바꿈이 patch와 다르다.</exception>
    public static byte[]? Reverse(FilePatch patch, byte[] current)
    {
        string text = new UTF8Encoding(false, true).GetString(current);
        if (text.Contains('\r'))
        {
            throw new FormatException($"{patch.Path}: CR line endings are not supported in patched files");
        }

        bool currentFinalNewline = text.EndsWith('\n');
        string[] currentLines = SplitLines(text);
        var original = new List<string>();
        int cursor = 0; // currentLines에서 다음에 볼 줄(0부터)
        bool originalMissingFinalNewline = false;
        Hunk? last = null;
        foreach (Hunk hunk in patch.Hunks)
        {
            int start = hunk.NewCount == 0 ? hunk.NewStart : hunk.NewStart - 1;
            if (start < cursor || start > currentLines.Length)
            {
                throw new FormatException($"{patch.Path}: hunk @@ +{hunk.NewStart} is out of order or beyond the file");
            }

            original.AddRange(currentLines[cursor..start]);
            cursor = start;
            foreach (Line line in hunk.Lines)
            {
                if (line.Kind is ' ' or '+')
                {
                    if (cursor >= currentLines.Length || currentLines[cursor] != line.Text)
                    {
                        throw new FormatException($"{patch.Path}: current line {cursor + 1} does not match the patch");
                    }

                    ++cursor;
                }

                if (line.Kind is ' ' or '-')
                {
                    original.Add(line.Text);
                }
            }

            originalMissingFinalNewline = hunk.OldMissingFinalNewline;
            last = hunk;
        }

        original.AddRange(currentLines[cursor..]);
        bool hunkReachesEnd = cursor == currentLines.Length && last is not null;
        bool expectedCurrentFinalNewline = !(hunkReachesEnd && last!.NewMissingFinalNewline);
        if (currentLines.Length > 0 && currentFinalNewline != expectedCurrentFinalNewline)
        {
            throw new FormatException($"{patch.Path}: final newline of the current file does not match the patch");
        }

        if (patch.Added)
        {
            return null;
        }

        bool originalFinalNewline = hunkReachesEnd ? !originalMissingFinalNewline : currentFinalNewline;
        string restored = string.Join('\n', original) + (original.Count > 0 && originalFinalNewline ? "\n" : "");
        return Encoding.UTF8.GetBytes(restored);
    }

    private static string[] SplitLines(string text)
    {
        if (text.Length == 0)
        {
            return [];
        }

        string body = text.EndsWith('\n') ? text[..^1] : text;
        return body.Split('\n');
    }

    private static (Hunk Hunk, int Next) ParseHunk(string[] lines, int count, int index)
    {
        string header = lines[index];
        int close = header.IndexOf(" @@", 3, StringComparison.Ordinal);
        string[] ranges = close < 0 ? [] : header[3..close].Split(' ');
        if (ranges.Length != 2 || !ranges[0].StartsWith('-') || !ranges[1].StartsWith('+'))
        {
            throw new FormatException($"patch line {index + 1}: malformed hunk header '{header}'");
        }

        (int oldStart, int oldCount) = Range(ranges[0][1..], index);
        (int newStart, int newCount) = Range(ranges[1][1..], index);
        var body = new List<Line>();
        int oldSeen = 0;
        int newSeen = 0;
        bool oldMissing = false;
        bool newMissing = false;
        ++index;
        while (index < count && (oldSeen < oldCount || newSeen < newCount || lines[index].StartsWith('\\')))
        {
            string line = lines[index];
            if (line.StartsWith('\\'))
            {
                if (line != "\\ No newline at end of file" || body.Count == 0)
                {
                    throw new FormatException($"patch line {index + 1}: unexpected marker '{line}'");
                }

                char previous = body[^1].Kind;
                oldMissing |= previous is ' ' or '-';
                newMissing |= previous is ' ' or '+';
                ++index;
                continue;
            }

            char kind = line.Length == 0 ? ' ' : line[0];
            if (kind is not (' ' or '-' or '+'))
            {
                throw new FormatException($"patch line {index + 1}: unexpected hunk line '{line}'");
            }

            body.Add(new(kind, line.Length == 0 ? "" : line[1..]));
            oldSeen += kind is ' ' or '-' ? 1 : 0;
            newSeen += kind is ' ' or '+' ? 1 : 0;
            ++index;
        }

        if (oldSeen != oldCount || newSeen != newCount)
        {
            throw new FormatException($"patch: hunk '{header}' line counts do not match its body");
        }

        return (new(oldStart, oldCount, newStart, newCount, body, oldMissing, newMissing), index);
    }

    private static (int Start, int Count) Range(string text, int index)
    {
        string[] parts = text.Split(',');
        if (parts.Length is < 1 or > 2 || !int.TryParse(parts[0], out int start) || start < 0 ||
            (parts.Length == 2 && (!int.TryParse(parts[1], out int parsed) || parsed < 0)))
        {
            throw new FormatException($"patch line {index + 1}: malformed hunk range '{text}'");
        }

        return (start, parts.Length == 2 ? int.Parse(parts[1]) : 1);
    }
}
