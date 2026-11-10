using System.ComponentModel;
using Rocks.Tooling.Common;

namespace Rocks.Build;

internal static class Executor
{
    public static async Task<NativeActionResult> RunAsync(NativeBuildRequest request, NativeAction action,
        string outputRoot, string toolchainKey, IncrementalStore store, CancellationToken cancellationToken)
    {
        cancellationToken.ThrowIfCancellationRequested();
        string key = action.Key(toolchainKey, outputRoot, request.RootDirectory);
        if (store.IsCurrent(action, key))
            return Result(action, false, true, 0, "", "");
        string temporaryRoot = Path.Combine(outputRoot, ".native-tmp", Guid.NewGuid().ToString("N"));
        Directory.CreateDirectory(temporaryRoot);
        string[] stagedOutputs = action.Outputs.Select(path => Path.Combine(temporaryRoot, Path.GetFileName(path))).ToArray();
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
                process = await ProcessRunner.RunAsync(new ProcessSpec(action.Tool,
                    action.ResolveArguments(stagedOutputs), request.RootDirectory, request.Toolchain.Environment), cancellationToken);
            }
            cancellationToken.ThrowIfCancellationRequested();
            if (process.ExitCode != 0)
                return Result(action, true, false, process.ExitCode, process.StdOut, process.StdErr);
            if (stagedOutputs.Any(path => !File.Exists(path)))
                return Result(action, true, false, -1, process.StdOut, process.StdErr + "\nNative tool did not produce every declared output.");
            if (action.Key(toolchainKey, outputRoot, request.RootDirectory) != key)
                return Result(action, true, false, -1, process.StdOut, process.StdErr + "\nBuild inputs changed during the action; outputs were not published.");

            // dependency는 이 함수가 모든 출력을 게시한 뒤에만 실행된다. crash로 부분 교체가 남아도
            // cache index의 모든 output hash가 일치해야 hit이므로 부분 산출물을 성공으로 재사용하지 않는다.
            PublishOutputs(action.Outputs, stagedOutputs, temporaryRoot);
            store.Publish(action, key);
            return Result(action, true, true, 0, process.StdOut, process.StdErr);
        }
        catch (Exception error) when (error is IOException or UnauthorizedAccessException or Win32Exception)
        {
            store.Forget(action.Id);
            return Result(action, true, false, -1, "", error.Message);
        }
        finally
        {
            // 이 invocation이 직접 생성한 고유 하위 디렉터리만 제거한다.
            if (Directory.Exists(temporaryRoot)) Directory.Delete(temporaryRoot, true);
        }
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
