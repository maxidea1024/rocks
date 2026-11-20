using System.Globalization;
using System.Text;
using System.Text.RegularExpressions;
using Rocks.Tooling.Common;

namespace Rocks.MetaCompiler;

// 첫 실행과 같은 합성 번역 단위 끝에 질문 줄을 붙여 Clang을 다시 실행하는 탐침의 공용 실행·격리다(Docs/Design/ClangVersionSupport.ko.md
// 4.5절). 탐침 텍스트는 앞부분(include·판정 형판 등) + 질문마다 한 줄 + 뒷부분이다. 질문 줄 하나의 오류(이름할 수 없는 형식, 인스턴스화
// hard error)가 번역 단위 전체를 실패시키므로, 진단의 `<stdin>` 위치(instantiation note 포함)를 질문 줄에 대응시켜 그 질문만 빼고 다시
// 실행한다. 어느 질문에도 대응하지 않는 오류는 묶음을 반으로 나눠 다시 실행한다. 실행 순서와 결과는 결정적이다.
internal static class ClangProbe
{
    // 질문 하나다. Index는 호출자의 질문 번호(실행이 달라도 같은 질문은 같은 번호), Text는 줄바꿈 없는 한 줄이다.
    internal readonly record struct Question(int Index, string Text);

    // 탐침 실행 하나의 결과다. 성공이면 Answers(모든 질문의 답), 실패면 null과 Failure(Clang의 첫 error 줄)·Blamed(오류의
    // `<stdin>` 위치가 질문 줄에 대응한 질문 → 그 오류 줄)·Unattributed(어느 질문에도 대응하지 않는 오류가 있거나 위치가 있는
    // 오류가 없음)다.
    internal sealed record Run<TAnswer>(Dictionary<int, TAnswer>? Answers, string Failure, Dictionary<int, string> Blamed, bool Unattributed);

    // 판정을 마친 질문의 답과 실패(그 질문의 Clang error 줄)다.
    internal sealed class Outcome<TAnswer>
    {
        public Dictionary<int, TAnswer> Answers { get; } = [];

        public Dictionary<int, string> Failures { get; } = [];
    }

    // baseLines는 첫 실행 합성 번역 단위의 줄 수(입력 헤더 include 줄), run은 그 단위 끝에 텍스트를 붙여 경고를 끄고 탐침 선언만
    // 덤프하는 Clang 실행, read는 성공한 실행의 덤프에서 질문 번호별 답을 읽는 함수다(덤프를 해석하지 못하면 null).
    internal sealed class Session<TAnswer>(int baseLines, Func<string, Task<ProcessResult>> run, Func<string, Dictionary<int, TAnswer>?> read)
    {
        // prelude(줄바꿈으로 끝난다) 뒤에 질문 줄과 epilogue를 붙여 실행한다. questions가 비면 앞뒤 부분만 컴파일된다.
        public async Task<Run<TAnswer>> RunAsync(string prelude, IReadOnlyList<Question> questions, string epilogue)
        {
            StringBuilder text = new(prelude);
            int line = baseLines + 1 + prelude.Count(character => character == '\n');
            Dictionary<int, int> questionLines = [];
            foreach (Question question in questions)
            {
                questionLines.Add(line++, question.Index);
                text.Append(question.Text).Append('\n');
            }

            text.Append(epilogue);
            ProcessResult result = await run(text.ToString());
            if (result.ExitCode != 0)
            {
                Dictionary<int, string> blamed = [];
                bool unattributed = Attribute(result.StdErr, questionLines, blamed);
                return new(null, FirstError(result), blamed, unattributed);
            }

            Dictionary<int, TAnswer>? answers = read(result.StdOut);
            if (answers is null || questions.Any(question => !answers.ContainsKey(question.Index)))
            {
                return new(null, "Clang reported no answer for the probe", [], true);
            }

            return new(answers, "", [], false);
        }

        // known(이미 실행한 결과)이 있으면 그것부터 쓴다. 실패한 질문은 Failures에, 나머지는 Answers에 담는다. 오류가 질문에
        // 대응하면 그 질문을 빼고 나머지를 다시 실행하고, 대응하지 않으면 묶음을 반으로 나눈다.
        public async Task JudgeAsync(string prelude, IReadOnlyList<Question> questions, string epilogue, Outcome<TAnswer> outcome, Run<TAnswer>? known = null)
        {
            if (questions.Count == 0)
            {
                return;
            }

            Run<TAnswer> probe = known ?? await RunAsync(prelude, questions, epilogue);
            if (probe.Answers is not null)
            {
                foreach (Question question in questions)
                {
                    outcome.Answers[question.Index] = probe.Answers[question.Index];
                }

                return;
            }

            if (probe.Blamed.Count != 0)
            {
                foreach ((int index, string failure) in probe.Blamed)
                {
                    outcome.Failures[index] = failure;
                }

                await JudgeAsync(prelude, questions.Where(question => !probe.Blamed.ContainsKey(question.Index)).ToList(), epilogue, outcome);
                return;
            }

            if (questions.Count == 1)
            {
                outcome.Failures[questions[0].Index] = probe.Failure;
                return;
            }

            int half = questions.Count / 2;
            await JudgeAsync(prelude, questions.Take(half).ToList(), epilogue, outcome);
            await JudgeAsync(prelude, questions.Skip(half).ToList(), epilogue, outcome);
        }
    }

    // 불완전한 대상을 public 헤더를 더해 다시 판정한다(NativeMemberProbe의 두 번째 탐침, 레코드 닫힘의 중첩 포인터 판정). prelude는
    // 헤더 목록 → 앞부분이다. 질문에 대응하지 않는 실패면 헤더를 하나씩(질문 없이) 컴파일해 실패하는 헤더를 빼고 다시 실행한다 — 이
    // 비용은 실패할 때만 든다. 헤더가 없거나 남은 헤더로도 앞부분조차 컴파일되지 않으면 null(호출자는 불완전 판정을 그대로 두고 허용한다).
    internal static async Task<Outcome<TAnswer>?> WidenAsync<TAnswer>(Session<TAnswer> session, Func<IReadOnlyList<string>, string> prelude, string epilogue,
        IReadOnlyList<Question> questions, IReadOnlyList<string> headers)
    {
        if (headers.Count == 0 || questions.Count == 0)
        {
            return null;
        }

        Run<TAnswer> wide = await session.RunAsync(prelude(headers), questions, epilogue);
        if (wide.Answers is null && wide.Unattributed)
        {
            List<string> usable = [];
            foreach (string header in headers)
            {
                if ((await session.RunAsync(prelude([header]), [], epilogue)).Answers is not null)
                {
                    usable.Add(header);
                }
            }

            if (usable.Count == 0)
            {
                return null;
            }

            if (usable.Count != headers.Count)
            {
                headers = usable;
                wide = await session.RunAsync(prelude(headers), questions, epilogue);
            }

            if (wide.Answers is null && wide.Unattributed && (await session.RunAsync(prelude(headers), [], epilogue)).Answers is null)
            {
                return null;
            }
        }

        Outcome<TAnswer> widened = new();
        await session.JudgeAsync(prelude(headers), questions, epilogue, widened, wide);
        return widened;
    }

    // 실패한 탐침 실행의 진단을 error(또는 fatal error)와 딸린 note 묶음으로 나눠, 묶음의 `<stdin>` 위치 중 질문 줄이 있으면 그
    // 질문에 그 error 줄을 대응시킨다(blamed). 어느 질문에도 대응하지 않는 묶음이 있거나 위치가 있는 error가 없으면 참이다.
    // Clang 진단 문구는 지역화될 수 있으므로 위치·심각도 형식만 해석한다(clang `file:line:col:`·clang-cl `file(line,col):`).
    internal static bool Attribute(string diagnostics, Dictionary<int, int> questionLines, Dictionary<int, string> blamed)
    {
        bool unattributed = false;
        bool sawError = false;
        string? current = null;
        bool currentMapped = false;
        foreach (string raw in diagnostics.Split('\n'))
        {
            string line = raw.Trim();
            Match match = CompilerDriver.ClangLocation.Match(line);
            if (!match.Success)
            {
                match = CompilerDriver.ClangClLocation.Match(line);
            }

            if (!match.Success)
            {
                continue;
            }

            string severity = match.Groups["severity"].Value;
            if (severity is "error" or "fatal error")
            {
                unattributed |= current is not null && !currentMapped;
                current = line;
                currentMapped = false;
                sawError = true;
            }
            else if (severity != "note")
            {
                unattributed |= current is not null && !currentMapped;
                current = null;
                continue;
            }

            if (current is not null && match.Groups["file"].Value == "<stdin>" &&
                int.TryParse(match.Groups["line"].Value, NumberStyles.None, CultureInfo.InvariantCulture, out int number) &&
                questionLines.TryGetValue(number, out int index))
            {
                blamed.TryAdd(index, current);
                currentMapped = true;
            }
        }

        unattributed |= current is not null && !currentMapped;
        return unattributed || !sawError;
    }

    // 탐침 실행의 첫 error(또는 fatal error) 줄이다. 위치·심각도 형식만 해석하고 그 줄을 그대로 옮긴다. 그런 줄이 없으면 비어
    // 있지 않은 첫 줄이다.
    internal static string FirstError(ProcessResult result)
    {
        string? fallback = null;
        foreach (string raw in result.StdErr.Split('\n'))
        {
            string line = raw.Trim();
            Match match = CompilerDriver.ClangLocation.Match(line);
            if (!match.Success)
            {
                match = CompilerDriver.ClangClLocation.Match(line);
            }

            if (match.Success && match.Groups["severity"].Value is "error" or "fatal error")
            {
                return line;
            }

            if (fallback is null && line.Length != 0)
            {
                fallback = line;
            }
        }

        return fallback ?? $"exit code {result.ExitCode}";
    }
}
