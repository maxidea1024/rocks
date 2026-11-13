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
        // 루트는 절대 경로만 받는다(상대 경로를 어느 기준으로 풀지 — 작업 디렉터리·소스 루트 — 가 단계마다 달라지지 않게).
        InputValidation.RequireAbsoluteRoots(request);
        string root = Path.GetFullPath(request.RootDirectory);
        request = request with
        {
            RootDirectory = root,
            OutputDirectory = Path.GetFullPath(request.OutputDirectory),
            SdkDirectory = request.SdkDirectory is null ? null : Path.GetFullPath(request.SdkDirectory)
        };
        InputValidation.Validate(request);
        Directory.CreateDirectory(request.OutputDirectory);
        await using var outputLock = await AcquireOutputLockAsync(request.OutputDirectory, cancellationToken);
        // 계획 두 단계(Docs/Design/ReflectionFoundations.ko.md 8.1절): BuildInfo 정의 TU를 만들어 실행 파일 소스에 더한 뒤 액션
        // 그래프를 세운다. 첫 InputSnapshot은 Prepare가 읽은 값의 메모로 만들어 마지막 비교가 Prepare 읽기부터 빌드 끝까지를
        // 덮는다(ActionGraph.Create는 파일 내용을 읽지 않으므로 메모에 없는 파일은 그 뒤 여기서 처음 읽는다).
        var prepared = BuildInfoGenerator.Prepare(request);
        request = prepared.Request;
        var plan = prepared.Plan ?? ActionGraph.Create(request);
        string compilationDatabase = CompilationDatabase.Write(request.OutputDirectory, request.RootDirectory, plan);
        var store = new IncrementalStore(request.OutputDirectory);
        string toolchainKey = Fingerprints.Toolchain(request.Toolchain);
        string initialInputs = InputSnapshot(request, plan, prepared.Digests);
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
                stable = toolchainKey == Fingerprints.Toolchain(request.Toolchain) && initialInputs == InputSnapshot(request, plan, null);
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
        // 도구 빌드 ID는 link가 성공한(실행했거나 재사용한) 실행 파일만 알린다. 입력이 바뀐 빌드는 어떤 ID도 확정하지 않는다.
        var toolBuildIds = new Dictionary<string, string>(StringComparer.Ordinal);
        foreach ((string module, string id) in prepared.ToolBuildIds)
        {
            if (stable && results.Any(value => value.Kind == "Link" && value.Succeeded &&
                    string.Equals(value.Module, module, StringComparison.OrdinalIgnoreCase)))
            {
                toolBuildIds.Add(module, id);
            }
        }

        return new NativeBuildResult
        {
            Succeeded = succeeded,
            ExecutedActions = results.Count(value => value.Executed),
            SkippedActions = results.Count(value => !value.Executed && value.Succeeded),
            ModuleOutputs = plan.ModuleOutputs,
            Actions = results,
            CompilationDatabasePath = compilationDatabase,
            Failure = !stable ? "Source/toolchain inputs changed during the build." :
                results.FirstOrDefault(value => !value.Succeeded)?.StdErr,
            ToolchainFingerprint = prepared.ToolchainFingerprint,
            ToolBuildIds = toolBuildIds
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

    // memo가 있으면 BuildInfo Prepare가 읽은 열거·digest를 쓴다(첫 snapshot). <out>/BuildInfo 아래 파일은 Prepare만 잠금 아래에서
    // 쓰는 생성 파일이라 빼고, 그 내용은 생성 TU 액션 key가 담는다.
    private static string InputSnapshot(NativeBuildRequest request, NativePlan plan, FileDigests? memo)
    {
        string buildInfo = BuildInfoGenerator.DirectoryOf(request.OutputDirectory);
        bool Skip(string path) => Fingerprints.IsUnder(path, buildInfo);
        var produced = plan.Actions.SelectMany(value => value.Outputs).ToHashSet(NativeToolchains.PathComparer);
        string[] files = plan.Actions.SelectMany(value => value.Inputs.Concat(value.DeclaredInputs))
            .Where(value => !produced.Contains(value) && !Skip(value)).Distinct(NativeToolchains.PathComparer).Order(StringComparer.Ordinal).ToArray();
        string[] headers = plan.Actions.SelectMany(value => value.HeaderDirectories)
            .Distinct(NativeToolchains.PathComparer).Order(StringComparer.Ordinal).ToArray();
        return Fingerprints.Strings([Fingerprints.Paths(files, memo: memo, skip: Skip),
            Fingerprints.Paths(headers, true, request.OutputDirectory, request.RootDirectory, memo, Skip)]);
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
