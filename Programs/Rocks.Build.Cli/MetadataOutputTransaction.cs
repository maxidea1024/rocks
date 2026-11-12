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
    // Generated가 null이면 삭제 항목이다(입력에서 빠진 헤더의 생성물·잉여 파일).
    private sealed record Item(string Final, byte[]? Generated, byte[]? Original, string? Backup);

    // 모든 모듈의 생성물과 삭제를 한 번에 게시한다. outputs의 Staged가 null이면 Final을 지운다.
    // manifests는 실행이 읽은 identity 명세와 그 시점 내용이며 게시 동안 하나라도 바뀌면 전체를 되돌린다.
    // backupDirectory를 주면 바꾸거나 지울 출력의 백업을 그 디렉터리(호출마다 새 경로)에 두고, 없으면 출력 옆에 둔다.
    // 백업은 성공·완전한 복구 뒤 지우며, 복구가 불완전하면 MetadataRollbackException이 남은 백업을 알려 준다.
    // 단일 writer가 출력과 명세를 관리한다는 전제다. 다른 프로세스의 경합을 OS 원자 CAS로
    // 막지 않으며, 강제 종료·전원 장애를 넘는 다중 파일 원자성은 제공하지 않는다.
    public static void Publish(IReadOnlyList<(string? Staged, string Final)> outputs,
        IReadOnlyList<(string Path, byte[] Snapshot)> manifests, CancellationToken cancellationToken,
        IMetadataOutputFiles? files = null, string? backupDirectory = null)
    {
        ArgumentNullException.ThrowIfNull(outputs);
        ArgumentNullException.ThrowIfNull(manifests);
        foreach ((string manifestPath, byte[] snapshot) in manifests)
        {
            ArgumentException.ThrowIfNullOrWhiteSpace(manifestPath);
            ArgumentNullException.ThrowIfNull(snapshot);
            if (snapshot.Length > BoundedFile.MaximumIdentityBytes)
            {
                throw new InvalidDataException("Identity manifest snapshot exceeds size limit.");
            }
        }

        files ??= MetadataOutputFiles.Instance;
        StringComparer paths = OperatingSystem.IsWindows() ? StringComparer.OrdinalIgnoreCase : StringComparer.Ordinal;
        List<(string Path, byte[] Expected)> expectedManifests = manifests
            .Select(manifest => (Path.GetFullPath(manifest.Path), manifest.Snapshot.ToArray())).ToList();
        HashSet<string> manifestPaths = new(expectedManifests.Select(manifest => manifest.Path), paths);
        HashSet<string> finalPaths = new(paths);
        HashSet<string> stagePaths = new(paths);
        foreach ((string? staged, string final) in outputs)
        {
            string finalPath = Path.GetFullPath(final);
            if (!finalPaths.Add(finalPath) || manifestPaths.Contains(finalPath))
            {
                throw new ArgumentException("Metadata staging, output and manifest paths must be distinct.");
            }

            if (staged is not null)
            {
                string stagePath = Path.GetFullPath(staged);
                if (!stagePaths.Add(stagePath) || paths.Equals(stagePath, finalPath) || manifestPaths.Contains(stagePath))
                {
                    throw new ArgumentException("Metadata staging, output and manifest paths must be distinct.");
                }
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
        void CheckManifests()
        {
            cancellationToken.ThrowIfCancellationRequested();
            foreach ((string manifest, byte[] expected) in expectedManifests)
            {
                if (!files.Exists(manifest) || !files.ReadBounded(manifest, BoundedFile.MaximumIdentityBytes).AsSpan().SequenceEqual(expected))
                {
                    throw new IOException("Identity manifest changed during metadata publication: " + manifest);
                }
            }
        }

        bool Unchanged(Item item) =>
            item.Generated is null ? item.Original is null : item.Original is not null && item.Generated.AsSpan().SequenceEqual(item.Original);

        try
        {
            CheckManifests();
            int index = 0;
            foreach ((string? staged, string final) in outputs)
            {
                cancellationToken.ThrowIfCancellationRequested();
                string target = Path.GetFullPath(final);
                byte[]? generated = staged is null ? null : files.Read(Path.GetFullPath(staged)); // 하나라도 실패하면 어떤 출력도 게시하지 않는다.
                byte[]? old = files.Exists(target) ? files.Read(target) : null;
                string? backup = old is null ? null
                    : backupDirectory is null ? target + ".rocks-backup-" + tag + "-" + index
                    : Path.Combine(Path.GetFullPath(backupDirectory), index + ".rocks-backup");
                items.Add(new(target, generated, old, backup));
                ++index;
            }

            if (backupDirectory is not null && items.Any(item => item.Backup is not null && !Unchanged(item)))
            {
                Directory.CreateDirectory(backupDirectory);
            }

            foreach (Item item in items.Where(item => item.Backup is not null && !Unchanged(item)))
            {
                CheckManifests();
                files.CreateBackup(item.Backup!, item.Original!);
                createdBackups.Add(item.Backup!);
            }

            foreach (Item item in items)
            {
                CheckManifests();
                bool stillExists = files.Exists(item.Final);
                if (stillExists != (item.Original is not null) ||
                    (stillExists && !files.Read(item.Final).AsSpan().SequenceEqual(item.Original)))
                {
                    throw new IOException("Metadata output changed before publication: " + item.Final);
                }

                if (Unchanged(item))
                {
                    continue;
                }

                // 쓰기 함수가 변경 후 예외를 던질 수도 있으므로 호출 전에 복구 목록에 넣는다.
                attempted.Add(item);
                if (item.Generated is null)
                {
                    files.Delete(item.Final);
                }
                else
                {
                    files.WriteAtomic(item.Final, item.Generated);
                }
            }

            CheckManifests();
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

            var cleanup = Cleanup(files, createdBackups, backupDirectory);
            if (cleanup.Count != 0)
            {
                throw new IOException("Metadata publication failed; rollback succeeded but backup cleanup failed: " + string.Join("; ", cleanup), cause);
            }

            ExceptionDispatchInfo.Capture(cause).Throw();
            throw; // 도달 불가. 분석기가 정상 반환 경로로 취급하지 않게 한다.
        }

        var remaining = Cleanup(files, createdBackups, backupDirectory);
        if (remaining.Count != 0)
        {
            throw new IOException("Metadata publication succeeded, but backup cleanup failed: " + string.Join("; ", remaining));
        }
    }

    private static List<string> Cleanup(IMetadataOutputFiles files, IReadOnlyList<string> paths, string? directory)
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

        // 이 호출의 백업 디렉터리는 비었을 때만 지운다(다른 파일이 있으면 남긴다).
        if (failures.Count == 0 && directory is not null && Directory.Exists(directory) && !Directory.EnumerateFileSystemEntries(directory).Any())
        {
            try
            {
                Directory.Delete(directory);
                string? parent = Path.GetDirectoryName(Path.GetFullPath(directory));
                if (parent is not null && Directory.Exists(parent) && !Directory.EnumerateFileSystemEntries(parent).Any())
                {
                    Directory.Delete(parent);
                }
            }
            catch (IOException error)
            {
                failures.Add($"{directory}: {error.Message}");
            }
        }

        return failures;
    }
}
