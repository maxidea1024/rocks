using System.Runtime.CompilerServices;
using System.Runtime.ExceptionServices;
using Rocks.Tooling.Common;

[assembly: InternalsVisibleTo("Rocks.Build.IntegrationTests")]

namespace Rocks.Build.Cli;

// 복구 실패 후 기존 값이 있던 출력은 BackupPath에서 수동 복원할 수 있다.
// 원래 없던 출력의 BackupPath는 null이며 해당 파일을 제거해야 한다.
internal sealed record MetadataRecoveryBackup(string Target, string? BackupPath);

internal sealed class MetadataRollbackException : IOException
{
    public MetadataRollbackException(Exception cause, IReadOnlyList<string> failures,
        IReadOnlyList<MetadataRecoveryBackup> backups)
        : base("Metadata publication failed and rollback was incomplete. Recovery backups were retained. " +
            string.Join("; ", failures) + " | " + string.Join("; ", backups.Select(item =>
                item.BackupPath is null ? $"remove {item.Target}" : $"restore {item.BackupPath} -> {item.Target}")), cause)
    {
        Failures = failures;
        Backups = backups;
    }
    public IReadOnlyList<string> Failures { get; }
    public IReadOnlyList<MetadataRecoveryBackup> Backups { get; }
}

// 테스트는 실제 파일을 사용하면서 쓰기·삭제 지점에 오류를 주입할 수 있다.
internal interface IMetadataOutputFiles
{
    bool Exists(string path);
    byte[] Read(string path);
    byte[] ReadBounded(string path, int maximumBytes);
    void CreateBackup(string path, byte[] bytes);
    void WriteAtomic(string path, byte[] bytes);
    void Delete(string path);
}

internal sealed class MetadataOutputFiles : IMetadataOutputFiles
{
    public static MetadataOutputFiles Instance { get; } = new();
    public bool Exists(string path) => File.Exists(path);
    public byte[] Read(string path) => File.ReadAllBytes(path);
    public byte[] ReadBounded(string path, int maximumBytes) => BoundedFile.Read(path, maximumBytes);
    public void CreateBackup(string path, byte[] bytes)
    {
        bool created = false;
        try
        {
            using var stream = new FileStream(path, FileMode.CreateNew, FileAccess.Write, FileShare.None);
            created = true;
            stream.Write(bytes);
            stream.Flush(true);
        }
        catch (Exception error)
        {
            // CreateNew 자체가 충돌한 경우 다른 파일은 삭제하지 않는다.
            if (created)
            {
                try
                {
                    File.Delete(path);
                }
                catch (Exception cleanup)
                {
                    throw new IOException("Partial metadata backup could not be removed: " + path + ": " + cleanup.Message, error);
                }
            }

            throw;
        }
    }
    public void WriteAtomic(string path, byte[] bytes) => AtomicFile.WriteIfChanged(path, bytes);
    public void Delete(string path) => File.Delete(path);
}

internal static class MetadataOutputTransaction
{
    private sealed record Item(string Final, byte[] Generated, byte[]? Original, string? Backup);

    // 단일 writer가 출력과 명세를 관리한다는 전제다. 다른 프로세스의 경합을 OS 원자 CAS로
    // 막지 않으며, 강제 종료·전원 장애를 넘는 다중 파일 원자성은 제공하지 않는다.
    public static void Publish(IReadOnlyList<(string Staged, string Final)> outputs,
        string manifestPath, byte[] manifestSnapshot, CancellationToken cancellationToken,
        IMetadataOutputFiles? files = null)
    {
        ArgumentNullException.ThrowIfNull(outputs);
        ArgumentException.ThrowIfNullOrWhiteSpace(manifestPath);
        ArgumentNullException.ThrowIfNull(manifestSnapshot);
        if (manifestSnapshot.Length > BoundedFile.MaximumIdentityBytes)
        {
            throw new InvalidDataException("Identity manifest snapshot exceeds size limit.");
        }

        files ??= MetadataOutputFiles.Instance;
        string manifest = Path.GetFullPath(manifestPath);
        byte[] expectedManifest = manifestSnapshot.ToArray();
        StringComparer paths = OperatingSystem.IsWindows() ? StringComparer.OrdinalIgnoreCase : StringComparer.Ordinal;
        HashSet<string> finalPaths = new(paths);
        HashSet<string> stagePaths = new(paths);
        foreach ((string staged, string final) in outputs)
        {
            string stagePath = Path.GetFullPath(staged);
            string finalPath = Path.GetFullPath(final);
            if (!stagePaths.Add(stagePath) || !finalPaths.Add(finalPath) || paths.Equals(stagePath, finalPath) ||
                paths.Equals(stagePath, manifest) || paths.Equals(finalPath, manifest))
            {
                throw new ArgumentException("Metadata staging, output and manifest paths must be distinct.");
            }
        }

        if (stagePaths.Overlaps(finalPaths))
        {
            throw new ArgumentException("A metadata stage aliases another output.");
        }

        string tag = Guid.NewGuid().ToString("N");
        List<Item> items = [];
        List<string> createdBackups = [];
        List<Item> attempted = [];
        void CheckManifest()
        {
            cancellationToken.ThrowIfCancellationRequested();
            if (!files.Exists(manifest) || !files.ReadBounded(manifest, BoundedFile.MaximumIdentityBytes).AsSpan().SequenceEqual(expectedManifest))
            {
                throw new IOException("Identity manifest changed during metadata publication.");
            }
        }

        try
        {
            CheckManifest();
            int index = 0;
            foreach ((string staged, string final) in outputs)
            {
                cancellationToken.ThrowIfCancellationRequested();
                string source = Path.GetFullPath(staged);
                string target = Path.GetFullPath(final);
                byte[] generated = files.Read(source); // 하나라도 실패하면 어떤 출력도 게시하지 않는다.
                byte[]? old = files.Exists(target) ? files.Read(target) : null;
                string? backup = old is null ? null : target + ".rocks-backup-" + tag + "-" + index;
                items.Add(new(target, generated, old, backup));
                ++index;
            }

            foreach (Item item in items.Where(item => item.Backup is not null && !item.Generated.AsSpan().SequenceEqual(item.Original)))
            {
                CheckManifest();
                files.CreateBackup(item.Backup!, item.Original!);
                createdBackups.Add(item.Backup!);
            }

            foreach (Item item in items)
            {
                CheckManifest();
                bool stillExists = files.Exists(item.Final);
                if (stillExists != (item.Original is not null) ||
                    (stillExists && !files.Read(item.Final).AsSpan().SequenceEqual(item.Original)))
                {
                    throw new IOException("Metadata output changed before publication: " + item.Final);
                }

                if (item.Original is not null && item.Generated.AsSpan().SequenceEqual(item.Original))
                {
                    continue;
                }
                // 쓰기 함수가 변경 후 예외를 던질 수도 있으므로 호출 전에 복구 목록에 넣는다.
                attempted.Add(item);
                files.WriteAtomic(item.Final, item.Generated);
            }

            CheckManifest();
        }
        catch (Exception cause)
        {
            List<string> failures = [];
            for (int index = attempted.Count - 1; index >= 0; --index)
            {
                Item item = attempted[index];
                try
                {
                    if (item.Original is null)
                    {
                        files.Delete(item.Final);
                    }
                    else
                    {
                        files.WriteAtomic(item.Final, item.Original);
                    }
                }
                catch (Exception error)
                {
                    failures.Add($"{item.Final}: {error.Message}");
                }
            }

            if (failures.Count != 0)
            {
                var backups = attempted.Select(item => new MetadataRecoveryBackup(item.Final, item.Backup)).ToArray();
                throw new MetadataRollbackException(cause, failures, backups);
            }

            var cleanup = Cleanup(files, createdBackups);
            if (cleanup.Count != 0)
            {
                throw new IOException("Metadata publication failed; rollback succeeded but backup cleanup failed: " + string.Join("; ", cleanup), cause);
            }

            ExceptionDispatchInfo.Capture(cause).Throw();
            throw; // 도달 불가. 분석기가 정상 반환 경로로 취급하지 않게 한다.
        }

        var remaining = Cleanup(files, createdBackups);
        if (remaining.Count != 0)
        {
            throw new IOException("Metadata publication succeeded, but backup cleanup failed: " + string.Join("; ", remaining));
        }
    }

    private static List<string> Cleanup(IMetadataOutputFiles files, IReadOnlyList<string> paths)
    {
        List<string> failures = [];
        foreach (string path in paths)
        {
            try
            {
                files.Delete(path);
            }
            catch (Exception error)
            {
                failures.Add($"{path}: {error.Message}");
            }
        }

        return failures;
    }
}
