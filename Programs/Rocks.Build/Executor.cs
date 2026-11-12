using System.ComponentModel;
using Rocks.Tooling.Common;

[assembly: System.Runtime.CompilerServices.InternalsVisibleTo("Rocks.Build.Tests")]

namespace Rocks.Build;

internal static class Executor
{
    public static async Task<NativeActionResult> RunAsync(NativeBuildRequest request, NativeAction action,
        string outputRoot, string toolchainKey, IncrementalStore store, CancellationToken cancellationToken,
        Func<ProcessSpec, CancellationToken, Task<ProcessResult>>? runProcess = null,
        Action<string>? deleteTemporaryRoot = null)
    {
        cancellationToken.ThrowIfCancellationRequested();
        string key = action.Key(toolchainKey, outputRoot, request.RootDirectory);
        if (store.IsCurrent(action, key))
            return Result(action, false, true, 0, "", "");
        string temporaryRoot = Path.Combine(outputRoot, ".native-tmp", Guid.NewGuid().ToString("N"));
        Directory.CreateDirectory(temporaryRoot);
        string[] stagedOutputs = action.Outputs.Select(path => Path.Combine(temporaryRoot, Path.GetFileName(path))).ToArray();
        NativeActionResult? result = null;
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
                process = await (runProcess ?? ProcessRunner.RunAsync)(new ProcessSpec(action.Tool,
                    action.ResolveArguments(stagedOutputs), request.RootDirectory, request.Toolchain.Environment), cancellationToken);
            }
            cancellationToken.ThrowIfCancellationRequested();
            if (process.ExitCode != 0)
                result = Result(action, true, false, process.ExitCode, process.StdOut, process.StdErr);
            else if (stagedOutputs.Any(path => !File.Exists(path)))
                result = Result(action, true, false, -1, process.StdOut, process.StdErr + "\nNative tool did not produce every declared output.");
            else if (action.Key(toolchainKey, outputRoot, request.RootDirectory) != key)
                result = Result(action, true, false, -1, process.StdOut, process.StdErr + "\nBuild inputs changed during the action; outputs were not published.");
            else
            {

            // dependency는 이 함수가 모든 출력을 게시한 뒤에만 실행된다. crash로 부분 교체가 남아도
            // cache index의 모든 output hash가 일치해야 hit이므로 부분 산출물을 성공으로 재사용하지 않는다.
                PublishOutputs(action.Outputs, stagedOutputs, temporaryRoot);
                store.Publish(action, key);
                result = Result(action, true, true, 0, process.StdOut, process.StdErr);
            }
        }
        catch (ProcessCleanupException error)
        {
            // child가 여전히 이 경로를 사용할 수 있으므로 삭제나 재사용을 시도하지 않는다.
            store.Forget(action.Id);
            unsafeToDelete = true;
            if (error.InnerException is OperationCanceledException cancelled)
                throw new OperationCanceledException(cancelled.Message +
                    $"\nTemporary output retained because process cleanup is incomplete: {temporaryRoot}. {error.Message}",
                    error, cancelled.CancellationToken);
            result = Result(action, true, false, -1, "", error.ToString() +
                $"\nTemporary output retained because process cleanup is incomplete: {temporaryRoot}");
        }
        catch (Exception error) when (error is IOException or UnauthorizedAccessException or Win32Exception)
        {
            store.Forget(action.Id);
            result = Result(action, true, false, -1, "", error.Message);
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
                    throw new OperationCanceledException(cancelled.Message + "\n" + diagnostic, cancelled, cancelled.CancellationToken);
                if (pending is not null)
                    throw new IOException(diagnostic, pending);
                NativeActionResult original = result ?? throw new InvalidOperationException("Action result is missing.");
                result = original with { Succeeded = false, ExitCode = original.ExitCode == 0 ? -1 : original.ExitCode,
                    StdErr = original.StdErr + "\n" + diagnostic };
            }
        }
        if (pending is not null) System.Runtime.ExceptionServices.ExceptionDispatchInfo.Capture(pending).Throw();
        return result!;
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
                File.Move(staged[index], outputs[index], true);
                published.Add(outputs[index]);
            }
        }
        catch
        {
            foreach (string output in published)
            {
                if (backups.TryGetValue(output, out string? backup)) File.Move(backup, output, true);
                else File.Delete(output);
            }
            throw;
        }
    }

    private static NativeActionResult Result(NativeAction action, bool executed, bool succeeded,
        int exitCode, string stdout, string stderr) =>
        new(action.Id, action.Module, action.Kind, executed, succeeded, exitCode, stdout, stderr, action.Outputs[0]);
}
