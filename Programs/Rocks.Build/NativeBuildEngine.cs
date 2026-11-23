using Rocks.Tooling.Common;

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
        // IDE 계획 모드는 action을 세우지 않는다(IdePlanner.Plan만 쓴다, Docs/Design/ProjectFiles.ko.md 8.1절).
        if (request.PlanMode != NativePlanMode.Build)
        {
            throw new ArgumentException("BuildAsync accepts PlanMode Build only; IDE plans go through IdePlanner.Plan.");
        }

        string root = Path.GetFullPath(request.RootDirectory);
        request = request with
        {
            RootDirectory = root,
            OutputDirectory = Path.GetFullPath(request.OutputDirectory),
            SdkDirectory = request.SdkDirectory is null ? null : Path.GetFullPath(request.SdkDirectory)
        };
        InputValidation.Validate(request);
        // 경로를 디스크에 있는 철자로 맞춘다(Docs/Design/UnityBuildPch.ko.md 4.4절). 같은 트리를 c:\와 C:\처럼 철자만 다르게 실행해도
        // 인자·action key·compile DB·도구 빌드 ID가 같다. 검증은 받은 그대로의 요청으로 먼저 한다.
        request = PathSpelling.OnDisk(request);
        Directory.CreateDirectory(request.OutputDirectory);
        await using var outputLock = await AcquireOutputLockAsync(request.OutputDirectory, cancellationToken);
        // 실행 파일마다 모듈 표 TU를 생성 소스로 더한다(Docs/Design/ModuleLoader.ko.md 3.2절). 아래에서 규칙 생성 TU와 함께 쓴다.
        request = ModuleTables.Add(request);
        // 규칙이 만든 생성 번역 단위(도구 표 등)를 먼저 쓴다. 그 뒤의 BuildInfo·액션 그래프는 이것을 보통 소스로 본다.
        request = GeneratedSources.Write(request);
        // 계획 두 단계(Docs/Design/ReflectionFoundations.ko.md 8.1절): BuildInfo 정의 TU를 만들어 실행 파일 소스에 더한 뒤 액션
        // 그래프를 세운다. 첫 InputSnapshot은 Prepare가 읽은 값의 메모로 만들어 마지막 비교가 Prepare 읽기부터 빌드 끝까지를
        // 덮는다(ActionGraph.Create는 파일 내용을 읽지 않으므로 메모에 없는 파일은 그 뒤 여기서 처음 읽는다).
        // 작업 파일 집합(Docs/Design/UnityBuildPch.ko.md 7.1절)은 계획을 세우기 전에 한 번 구한다. git이 끝날 때까지 기다리고 실패는
        // 빌드 오류다(시간으로 포기해 빈 집합으로 진행하면 같은 입력에서 다른 묶음 구성이 나온다).
        if (request.Unity && request.Adaptive && request.WorkingSet is null)
        {
            request = request with
            {
                WorkingSet = await SourceWorkingSet.QueryAsync(request.RootDirectory, request.Toolchain.Environment, cancellationToken)
            };
        }

        var prepared = BuildInfoGenerator.Prepare(request);
        request = prepared.Request;
        var plan = prepared.Plan ?? ActionGraph.Create(request);
        // 묶음 파일을 쓴다(Docs/Design/UnityBuildPch.ko.md 6.3절 — 출력 잠금 아래, 구성이 같으면 다시 쓰지 않는다). compile action의
        // 입력이므로 첫 InputSnapshot 전에 있어야 한다.
        foreach (UnityBatch batch in plan.UnityBatches)
        {
            AtomicFile.WriteIfChanged(batch.File, new System.Text.UTF8Encoding(false).GetBytes(batch.Content));
        }

        // 정의 헤더와 PCH stub 소스도 action의 입력이다(8.2·8.5절).
        foreach (PlannedFile file in plan.GeneratedFiles)
        {
            AtomicFile.WriteIfChanged(file.Path, new System.Text.UTF8Encoding(false).GetBytes(file.Content));
        }

        string compilationDatabase = CompilationDatabase.Write(request.OutputDirectory, request.RootDirectory, plan);
        if (request.PlanOnly)
        {
            // 계획만: 생성 TU·묶음 파일·compile DB는 위에서 썼다. action과 증분 기록은 건드리지 않는다.
            return new NativeBuildResult
            {
                IdeCompiles = IdePlanner.Select(plan, request.OutputDirectory),
                Succeeded = true,
                ExecutedActions = 0,
                SkippedActions = 0,
                ModuleOutputs = plan.ModuleOutputs,
                Actions = [],
                CompilationDatabasePath = compilationDatabase,
                UnityDefineExcluded = plan.UnityDefineExcluded,
                UnityBatches = plan.UnityBatches.Count,
                PchActions = plan.Actions.Count(action => action.Kind == "Pch"),
                AdaptiveSeparated = plan.AdaptiveSeparated,
                Notices = plan.Notices,
                PlanOnly = true,
                ToolchainFingerprint = prepared.ToolchainFingerprint
            };
        }

        var store = new IncrementalStore(request.OutputDirectory);
        // 지난 실행의 include 소유 위반 보고는 실행 전에 지운다(이번 빌드가 검사 전에 실패해도 옛 보고가 남지 않는다 — BS1b).
        File.Delete(Path.Combine(request.OutputDirectory, IncludeOwnership.ReportFileName));
        string toolchainKey = Fingerprints.Toolchain(request.Toolchain);
        string initialInputs = InputSnapshot(request, plan, prepared.Digests);
        // 입력 메모는 첫 InputSnapshot이 읽은 값에서 시작한다(같은 파일을 action마다 다시 읽지 않는다 — BuildInputs).
        var inputs = new BuildInputs(request.RootDirectory, request.OutputDirectory, request.Toolchain, plan, prepared.Digests);
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
                // 의존 기록에 적은 파일 가운데 InputSnapshot 밖의 것(header 디렉터리 밖으로 벗어난 include)도 다시 읽어 대조한다.
                stable = toolchainKey == Fingerprints.Toolchain(request.Toolchain) && initialInputs == InputSnapshot(request, plan, null) &&
                    inputs.ChangedDependencies().Count == 0;
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
        string? moduleManifest = null;
        IReadOnlyList<string> notices = plan.Notices;
        string? ownershipFailure = null;
        if (succeeded)
        {
            // include 소유 모듈 검사(Docs/Design/BuildTargets 3.5절 BD11·BS1b): 모든 컴파일 액션의 의존 기록이 이번 실행에서 게시됐거나
            // 증분 검증됐다. 위반은 빌드 실패이고, 공개 헤더 검사 기록으로 쓰지 않는 공개 의존을 알린다.
            IncludeOwnership.Result ownership = IncludeOwnership.Check(request, plan.Actions);
            notices = [.. notices, .. ownership.Notices];
            if (ownership.ViolationCount != 0)
            {
                ownershipFailure = $"Include ownership check failed ({ownership.ViolationCount} includes of headers the source cannot see; " +
                    "Docs/Design/BuildTargets section 3.5 BS1b):\n" + string.Join("\n", ownership.Violations) +
                    (ownership.ViolationCount > ownership.Violations.Count ? $"\n... and {ownership.ViolationCount - ownership.Violations.Count} more" : "") +
                    "\nAll violations: " + ownership.ReportPath;
                succeeded = false;
            }
        }

        if (succeeded)
        {
            RemoveStaleOutputs(request, plan);
            // Modular 타깃의 모듈 manifest는 링크가 모두 성공한 뒤에 쓴다(Docs/Design/BuildTargets 3.8절 BS8d1).
            moduleManifest = ModuleManifests.Write(request);
            // Modular Windows 공유 라이브러리의 export 수 진단(BS8d3)이다.
            (IReadOnlyDictionary<string, int> exportCounts, IReadOnlyList<string> unreadable) = ModularExports.Count(request);
            notices = [.. notices, .. ModularExports.Diagnose(exportCounts, unreadable: unreadable)];
            if (request.Timing)
            {
                TimingReport.Write(request, plan);
            }
        }
        // 도구 빌드 ID는 link가 성공한(실행했거나 재사용한) 실행 파일만 알린다. 입력이 바뀐 빌드는 어떤 ID도 확정하지 않는다.
        var toolBuildIds = new Dictionary<string, string>(StringComparer.Ordinal);
        foreach ((string module, string id) in prepared.ToolBuildIds)
        {
            if (stable && ownershipFailure is null && results.Any(value => value.Kind == "Link" && value.Succeeded &&
                    string.Equals(value.Module, module, StringComparison.OrdinalIgnoreCase)))
            {
                toolBuildIds.Add(module, id);
            }
        }

        return new NativeBuildResult
        {
            IdeCompiles = IdePlanner.Select(plan, request.OutputDirectory),
            Succeeded = succeeded,
            ExecutedActions = results.Count(value => value.Executed),
            SkippedActions = results.Count(value => !value.Executed && value.Succeeded),
            ModuleOutputs = plan.ModuleOutputs,
            Actions = results,
            CompilationDatabasePath = compilationDatabase,
            UnityDefineExcluded = plan.UnityDefineExcluded,
            UnityBatches = plan.UnityBatches.Count,
            PchActions = plan.Actions.Count(action => action.Kind == "Pch"),
            AdaptiveSeparated = plan.AdaptiveSeparated,
            Notices = notices,
            Failure = !stable ? "Source/toolchain inputs changed during the build." :
                ownershipFailure ?? results.FirstOrDefault(value => !value.Succeeded)?.StdErr,
            ToolchainFingerprint = prepared.ToolchainFingerprint,
            ToolBuildIds = toolBuildIds,
            ModuleManifestPath = moduleManifest
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
                return await Executor.RunAsync(request, action, request.OutputDirectory, toolchainKey, store, cancellationToken, inputs: inputs);
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

    // 빌드가 성공하면 이번 변형의 object 디렉터리(obj/<모듈>/<변형>)와 묶음 디렉터리(Unity/<모듈>)에서 이 계획이 만들지 않은 파일을
    // 지운다(Docs/Design/UnityBuildPch.ko.md 6.4절 — 낡은 묶음·object가 검사 도구와 디스크에 남지 않게). 다른 변형 디렉터리는 건드리지
    // 않는다. 변형 디렉터리를 쓰기 전 배치(obj/<모듈> 바로 아래의 object)의 파일도 지운다 — 어느 계획도 그 자리에 쓰지 않는다. 지우지
    // 못한 파일은 다음 성공한 빌드가 다시 시도한다(입력이 아니므로 빌드를 실패시키지 않는다).
    private static void RemoveStaleOutputs(NativeBuildRequest request, NativePlan plan)
    {
        var produced = plan.Actions.SelectMany(action => action.Outputs).ToHashSet(NativeToolchains.PathComparer);
        var batches = plan.UnityBatches.Select(batch => batch.File).ToHashSet(NativeToolchains.PathComparer);
        foreach ((string module, string directory) in plan.ObjectDirectories)
        {
            Remove(directory, produced, SearchOption.AllDirectories);
            Remove(Path.GetDirectoryName(directory)!, produced, SearchOption.TopDirectoryOnly);
            // 묶음 디렉터리는 유니티를 켠 변형만 쓴다. 끈 빌드(검증 변형)가 켠 빌드의 묶음 파일을 지우지 않는다.
            if (request.Unity)
            {
                Remove(UnityBuild.DirectoryOf(request.OutputDirectory, module), batches, SearchOption.AllDirectories);
            }
        }

        // PCH를 켠 빌드는 이번 계획이 쓰지 않은 PCH 변형 디렉터리(<out>/Pch/<제공 모듈>/<변형>)를 지운다(8.4절 — 인자가 바뀌어 낡은
        // 변형이 수십–수백 MB씩 남지 않게). PCH를 끈 빌드는 지우지 않는다.
        if (request.Pch)
        {
            var used = plan.PchDirectories.ToHashSet(NativeToolchains.PathComparer);
            string pchRoot = Path.Combine(request.OutputDirectory, PchBuild.DirectoryName);
            foreach (string provider in Directory.Exists(pchRoot) ? Directory.GetDirectories(pchRoot) : [])
            {
                foreach (string variant in Directory.GetDirectories(provider))
                {
                    if (used.Contains(variant))
                    {
                        continue;
                    }

                    try
                    {
                        Directory.Delete(variant, true);
                    }
                    catch (Exception error) when (error is IOException or UnauthorizedAccessException)
                    {
                        Console.Error.WriteLine($"Build outputs: warning: cannot delete the stale PCH variant {variant}: {error.Message}");
                    }
                }
            }
        }

        static void Remove(string directory, HashSet<string> keep, SearchOption depth)
        {
            if (!Directory.Exists(directory))
            {
                return;
            }

            foreach (string file in Directory.EnumerateFiles(directory, "*", depth).ToArray())
            {
                if (keep.Contains(file))
                {
                    continue;
                }

                try
                {
                    File.Delete(file);
                }
                catch (Exception error) when (error is IOException or UnauthorizedAccessException)
                {
                    Console.Error.WriteLine($"Build outputs: warning: cannot delete the stale file {file}: {error.Message}");
                }
            }
        }
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
