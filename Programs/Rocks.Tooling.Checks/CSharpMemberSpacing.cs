using System.Text;
using Microsoft.CodeAnalysis;
using Microsoft.CodeAnalysis.CSharp;
using Microsoft.CodeAnalysis.CSharp.Syntax;
using Microsoft.CodeAnalysis.Text;

namespace Rocks.Tooling.Checks;

/// <summary>
/// C# 선언 사이 빈 줄 검사다(Docs/CodingStyle.md "C# 멤버 사이 빈 줄 규칙" — 규칙 <c>member-blank-line</c>). dotnet format에 해당
/// 규칙이 없어 Roslyn 구문 트리만으로 본다(컴파일·의미 분석 없음). 선언 목록(형식 본문의 멤버, namespace 본문과 컴파일 단위의
/// 선언)에서 앞 선언의 마지막 줄 바로 다음 줄이 빈 줄이어야 한다. 연속 필드·열거형 멤버·첫 선언·사이에 전처리 지시문이 있는
/// 경우는 보지 않는다.
/// </summary>
public static class CSharpMemberSpacing
{
    /// <summary>위반 규칙 ID다.</summary>
    public const string Rule = "member-blank-line";

    // 파싱 언어 버전이다(Programs/Directory.Build.props의 LangVersion 14.0과 같다).
    private static readonly CSharpParseOptions ParseOptions = new(LanguageVersion.CSharp14);

    /// <summary>한 파일 텍스트의 검사 결과다.</summary>
    /// <param name="Lines">위반 행(1부터 — 뒤 선언의 첫 줄)이다.</param>
    /// <param name="ParseErrorLine">구문 오류가 있으면 첫 오류의 행(1부터), 없으면 0이다(오류가 있으면 Lines는 비어 있다).</param>
    /// <param name="Fixed">위반마다 빈 줄 하나를 넣은 텍스트다(구문 오류면 원문 그대로).</param>
    public sealed record Result(IReadOnlyList<int> Lines, int ParseErrorLine, string Fixed);

    /// <summary>
    /// 텍스트 하나를 검사한다. 넣는 줄 끝은 텍스트의 첫 줄 끝(CRLF면 CRLF, 아니면 LF — 줄 끝이 섞인 파일도 첫 줄 끝)을 따른다.
    /// 전처리 기호 없이 파싱하므로 실제 빌드에서 켜지는 `#if` 쪽도 disabled text로 보고 보지 않는다(한계).
    /// </summary>
    public static Result Inspect(string text)
    {
        SyntaxTree tree = CSharpSyntaxTree.ParseText(text, ParseOptions);
        Diagnostic? error = tree.GetDiagnostics().FirstOrDefault(diagnostic => diagnostic.Severity == DiagnosticSeverity.Error);
        if (error is not null)
        {
            return new([], error.Location.GetLineSpan().StartLinePosition.Line + 1, text);
        }

        SyntaxNode root = tree.GetRoot();
        SourceText source = tree.GetText();
        string newline = FirstLineEnding(text);
        // 바꿀 범위와 넣을 텍스트다. 보통은 뒤 선언 leading trivia 맨 앞(들여쓰기 앞)에 줄 끝 하나를 넣는다. 앞 선언이 같은 줄에서
        // 끝나면 앞 선언 뒤 공백을 지우고 줄 끝 둘과 앞 선언 첫 줄의 들여쓰기를 넣는다(줄을 나누고 빈 줄을 만든다).
        List<(int Start, int Length, string Text, int Line)> edits = [];
        foreach (SyntaxList<MemberDeclarationSyntax> members in DeclarationLists(root))
        {
            for (int index = 1; index < members.Count; index++)
            {
                MemberDeclarationSyntax previous = members[index - 1];
                MemberDeclarationSyntax next = members[index];
                int newlines = MissingNewlines(previous, next);
                int line = tree.GetLineSpan(new(next.FullSpan.Start, 0)).StartLinePosition.Line + 1;
                if (newlines == 1)
                {
                    edits.Add((next.FullSpan.Start, 0, newline, line));
                }
                else if (newlines == 2)
                {
                    int start = previous.Span.End;
                    TextLine first = source.Lines.GetLineFromPosition(previous.SpanStart);
                    string indent = Indentation(source.ToString(first.Span));
                    edits.Add((start, next.FullSpan.Start - start, newline + newline + indent, line));
                }
            }
        }

        edits.Sort((left, right) => left.Start.CompareTo(right.Start));
        StringBuilder fixedText = new(text);
        for (int index = edits.Count - 1; index >= 0; index--)
        {
            fixedText.Remove(edits[index].Start, edits[index].Length);
            fixedText.Insert(edits[index].Start, edits[index].Text);
        }

        return new(edits.Select(edit => edit.Line).ToArray(), 0, fixedText.ToString());
    }

    /// <summary>
    /// 파일들(저장소 루트 기준 상대 경로가 아니어도 된다 — 보고는 root 상대)을 검사한다. apply=true이면 위반을 고쳐 다시 쓰고(인코딩은
    /// UTF-8, BOM은 원래대로) 구문 오류만 돌려준다. apply=false이면 파일을 바꾸지 않고 위반과 구문 오류를 돌려준다.
    /// </summary>
    public static IReadOnlyList<CheckFailure> Check(string root, IEnumerable<string> files, bool apply)
    {
        List<CheckFailure> failures = [];
        foreach (string file in files.Order(StringComparer.Ordinal))
        {
            byte[] bytes = File.ReadAllBytes(file);
            bool bom = bytes.Length >= 3 && bytes[0] == 0xEF && bytes[1] == 0xBB && bytes[2] == 0xBF;
            string relative = Path.GetRelativePath(root, file).Replace('\\', '/');
            // 잘못된 byte를 U+FFFD로 바꿔 조용히 다시 쓰지 않게 엄격히 읽는다(UTF-8이 아니면 보고만 한다).
            string text;
            try
            {
                text = new UTF8Encoding(false, throwOnInvalidBytes: true).GetString(bytes, bom ? 3 : 0,
                    bytes.Length - (bom ? 3 : 0));
            }
            catch (DecoderFallbackException)
            {
                failures.Add(new(relative, 0, Rule, "not UTF-8"));
                continue;
            }

            Result result = Inspect(text);
            if (result.ParseErrorLine != 0)
            {
                failures.Add(new(relative, result.ParseErrorLine, Rule, "parse error"));
                continue;
            }

            if (result.Lines.Count == 0)
            {
                continue;
            }

            if (apply)
            {
                File.WriteAllText(file, result.Fixed, new UTF8Encoding(bom));
                continue;
            }

            failures.AddRange(result.Lines.Select(line => new CheckFailure(relative, line, Rule,
                "a blank line is required between this declaration and the previous one")));
        }

        return failures;
    }

    // 텍스트의 첫 줄 끝이다(줄 끝이 없으면 LF).
    private static string FirstLineEnding(string text)
    {
        int index = text.IndexOf('\n');
        return index > 0 && text[index - 1] == '\r' ? "\r\n" : "\n";
    }

    // 줄 앞의 들여쓰기(공백·탭)다.
    private static string Indentation(string line)
    {
        int length = 0;
        while (length < line.Length && (line[length] == ' ' || line[length] == '\t'))
        {
            length++;
        }

        return line[..length];
    }

    // 검사할 선언 목록이다: 컴파일 단위·namespace 본문(file-scoped 포함)·형식 본문(class·struct·record·interface). 열거형은
    // EnumDeclarationSyntax라 TypeDeclarationSyntax가 아니어서 빠진다.
    private static IEnumerable<SyntaxList<MemberDeclarationSyntax>> DeclarationLists(SyntaxNode root)
    {
        foreach (SyntaxNode node in root.DescendantNodesAndSelf())
        {
            switch (node)
            {
                case CompilationUnitSyntax unit:
                    yield return unit.Members;
                    break;
                case BaseNamespaceDeclarationSyntax space:
                    yield return space.Members;
                    break;
                case TypeDeclarationSyntax type:
                    yield return type.Members;
                    break;
            }
        }
    }

    // next가 previous 바로 뒤에서 빈 줄 없이 시작하면 고치는 데 넣을 줄 끝 수(1 또는 2), 아니면(예외 포함) 0이다.
    private static int MissingNewlines(MemberDeclarationSyntax previous, MemberDeclarationSyntax next)
    {
        // 필드·이벤트 필드 무리는 빈 줄 없이 묶어도 된다.
        if (previous is BaseFieldDeclarationSyntax && next is BaseFieldDeclarationSyntax)
        {
            return 0;
        }

        // 최상위 문장(top-level statements)은 선언이 아니라 문장이다(블록 뒤 빈 줄은 IDE2003 몫).
        if (previous is GlobalStatementSyntax || next is GlobalStatementSyntax)
        {
            return 0;
        }

        SyntaxTriviaList leading = next.GetLeadingTrivia();
        // 사이에 전처리 지시문(꺼진 #if 쪽의 disabled text 포함)이 있으면 지시문의 위치가 뜻을 가지므로 보지 않는다.
        if (leading.Any(trivia => trivia.IsDirective || trivia.IsKind(SyntaxKind.DisabledTextTrivia)))
        {
            return 0;
        }

        // 앞 선언이 같은 줄에서 끝나면(줄 끝이 없음) 줄을 나누고 빈 줄을 만든다.
        if (!previous.GetTrailingTrivia().Any(trivia => trivia.IsKind(SyntaxKind.EndOfLineTrivia)))
        {
            return 2;
        }

        // 앞 선언의 마지막 줄 바로 다음 줄이 빈 줄인가: leading trivia 첫 줄이 공백뿐이고 줄 끝으로 끝난다.
        foreach (SyntaxTrivia trivia in leading)
        {
            if (trivia.IsKind(SyntaxKind.WhitespaceTrivia))
            {
                continue;
            }

            return trivia.IsKind(SyntaxKind.EndOfLineTrivia) ? 0 : 1;
        }

        return 1;
    }
}
