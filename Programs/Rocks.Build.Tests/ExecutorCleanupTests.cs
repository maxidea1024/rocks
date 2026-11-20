using Rocks.Build;
using Rocks.Tooling.Common;

internal static class ExecutorCleanupTests
{
    public static async Task RunAsync(NativeBuildRequest request, string root)
    {
        string outputRoot = Directory.CreateDirectory(Path.Combine(root, "cleanup-faults")).FullName;
        string input = Path.Combine(outputRoot, "input.cpp");
        string output = Path.Combine(outputRoot, "output.obj");
        File.WriteAllText(input, "input");
        File.WriteAllText(output, "old output");
        var action = new NativeAction("cleanup-test", "Fixture", "Compile", "fake",
            ["{OUTPUT0}"], [output], [input], [], [], [], []);
        var store = new IncrementalStore(outputRoot);
        string key = action.Key("fixture", outputRoot, request.RootDirectory);

        void Check(bool condition, string message)
        {
            if (!condition)
            {
                throw new InvalidOperationException(message);
            }
        }

        void FailDelete(string path) => throw new IOException("injected sharing violation");
        Task<ProcessResult> Success(ProcessSpec spec, CancellationToken _)
        {
            File.WriteAllText(spec.Arguments[0], "new output");
            return Task.FromResult(new ProcessResult(0, "", ""));
        }

        var successfulWork = await Executor.RunAsync(request, action, outputRoot, "fixture", store,
            CancellationToken.None, Success, FailDelete);
        Check(!successfulWork.Succeeded && successfulWork.StdErr.Contains("injected sharing violation") &&
            !store.IsCurrent(action, key), "Cleanup failure must fail the action and invalidate its cache entry.");

        File.WriteAllText(output, "old output");
        var failedWork = await Executor.RunAsync(request, action, outputRoot, "fixture", store,
            CancellationToken.None, (_, _) => Task.FromResult(new ProcessResult(42, "", "compiler diagnostic")), FailDelete);
        Check(!failedWork.Succeeded && failedWork.ExitCode == 42 && failedWork.StdErr.Contains("compiler diagnostic") &&
            failedWork.StdErr.Contains("injected sharing violation") && File.ReadAllText(output) == "old output",
            "Cleanup failure must retain the original compiler diagnostic and published output.");

        bool deleteCalled = false;
        var incomplete = await Executor.RunAsync(request, action, outputRoot, "fixture", store,
            CancellationToken.None, (_, _) => throw new ProcessCleanupException("child still running"),
            _ => deleteCalled = true);
        Check(!incomplete.Succeeded && !deleteCalled && incomplete.StdErr.Contains("child still running") &&
            incomplete.StdErr.Contains(".native-tmp") && !store.IsCurrent(action, key),
            "Incomplete child cleanup must quarantine the unique staging directory and invalidate cache.");

        using var cancellation = new CancellationTokenSource();
        cancellation.Cancel();
        try
        {
            await Executor.RunAsync(request, action, outputRoot, "fixture", store, CancellationToken.None,
                (_, _) => throw new OperationCanceledException("original cancellation", cancellation.Token), FailDelete);
            throw new InvalidOperationException("Cancellation was ignored.");
        }
        catch (OperationCanceledException error)
        {
            Check(error.CancellationToken == cancellation.Token && error.Message.Contains("original cancellation") &&
                error.Message.Contains("injected sharing violation") && error.InnerException is OperationCanceledException,
                "Cancellation and the cleanup diagnostic must both reach the caller.");
        }

        deleteCalled = false;
        try
        {
            await Executor.RunAsync(request, action, outputRoot, "fixture", store, CancellationToken.None,
                (_, _) => throw new ProcessCleanupException("child still running",
                    new OperationCanceledException("original cancellation", cancellation.Token)), _ => deleteCalled = true);
            throw new InvalidOperationException("Cancellation with incomplete cleanup was ignored.");
        }
        catch (OperationCanceledException error)
        {
            Check(!deleteCalled && error.CancellationToken == cancellation.Token &&
                error.Message.Contains(".native-tmp") && error.InnerException is ProcessCleanupException,
                "Cancellation with a live child must retain its staging directory and diagnostic.");
        }

        // 출력 게시(Docs/Design/ClangVersionSupport.ko.md 개정 6 (자)): 다른 프로세스(백신·인덱서)가 대상 object를 삭제 공유로 열고 있어도
        // Windows의 POSIX 의미 이름 바꾸기로 교체하고, 삭제 공유 없이 열고 있으면 원본·대상 경로를 담은 실패로 알린다(대기·재시도 없음).
        if (OperatingSystem.IsWindows())
        {
            File.WriteAllText(output, "old output");
            using (var shared = new FileStream(output, FileMode.Open, FileAccess.Read, FileShare.Read | FileShare.Delete))
            {
                var replaced = await Executor.RunAsync(request, action, outputRoot, "fixture", store, CancellationToken.None, Success);
                Check(replaced.Succeeded, "Publishing must replace an output held open with delete sharing: " + replaced.StdErr);
            }

            Check(File.ReadAllText(output) == "new output", "The replaced output must hold the new contents.");
            File.WriteAllText(output, "old output");
            using (var exclusive = new FileStream(output, FileMode.Open, FileAccess.Read, FileShare.Read))
            {
                var blocked = await Executor.RunAsync(request, action, outputRoot, "fixture", store, CancellationToken.None, Success);
                Check(!blocked.Succeeded && blocked.StdErr.Contains("Cannot publish", StringComparison.Ordinal) &&
                    blocked.StdErr.Contains(output, StringComparison.OrdinalIgnoreCase) && !store.IsCurrent(action, key),
                    "A publish failure must name the step and the output path and invalidate the cache entry: " + blocked.StdErr);
            }

            Check(File.ReadAllText(output) == "old output", "A failed publish must keep the previous output.");
        }
    }
}
