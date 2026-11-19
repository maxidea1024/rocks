using System.Text.Json;
using Rocks.Build;
using Rocks.Tooling.Common;

/// <summary>
/// action 소요 시간 기록(NativeActionResult.DurationMilliseconds — Docs/Design/ReflectionGeneratedSources.ko.md GSD12)을 검증한다.
/// 시간은 주입한 시계로만 흐르게 해 값을 정확히 단언한다(실제 경과 시간에 기대지 않는다).
/// </summary>
internal static class ActionDurationTests
{
    private const long TicksPerSecond = 1000;

    private const long SuccessMilliseconds = 1500;

    private const long FailureMilliseconds = 250;

    private const int FailureExitCode = 42;

    // 호출자가 Advance로만 움직이는 시계다(1 tick = 1 ms).
    private sealed class ManualClock : TimeProvider
    {
        private long now;

        public override long TimestampFrequency => TicksPerSecond;

        public override long GetTimestamp() => now;

        public void Advance(long milliseconds) => now += milliseconds;
    }

    public static async Task RunAsync(NativeBuildRequest request, string root)
    {
        string outputRoot = Directory.CreateDirectory(Path.Combine(root, "action-duration")).FullName;
        string input = Path.Combine(outputRoot, "input.cpp");
        string output = Path.Combine(outputRoot, "output.obj");
        File.WriteAllText(input, "input");
        var action = new NativeAction("duration-test", "Fixture", "Compile", "fake",
            ["{OUTPUT0}"], [output], [input], [], [], [], []);
        var store = new IncrementalStore(outputRoot);
        var clock = new ManualClock();
        string keyBefore = action.Key("fixture", outputRoot, request.RootDirectory);

        void Check(bool condition, string message)
        {
            if (!condition)
            {
                throw new InvalidOperationException(message);
            }
        }

        Task<ProcessResult> Succeed(ProcessSpec spec, CancellationToken _)
        {
            clock.Advance(SuccessMilliseconds);
            File.WriteAllText(spec.Arguments[0], "object");
            return Task.FromResult(new ProcessResult(0, "", ""));
        }

        Task<ProcessResult> Fail(ProcessSpec spec, CancellationToken _)
        {
            clock.Advance(FailureMilliseconds);
            return Task.FromResult(new ProcessResult(FailureExitCode, "", "diagnostic"));
        }

        var executed = await Executor.RunAsync(request, action, outputRoot, "fixture", store, CancellationToken.None, Succeed, clock: clock);
        Check(executed.Executed && executed.Succeeded && executed.DurationMilliseconds == SuccessMilliseconds,
            $"An executed action must record the time its tool ran: {executed.DurationMilliseconds}");

        // cache hit은 시계가 흘러도 0이다(실행하지 않았다).
        clock.Advance(SuccessMilliseconds);
        var cached = await Executor.RunAsync(request, action, outputRoot, "fixture", store, CancellationToken.None, Succeed, clock: clock);
        Check(!cached.Executed && cached.Succeeded && cached.DurationMilliseconds == 0,
            $"A cached action must record zero duration: {cached.DurationMilliseconds}");

        // 측정값은 key에 들어가지 않는다(같은 입력이면 실행 전후 key가 같다).
        Check(action.Key("fixture", outputRoot, request.RootDirectory) == keyBefore, "The action key must not depend on the measured duration.");

        // 실패한 action도 걸린 시간을 남긴다.
        File.AppendAllText(input, " changed");
        var failed = await Executor.RunAsync(request, action, outputRoot, "fixture", store, CancellationToken.None, Fail, clock: clock);
        Check(failed.Executed && !failed.Succeeded && failed.ExitCode == FailureExitCode && failed.DurationMilliseconds == FailureMilliseconds,
            $"A failed action must record the time its tool ran: {failed.DurationMilliseconds}");

        // action-report.json은 NativeBuildResult를 그대로 직렬화한다. 필드가 그 이름으로 실린다.
        using JsonDocument report = JsonDocument.Parse(JsonSerializer.Serialize(executed));
        Check(report.RootElement.GetProperty("DurationMilliseconds").GetInt64() == SuccessMilliseconds,
            "The action report must carry DurationMilliseconds.");
    }
}
