using System.ComponentModel;
using Rocks.Tooling.Common;

[assembly: System.Runtime.CompilerServices.InternalsVisibleTo("Rocks.Build.Tests")]

namespace Rocks.Build;

internal static class Executor
{
    public static async Task<NativeActionResult> RunAsync(NativeBuildRequest request, NativeAction action,
        string outputRoot, string toolchainKey, IncrementalStore store, CancellationToken cancellationToken,
        Func<ProcessSpec, CancellationToken, Task<ProcessResult>>? runProcess = null,
        Action<string>? deleteTemporaryRoot = null, TimeProvider? clock = null, BuildInputs? inputs = null)
    {
        cancellationToken.ThrowIfCancellationRequested();
        // inputs는 이 빌드의 입력 메모다. 없으면(action 하나만 실행하는 시험) 이 호출만의 메모를 쓴다.
        inputs ??= new BuildInputs(request.RootDirectory, outputRoot, request.Toolchain);
        string key = action.Key(toolchainKey, inputs);
        if (store.IsCurrent(action, key, inputs))
        {
            return Result(action, false, true, 0, "", "");
        }

        // 실행 전 검사(공유 PCH 사용 검사): 위반이면 도구를 실행하지 않고 실패한다.
        if (action.PreRun?.Value is string violation)
        {
            return Result(action, true, false, -1, "", violation);
        }

        // 소요 시간은 실제로 실행하는 action만 잰다(cache hit은 0 — NativeActionResult.DurationMilliseconds). clock은 시험이 시간을
        // 결정적으로 주입하는 자리이고 기본은 시스템 단조 시계다.
        TimeProvider time = clock ?? TimeProvider.System;
        long started = time.GetTimestamp();
        string temporaryRoot = Path.Combine(outputRoot, ".native-tmp", Guid.NewGuid().ToString("N"));
        Directory.CreateDirectory(temporaryRoot);
        string[] stagedOutputs = action.Outputs.Select(path => Path.Combine(temporaryRoot, Path.GetFileName(path))).ToArray();
        // 컴파일러가 쓰는 depfile이다(의존 기록의 원천). response file처럼 이름의 '+'로 staged 출력과 겹치지 않게 한다.
        string depfile = Path.Combine(temporaryRoot, DepfileName);
        // 펼친 인자다. key는 위에서 자리 표시 인자로 이미 계산했으므로 아래 response file·depfile 경로는 key에 들어가지 않는다.
        string[] arguments = action.ResolveArguments(stagedOutputs, depfile);
        ResponseFileQuoting? quoting = NativeCommandLine.QuotingOf(request.Toolchain, action);
        ResponseFileQuoting? syntax = NativeCommandLine.ResponseFileSyntax(request.ResponseFiles, quoting, action.Tool, arguments);
        NativeActionResult? result = null;
        // 실행한 프로세스의 자원 사용량이다(request.MeasureProcesses일 때만 청한다).
        ProcessUsage? usage = null;
        Exception? pending = null;
        bool unsafeToDelete = false;
        try
        {
            ProcessResult process;
            if (action.Kind == "Copy")
            {
                File.Copy(action.Inputs[0], stagedOutputs[0]);
                process = new ProcessResult(0, "", "");
            }
            else
            {
                IReadOnlyList<string> processArguments = arguments;
                if (syntax is ResponseFileQuoting responseQuoting)
                {
                    // 이 invocation만의 임시 디렉터리에 두므로 병렬 action끼리 이름이 겹치지 않고, 아래 임시 디렉터리 정리가 성공·실패·
                    // 취소 모두에서 함께 지운다(자식 정리가 끝나지 않으면 자식이 아직 읽을 수 있어 함께 보존한다).
                    string path = Path.Combine(temporaryRoot, NativeCommandLine.ResponseFileName);
                    if (stagedOutputs.Contains(path, NativeToolchains.PathComparer))
                    {
                        throw new InvalidOperationException($"Action {action.Id}: an output is named like the response file {path}.");
                    }

                    File.WriteAllBytes(path, ResponseFile.Encode(arguments, responseQuoting));
                    processArguments = ["@" + path];
                }

                process = await (runProcess ?? ProcessRunner.RunAsync)(new ProcessSpec(action.Tool,
                    processArguments, request.RootDirectory, request.Toolchain.Environment, MeasureUsage: request.MeasureProcesses), cancellationToken);
                usage = process.Usage;
            }

            cancellationToken.ThrowIfCancellationRequested();
            string? dependencyFailure = null;
            if (process.ExitCode == 0 && action.DependencyRecord is string dependencyRecord)
            {
                dependencyFailure = WriteDependencyRecord(request, action, inputs, depfile,
                    stagedOutputs[IndexOfOutput(action, dependencyRecord)]);
            }

            if (process.ExitCode != 0)
            {
                // 실패 안내문(리플렉션 생성 소스의 RMC3000 안내)은 도구 출력과 명령 정보 뒤에 붙인다.
                result = Result(action, true, false, process.ExitCode, process.StdOut,
                    process.StdErr + CommandDiagnostic(action, arguments, quoting, syntax is not null, false) +
                    (action.FailureNote is null ? "" : "\n" + action.FailureNote + "\n"));
            }
            else if (dependencyFailure is not null)
            {
                result = Result(action, true, false, -1, process.StdOut, process.StdErr + "\n" + dependencyFailure);
            }
            else if (stagedOutputs.Any(path => !File.Exists(path)))
            {
                result = Result(action, true, false, -1, process.StdOut, process.StdErr + "\nNative tool did not produce every declared output.");
            }
            else if (InputsChanged(action, inputs))
            {
                result = Result(action, true, false, -1, process.StdOut, process.StdErr + "\nBuild inputs changed during the action; outputs were not published.");
            }
            else
            {

                // dependency는 이 함수가 모든 출력을 게시한 뒤에만 실행된다. crash로 부분 교체가 남아도
                // cache index의 모든 output hash가 일치해야 hit이므로 부분 산출물을 성공으로 재사용하지 않는다. 의존 기록은 출력의
                // 하나라 object와 함께 게시된다(실패·취소한 실행은 기록을 남기지 않는다).
                PublishOutputs(action.Outputs, stagedOutputs, temporaryRoot);
                store.Publish(action, key, inputs);
                result = Result(action, true, true, 0, process.StdOut, process.StdErr);
            }
        }
        catch (ProcessCleanupException error)
        {
            // child가 여전히 이 경로를 사용할 수 있으므로 삭제나 재사용을 시도하지 않는다.
            store.Forget(action.Id);
            unsafeToDelete = true;
            if (error.InnerException is OperationCanceledException cancelled)
            {
                throw new OperationCanceledException(cancelled.Message +
                    $"\nTemporary output retained because process cleanup is incomplete: {temporaryRoot}. {error.Message}",
                    error, cancelled.CancellationToken);
            }

            result = Result(action, true, false, -1, "", error.ToString() +
                $"\nTemporary output retained because process cleanup is incomplete: {temporaryRoot}");
        }
        catch (Exception error) when (error is IOException or UnauthorizedAccessException or Win32Exception)
        {
            store.Forget(action.Id);
            result = Result(action, true, false, -1, "",
                error.Message + CommandDiagnostic(action, arguments, quoting, syntax is not null, error is Win32Exception));
        }
        catch (Exception error)
        {
            store.Forget(action.Id);
            pending = error;
        }

        if (!unsafeToDelete)
        {
            try
            {
                // 이 invocation이 직접 생성한 고유 하위 디렉터리만 제거한다.
                (deleteTemporaryRoot ?? (path => Directory.Delete(path, true)))(temporaryRoot);
            }
            catch (DirectoryNotFoundException) { }
            catch (Exception error) when (error is IOException or UnauthorizedAccessException)
            {
                store.Forget(action.Id);
                string diagnostic = $"Temporary output cleanup failed: {temporaryRoot}: {error.Message}";
                if (pending is OperationCanceledException cancelled)
                {
                    throw new OperationCanceledException(cancelled.Message + "\n" + diagnostic, cancelled, cancelled.CancellationToken);
                }

                if (pending is not null)
                {
                    throw new IOException(diagnostic, pending);
                }

                NativeActionResult original = result ?? throw new InvalidOperationException("Action result is missing.");
                result = original with
                {
                    Succeeded = false,
                    ExitCode = original.ExitCode == 0 ? -1 : original.ExitCode,
                    StdErr = original.StdErr + "\n" + diagnostic
                };
            }
        }

        if (pending is not null)
        {
            System.Runtime.ExceptionServices.ExceptionDispatchInfo.Capture(pending).Throw();
        }

        return result! with
        {
            DurationMilliseconds = (long)time.GetElapsedTime(started).TotalMilliseconds,
            CpuMilliseconds = usage is null ? 0 : (long)usage.CpuTime.TotalMilliseconds,
            PeakMemoryBytes = usage?.PeakWorkingSetBytes ?? 0,
            ProcessMeasured = usage is not null
        };
    }

    // action 임시 디렉터리 안의 depfile 이름이다(response file 이름과 같은 이유로 '+'를 쓴다).
    internal const string DepfileName = "dependencies+.d";

    private static int IndexOfOutput(NativeAction action, string output)
    {
        for (int index = 0; index < action.Outputs.Count; ++index)
        {
            if (NativeToolchains.PathComparer.Equals(action.Outputs[index], output))
            {
                return index;
            }
        }

        throw new InvalidOperationException($"Action {action.Id}: the dependency record {output} is not one of its outputs.");
    }

    // depfile을 읽어 의존 기록을 staged 출력으로 쓴다(Docs/Design/UnityBuildPch.ko.md 4.1절). 툴체인 소유 헤더는 뺀다(툴체인 key가
    // 대신한다). 이 action의 산출물(object 자신)과 이 plan의 다른 산출물은 넣지 않는다 — 산출물 입력은 선언 입력이다. 해시는 빌드의 입력
    // 메모에서 가져온다(빌드 시작 때의 내용 — 빌드 중 바뀌면 엔진의 InputSnapshot 비교가 이 실행의 기록을 버린다). 실패하면 사유를
    // 돌려주고 기록을 쓰지 않는다.
    private static string? WriteDependencyRecord(NativeBuildRequest request, NativeAction action, BuildInputs inputs, string depfile, string staged)
    {
        try
        {
            if (!File.Exists(depfile))
            {
                return "The compiler did not write the dependency file; outputs were not published.";
            }

            var entries = new List<DependencyEntry>();
            foreach (string path in Depfile.Parse(File.ReadAllText(depfile), request.RootDirectory))
            {
                if (inputs.IsToolchainOwned(path) || inputs.IsProduced(path))
                {
                    continue;
                }

                if (!File.Exists(path))
                {
                    return $"A dependency reported by the compiler no longer exists: {path}; outputs were not published.";
                }

                string hash = inputs.Content(path);
                inputs.NoteDependency(path, hash);
                entries.Add(new(path, hash));
            }

            File.WriteAllBytes(staged, DependencyRecord.Encode(entries));
            return null;
        }
        catch (Exception error) when (error is InvalidDataException or IOException or UnauthorizedAccessException)
        {
            return "Cannot record the compiler's dependencies: " + error.Message;
        }
    }

    // 이 action이 도는 동안 자기 입력 파일(번역 단위의 소스 등 — 다른 action의 산출물은 뺀다. 선행 action이 끝난 뒤에는 바뀌지 않는다)이
    // 바뀌었는가. 메모(빌드 시작 때의 내용)와 지금 내용을 대조한다. header의 변화는 엔진의 InputSnapshot 전후 비교가 본다.
    private static bool InputsChanged(NativeAction action, BuildInputs inputs)
    {
        foreach (string input in action.Inputs)
        {
            if (inputs.IsProduced(input))
            {
                continue;
            }

            if (!File.Exists(input) || Fingerprints.FileHash(input) != inputs.Content(input))
            {
                return true;
            }
        }

        return false;
    }

    private static void PublishOutputs(IReadOnlyList<string> outputs, IReadOnlyList<string> staged,
        string temporaryRoot)
    {
        var backups = new Dictionary<string, string>(NativeToolchains.PathComparer);
        var published = new List<string>();
        try
        {
            for (int index = 0; index < outputs.Count; ++index)
            {
                string output = outputs[index];
                Directory.CreateDirectory(Path.GetDirectoryName(output)!);
                if (File.Exists(output))
                {
                    string backup = Path.Combine(temporaryRoot, "backup-" + index);
                    File.Copy(output, backup);
                    backups.Add(output, backup);
                }
            }

            for (int index = 0; index < outputs.Count; ++index)
            {
                // 대상을 다른 핸들(백신·인덱서)이 열고 있어도 삭제 공유면 바꾸고, 실패하면 경로를 담아 알린다(FileReplace).
                FileReplace.Move(staged[index], outputs[index]);
                published.Add(outputs[index]);
            }
        }
        catch
        {
            foreach (string output in published)
            {
                if (backups.TryGetValue(output, out string? backup))
                {
                    FileReplace.Move(backup, output);
                }
                else
                {
                    File.Delete(output);
                }
            }

            throw;
        }
    }

    // 실패 진단에 더하는 명령 정보다. response file을 쓴 action은 @파일 대신 펼친 명령을(출력은 compile_commands.json처럼 최종
    // 경로로) 보인다. 그대로 넘긴 명령이 시작조차 못 했고 호스트 한계를 넘었으면 그 사실과 response file을 쓰지 않은 이유를 알린다
    // (종료 코드 실패에는 붙이지 않는다 — POSIX 기준은 실제 한계가 아니라 전환 기준이다). 그 밖에는 도구의 출력만 둔다.
    // executed는 실제로 실행한(staged 출력 경로의) 인자다.
    private static string CommandDiagnostic(NativeAction action, IReadOnlyList<string> executed, ResponseFileQuoting? quoting,
        bool responseFile, bool startFailed)
    {
        if (action.Kind == "Copy")
        {
            return "";
        }

        if (responseFile)
        {
            return $"\nCommand (arguments passed in {NativeCommandLine.ResponseFileName}; shown expanded with final output paths): " +
                NativeCommandLine.Display(action.Tool, action.ResolveArguments(action.Outputs, Path.ChangeExtension(action.Outputs[0], ".d")));
        }

        if (startFailed && NativeCommandLine.ExceedsHostLimit(action.Tool, executed))
        {
            string reason = quoting is null
                ? $"{Path.GetFileName(action.Tool)} has no known response file syntax"
                : "response files are disabled for this build";
            string size = OperatingSystem.IsWindows()
                ? $"{ResponseFile.WindowsCommandLineLength(action.Tool, executed)} characters"
                : $"{ResponseFile.PosixArgumentBytes(action.Tool, executed)} argument bytes";
            return $"\nThe command line is {size} ({NativeCommandLine.LimitText(action.Tool)}) and {reason}.";
        }

        return "";
    }

    private static NativeActionResult Result(NativeAction action, bool executed, bool succeeded,
        int exitCode, string stdout, string stderr) =>
        new(action.Id, action.Module, action.Kind, executed, succeeded, exitCode, stdout, stderr, action.Outputs[0]);
}
