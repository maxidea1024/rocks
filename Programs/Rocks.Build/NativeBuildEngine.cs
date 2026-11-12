namespace Rocks.Build;

/// <summary>
/// 한 output directory의 writer는 프로세스 간 lock으로 직렬화한다. caller는 호출 동안 request의
/// collection을 변경하지 않는다. CMake/Python을 실행하지 않으며 성공한 action만 cache에 기록한다.
/// </summary>
public sealed class NativeBuildEngine
{
    public async Task<NativeBuildResult> BuildAsync(NativeBuildRequest request,
        CancellationToken cancellationToken = default)
    {
        string root = Path.GetFullPath(request.RootDirectory);
        request = request with
        {
            RootDirectory = root,
            OutputDirectory = Path.GetFullPath(request.OutputDirectory, root)
        };
        InputValidation.Validate(request);
        Directory.CreateDirectory(request.OutputDirectory);
        await using var outputLock = await AcquireOutputLockAsync(request.OutputDirectory, cancellationToken);
        var plan = ActionGraph.Create(request);
        string compilationDatabase = CompilationDatabase.Write(request.OutputDirectory, request.RootDirectory, plan);
        var store = new IncrementalStore(request.OutputDirectory);
        string toolchainKey = Fingerprints.Toolchain(request.Toolchain);
        string initialInputs = InputSnapshot(request, plan);
        var tasks = new Dictionary<string, Task<NativeActionResult>>(StringComparer.Ordinal);
        using var slots = new SemaphoreSlim(request.MaxParallelism);
        foreach (var action in plan.Actions)
        {
            Task<NativeActionResult>[] predecessors = action.Dependencies.Select(id => tasks[id]).ToArray();
            tasks.Add(action.Id, RunAsync(action, predecessors));
        }

        NativeActionResult[] results;
        bool stable = false;
        try
        {
            results = await Task.WhenAll(tasks.Values);
        }
        finally
        {
            // 취소/예외에서도 입력 변경을 검사한다. 변화 중 생성된 산출물은 다음 실행에서 hit이
            // 되면 안 되므로 완료된 action의 cache entry를 제거한다.
            try
            {
                stable = toolchainKey == Fingerprints.Toolchain(request.Toolchain) && initialInputs == InputSnapshot(request, plan);
            }
            catch (IOException)
            {
                stable = false;
            }

            if (!stable)
            {
                foreach (var task in tasks.Values.Where(value => value.IsCompletedSuccessfully && value.Result.Executed))
                {
                    store.Forget(task.Result.Id);
                }
            }

            store.Save();
        }

        bool succeeded = stable && results.All(value => value.Succeeded);
        return new NativeBuildResult
        {
            Succeeded = succeeded,
            ExecutedActions = results.Count(value => value.Executed),
            SkippedActions = results.Count(value => !value.Executed && value.Succeeded),
            ModuleOutputs = plan.ModuleOutputs,
            Actions = results,
            CompilationDatabasePath = compilationDatabase,
            Failure = !stable ? "Source/toolchain inputs changed during the build." :
                results.FirstOrDefault(value => !value.Succeeded)?.StdErr
        };

        async Task<NativeActionResult> RunAsync(NativeAction action, Task<NativeActionResult>[] dependencies)
        {
            var completed = await Task.WhenAll(dependencies);
            cancellationToken.ThrowIfCancellationRequested();
            if (completed.Any(value => !value.Succeeded))
            {
                return new(action.Id, action.Module, action.Kind, false, false, -1, "",
                    "A prerequisite action failed.", action.Outputs[0]);
            }
            // 의존 작업을 기다리는 동안 실행 슬롯을 보유하지 않아 작은 예산에서도 진척한다.
            await slots.WaitAsync(cancellationToken);
            try
            {
                return await Executor.RunAsync(request, action, request.OutputDirectory, toolchainKey, store, cancellationToken);
            }
            finally
            {
                slots.Release();
            }
        }
    }

    private static string InputSnapshot(NativeBuildRequest request, NativePlan plan)
    {
        var produced = plan.Actions.SelectMany(value => value.Outputs).ToHashSet(NativeToolchains.PathComparer);
        string[] files = plan.Actions.SelectMany(value => value.Inputs.Concat(value.DeclaredInputs))
            .Where(value => !produced.Contains(value)).Distinct(NativeToolchains.PathComparer).Order(StringComparer.Ordinal).ToArray();
        string[] headers = plan.Actions.SelectMany(value => value.HeaderDirectories)
            .Distinct(NativeToolchains.PathComparer).Order(StringComparer.Ordinal).ToArray();
        return Fingerprints.Strings([Fingerprints.Paths(files), Fingerprints.Paths(headers, true, request.OutputDirectory, request.RootDirectory)]);
    }

    private static async Task<FileStream> AcquireOutputLockAsync(string output, CancellationToken cancellationToken)
    {
        string path = Path.Combine(output, ".native-build.lock");
        while (true)
        {
            cancellationToken.ThrowIfCancellationRequested();
            try
            {
                return new FileStream(path, FileMode.OpenOrCreate, FileAccess.ReadWrite, FileShare.None);
            }
            catch (IOException)
            {
                await Task.Delay(50, cancellationToken);
            }
        }
    }
}
