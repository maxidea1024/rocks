using Rocks.Tooling.Checks;

namespace Rocks.ToolTests;

/// <summary>
/// C# 선언 사이 빈 줄 검사(<see cref="CSharpMemberSpacing"/> — Docs/CodingStyle.md "C# 멤버 사이 빈 줄 규칙")의 시험이다. 결정적인
/// 입력 문자열마다 위반 행과 고친 텍스트를 단언한다(dotnet format을 실행하지 않는다).
/// </summary>
internal static class CSharpMemberSpacingTests
{
    // 줄 목록을 LF로 잇는다(끝 줄 끝 포함).
    private static string Lines(params string[] lines)
    {
        return string.Join('\n', lines) + "\n";
    }

    private static void Expect(string name, string text, int[] lines, string? fixedText = null)
    {
        CSharpMemberSpacing.Result result = CSharpMemberSpacing.Inspect(text);
        Verify.Equal(0, result.ParseErrorLine, name + ": parse error");
        Verify.That(result.Lines.SequenceEqual(lines), $"{name}: lines {string.Join(',', result.Lines)}");
        // 고친 텍스트를 준 사례와 위반 없는 사례(원문 그대로)만 텍스트를 단언한다.
        if (fixedText is not null || lines.Length == 0)
        {
            Verify.Equal(fixedText ?? text, result.Fixed, name + ": fixed text");
        }

        // 고친 텍스트를 다시 보면 위반이 없고, 연속 빈 줄(IDE2000)도 생기지 않는다.
        CSharpMemberSpacing.Result again = CSharpMemberSpacing.Inspect(result.Fixed);
        Verify.That(again.Lines.Count == 0, name + ": fixed text is clean");
        Verify.That(!result.Fixed.Contains("\n\n\n", StringComparison.Ordinal) &&
            !result.Fixed.Contains("\r\n\r\n\r\n", StringComparison.Ordinal), name + ": no double blank line");
    }

    public static void Register(TestSuite suite, Options options)
    {
        _ = options;
        suite.Add("MemberSpacing.MembersNeedBlankLines", () =>
        {
            // 위반: 메서드 둘, 속성 둘(한 줄 자동 속성도), 필드 뒤 메서드.
            Expect("methods", Lines("class A", "{", "    void X() { }", "    void Y() { }", "}"), [4],
                Lines("class A", "{", "    void X() { }", "", "    void Y() { }", "}"));
            Expect("auto properties", Lines("class A", "{", "    int P { get; }", "    int Q { get; }", "}"), [4]);
            Expect("field then method", Lines("class A", "{", "    int f;", "    void X() { }", "}"), [4]);
            // 통과: 빈 줄이 있음, 첫 멤버, 공백만 있는 빈 줄.
            Expect("separated", Lines("class A", "{", "    void X() { }", "", "    void Y() { }", "}"), []);
            Expect("first member", Lines("class A", "{", "    void X() { }", "}"), []);
            Expect("whitespace blank line", Lines("class A", "{", "    void X() { }", "    ", "    void Y() { }", "}"), []);
            // 인터페이스의 본문 없는 선언도 대상이다.
            Expect("interface", Lines("interface I", "{", "    void X();", "    void Y();", "}"), [4]);
            return Task.CompletedTask;
        });
        suite.Add("MemberSpacing.Exceptions", () =>
        {
            // 연속 필드·이벤트 필드는 묶어도 된다.
            Expect("field group", Lines("class A", "{", "    const int A = 1;", "    int b;", "    event System.Action? E;",
                "}"), []);
            // 열거형 멤버는 대상이 아니다.
            Expect("enum", Lines("enum E", "{", "    A,", "    B,", "}"), []);
            // 사이에 전처리 지시문이 있으면 보지 않는다(꺼진 쪽 선언은 보이지 않는다).
            Expect("directive", Lines("class A", "{", "    void X() { }", "#if DEBUG", "    void Y() { }", "#endif",
                "    void Z() { }", "}"), []);
            Expect("disabled", Lines("class A", "{", "    void X() { }", "#if NEVER", "    void Y() { }", "    void W() { }",
                "#endif", "}"), []);
            // 레코드 위치 매개변수·객체 초기화자는 대상이 아니다.
            Expect("record parameters", Lines("record R(int A,", "    int B);"), []);
            // 최상위 문장은 문장이다(선언 사이 규칙이 아니다) — 문장 뒤 선언도 보지 않는다.
            Expect("top-level statements", Lines("using System;", "", "int a = 1;", "int b = a;", "Console.WriteLine(b);",
                "static void Local() { }", "class A { }", "class B { }"), [8]);
            Expect("initializer", Lines("class A", "{", "    object M() => new { X = 1,", "        Y = 2 };", "}"), []);
            return Task.CompletedTask;
        });
        suite.Add("MemberSpacing.CommentsAndAttributesBelongToNext", () =>
        {
            // 주석·특성 위에 빈 줄이 와야 한다 — 주석과 선언 사이의 빈 줄은 세지 않는다.
            Expect("comment", Lines("class A", "{", "    void X() { }", "    // 설명", "    void Y() { }", "}"), [4],
                Lines("class A", "{", "    void X() { }", "", "    // 설명", "    void Y() { }", "}"));
            Expect("comment then blank", Lines("class A", "{", "    void X() { }", "    // 설명", "", "    void Y() { }", "}"),
                [4]);
            Expect("attribute", Lines("class A", "{", "    void X() { }", "    [System.Obsolete]", "    void Y() { }", "}"),
                [4]);
            Expect("documented", Lines("class A", "{", "    void X() { }", "", "    /// <summary>Y.</summary>", "    void Y() { }",
                "}"), []);
            // 줄 끝 주석은 앞 선언에 남는다.
            Expect("trailing comment", Lines("class A", "{", "    void X() { } // 끝", "", "    void Y() { }", "}"), []);
            return Task.CompletedTask;
        });
        suite.Add("MemberSpacing.NamespaceAndCompilationUnit", () =>
        {
            Expect("compilation unit", Lines("class A { }", "class B { }"), [2]);
            Expect("file scoped", Lines("namespace N;", "", "class A { }", "class B { }"), [4]);
            Expect("block namespace", Lines("namespace N", "{", "    class A { }", "    delegate void D();", "}"), [4]);
            // 같은 줄의 두 선언은 줄을 나누고 빈 줄을 만든다.
            Expect("same line", Lines("class A", "{", "    void X() { } void Y() { }", "}"), [3],
                Lines("class A", "{", "    void X() { } ", "", "void Y() { }", "}"));
            return Task.CompletedTask;
        });
        suite.Add("MemberSpacing.LineEndingsParseErrorsAndFiles", () =>
        {
            // CRLF 파일은 CRLF로 고친다.
            string crlf = "class A\r\n{\r\n    void X() { }\r\n    void Y() { }\r\n}\r\n";
            Expect("crlf", crlf, [4], "class A\r\n{\r\n    void X() { }\r\n\r\n    void Y() { }\r\n}\r\n");
            // 구문 오류는 건너뛰지 않고 첫 오류 행을 알린다.
            CSharpMemberSpacing.Result broken = CSharpMemberSpacing.Inspect(Lines("class A", "{", "    void X( { }", "}"));
            Verify.That(broken.ParseErrorLine == 3 && broken.Lines.Count == 0, "parse error line");

            // 파일: 검사는 바꾸지 않고 보고, apply는 BOM을 지키며 고친다.
            string directory = Path.Combine(Path.GetTempPath(), "rocks-member-spacing-" + Environment.ProcessId);
            Directory.CreateDirectory(directory);
            try
            {
                string file = Path.Combine(directory, "A.cs");
                byte[] bom = [0xEF, 0xBB, 0xBF];
                byte[] body = System.Text.Encoding.UTF8.GetBytes(Lines("class A", "{", "    void X() { }", "    void Y() { }", "}"));
                File.WriteAllBytes(file, [.. bom, .. body]);
                IReadOnlyList<CheckFailure> found = CSharpMemberSpacing.Check(directory, [file], apply: false);
                Verify.That(found.Count == 1 && found[0].Path == "A.cs" && found[0].Line == 4 &&
                    found[0].Rule == CSharpMemberSpacing.Rule, "file violation");
                Verify.That(File.ReadAllBytes(file).SequenceEqual([.. bom, .. body]), "check does not write");
                Verify.That(CSharpMemberSpacing.Check(directory, [file], apply: true).Count == 0, "apply returns no failures");
                byte[] written = File.ReadAllBytes(file);
                Verify.That(written.Take(3).SequenceEqual(bom), "BOM kept");
                Verify.That(CSharpMemberSpacing.Check(directory, [file], apply: false).Count == 0, "clean after apply");

                string bad = Path.Combine(directory, "B.cs");
                File.WriteAllText(bad, Lines("class B", "{", "    void X( { }", "}"));
                IReadOnlyList<CheckFailure> parse = CSharpMemberSpacing.Check(directory, [bad], apply: true);
                Verify.That(parse.Count == 1 && parse[0].Line == 3 && parse[0].Message == "parse error", "file parse error");
            }
            finally
            {
                Directory.Delete(directory, recursive: true);
            }

            return Task.CompletedTask;
        });
    }
}
