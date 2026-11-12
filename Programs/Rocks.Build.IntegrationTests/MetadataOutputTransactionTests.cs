using Rocks.Build.Cli;
using Rocks.Tooling.Common;

namespace Rocks.Build.IntegrationTests;

internal static class MetadataOutputTransactionTests
{
    public static void Run()
    {
        SuccessAndNoOp();
        StagedFailurePublishesNothing();
        PartialWriteFailureRollsBack();
        CancellationAndManifestMutationRollBack();
        GrowingManifestRollsBack();
        RollbackFailureRetainsRecoveryBackups();
        BackupNameCollisionDoesNotDeleteForeignFile();
        DuplicateAndAliasedOutputsAreRejected();
        DeletionsPublishAndRollBack();
        EveryManifestIsCheckedDuringPublication();
        BackupDirectoryKeepsBackupsOutsideOutputs();
        Console.WriteLine("PASS metadata publication transaction: 11 groups");
    }

    private static void SuccessAndNoOp()
    {
        using var fixture = new Fixture();
        File.WriteAllText(fixture.A, "old-a");
        File.WriteAllText(fixture.B, "same-b");
        File.WriteAllText(fixture.StageA, "new-a");
        File.WriteAllText(fixture.StageB, "same-b");
        File.SetLastWriteTimeUtc(fixture.B, DateTime.UnixEpoch.AddSeconds(1000000000));
        DateTime timestamp = File.GetLastWriteTimeUtc(fixture.B);
        fixture.Publish();
        Require(File.ReadAllText(fixture.A) == "new-a" && File.ReadAllText(fixture.B) == "same-b", "successful output");
        Require(File.GetLastWriteTimeUtc(fixture.B) == timestamp, "identical output rewritten");
        Require(!Directory.EnumerateFiles(fixture.Root, "*.rocks-backup-*").Any(), "successful backup leaked");
    }

    private static void StagedFailurePublishesNothing()
    {
        using var fixture = new Fixture();
        File.WriteAllText(fixture.A, "old-a");
        File.WriteAllText(fixture.StageA, "new-a");
        Expect<FileNotFoundException>(() => fixture.Publish());
        Require(File.ReadAllText(fixture.A) == "old-a" && !File.Exists(fixture.B), "preflight failure modified output");
    }

    private static void PartialWriteFailureRollsBack()
    {
        using var fixture = new Fixture();
        fixture.Prepare();
        File.Delete(fixture.B); // 원래 존재하지 않는 경우까지 복구한다.
        var files = new InjectFiles();
        int calls = 0;
        files.Write = (path, bytes) =>
        {
            if (++calls == 2)
            {
                File.WriteAllText(path, "partial replacement");
                throw new IOException("injected after replace");
            }

            MetadataOutputFiles.Instance.WriteAtomic(path, bytes);
        };
        Expect<IOException>(() => fixture.Publish(files));
        Require(File.ReadAllText(fixture.A) == "old-a" && !File.Exists(fixture.B), "partial replacement was not fully rolled back");
        Require(calls == 3, "existing output was not restored");
        Require(!Directory.EnumerateFiles(fixture.Root, "*.rocks-backup-*").Any(), "rollback backup leaked");
    }

    private static void CancellationAndManifestMutationRollBack()
    {
        using var fixture = new Fixture();
        fixture.Prepare();
        using var cancellation = new CancellationTokenSource();
        var files = new InjectFiles();
        int calls = 0;
        files.Write = (path, bytes) =>
        {
            MetadataOutputFiles.Instance.WriteAtomic(path, bytes);
            if (++calls == 1)
            {
                cancellation.Cancel();
            }
        };
        Expect<OperationCanceledException>(() => fixture.Publish(files, cancellation.Token));
        Require(File.ReadAllText(fixture.A) == "old-a" && File.ReadAllText(fixture.B) == "old-b", "cancel did not restore originals");

        calls = 0;
        files.Write = (path, bytes) =>
        {
            MetadataOutputFiles.Instance.WriteAtomic(path, bytes);
            if (++calls == 2)
            {
                File.WriteAllText(fixture.Manifest, "changed-manifest");
            }
        };
        Expect<IOException>(() => fixture.Publish(files));
        Require(File.ReadAllText(fixture.A) == "old-a" && File.ReadAllText(fixture.B) == "old-b", "manifest change did not roll back");
        Require(File.ReadAllText(fixture.Manifest) == "changed-manifest", "external manifest edit overwritten");
    }

    private static void RollbackFailureRetainsRecoveryBackups()
    {
        using var fixture = new Fixture();
        fixture.Prepare();
        var files = new InjectFiles();
        int calls = 0;
        files.Write = (path, bytes) =>
        {
            ++calls;
            if (calls == 2)
            {
                File.WriteAllText(path, "partial replacement");
                throw new IOException("original write failed");
            }

            if (calls == 3)
            {
                throw new IOException("rollback write failed");
            }

            MetadataOutputFiles.Instance.WriteAtomic(path, bytes);
        };
        MetadataRollbackException failure = Expect<MetadataRollbackException>(() => fixture.Publish(files));
        Require(failure.InnerException?.Message.Contains("original write failed", StringComparison.Ordinal) == true,
            "original failure not preserved");
        Require(failure.Failures.Any(value => value.Contains("rollback write failed", StringComparison.Ordinal)), "rollback failure not recorded");
        Require(File.ReadAllText(fixture.A) == "old-a", "rollback stopped before restoring other outputs");
        var backup = failure.Backups.Single(value => value.Target == fixture.B);
        Require(backup.BackupPath is not null && File.ReadAllText(backup.BackupPath) == "old-b", "recovery backup missing or wrong");
        Require(File.ReadAllText(fixture.B) == "partial replacement", "failure injection did not mutate output");
    }

    // 백업 디렉터리를 주면 백업은 출력 옆(생성 include 디렉터리)이 아니라 그 디렉터리에 생기고, 성공하면 디렉터리째
    // 지워진다. 복구가 불완전하면 그 디렉터리의 백업을 알려 준다.
    private static void BackupDirectoryKeepsBackupsOutsideOutputs()
    {
        using var fixture = new Fixture();
        fixture.Prepare();
        string backups = Path.Combine(fixture.Root, "backup-parent", "tag");
        fixture.Publish(backupDirectory: backups);
        Require(File.ReadAllText(fixture.A) == "new-a" && File.ReadAllText(fixture.B) == "new-b", "publication with backup directory");
        Require(!Directory.Exists(Path.Combine(fixture.Root, "backup-parent")), "successful publication left its backup directory");
        Require(!Directory.EnumerateFiles(fixture.Root, "*.rocks-backup*").Any(), "backup was written next to an output");

        fixture.Prepare();
        var files = new InjectFiles();
        int calls = 0;
        files.Write = (path, bytes) =>
        {
            ++calls;
            if (calls == 2)
            {
                throw new IOException("original write failed");
            }

            if (calls == 3)
            {
                throw new IOException("rollback write failed");
            }

            MetadataOutputFiles.Instance.WriteAtomic(path, bytes);
        };
        MetadataRollbackException failure = Expect<MetadataRollbackException>(() => fixture.Publish(files, backupDirectory: backups));
        var backup = failure.Backups.Single(value => value.Target == fixture.A);
        Require(backup.BackupPath is not null && Path.GetDirectoryName(backup.BackupPath) == Path.GetFullPath(backups) &&
            File.ReadAllText(backup.BackupPath) == "old-a", "recovery backup is not in the backup directory");
    }

    private static void GrowingManifestRollsBack()
    {
        using var fixture = new Fixture();
        fixture.Prepare();
        var files = new InjectFiles();
        int calls = 0;
        files.Write = (path, bytes) =>
        {
            MetadataOutputFiles.Instance.WriteAtomic(path, bytes);
            if (++calls == 1)
            {
                using var manifest = new FileStream(fixture.Manifest, FileMode.Open, FileAccess.Write, FileShare.None);
                manifest.SetLength((long)BoundedFile.MaximumIdentityBytes + 1);
            }
        };
        Expect<InvalidDataException>(() => fixture.Publish(files));
        Require(File.ReadAllText(fixture.A) == "old-a" && File.ReadAllText(fixture.B) == "old-b",
            "growing manifest did not roll back outputs");
        Require(new FileInfo(fixture.Manifest).Length == (long)BoundedFile.MaximumIdentityBytes + 1,
            "external manifest mutation was overwritten");
    }

    private static void DuplicateAndAliasedOutputsAreRejected()
    {
        using var fixture = new Fixture();
        fixture.Prepare();
        byte[] snapshot = File.ReadAllBytes(fixture.Manifest);
        Expect<ArgumentException>(() => MetadataOutputTransaction.Publish(
            [(fixture.StageA, fixture.A), (fixture.StageB, fixture.A)], [(fixture.Manifest, snapshot)], CancellationToken.None));
        Expect<ArgumentException>(() => MetadataOutputTransaction.Publish(
            [(fixture.StageA, fixture.A), (fixture.A, fixture.B)], [(fixture.Manifest, snapshot)], CancellationToken.None));
        Expect<ArgumentException>(() => MetadataOutputTransaction.Publish(
            [(fixture.StageA, fixture.A), (null, fixture.A)], [(fixture.Manifest, snapshot)], CancellationToken.None));
        Require(File.ReadAllText(fixture.A) == "old-a" && File.ReadAllText(fixture.B) == "old-b", "invalid paths changed output");
    }

    // 삭제 항목(입력에서 빠진 헤더의 생성물)은 게시와 함께 지워지고, 실패하면 되살아난다.
    private static void DeletionsPublishAndRollBack()
    {
        using var fixture = new Fixture();
        fixture.Prepare();
        byte[] snapshot = File.ReadAllBytes(fixture.Manifest);
        MetadataOutputTransaction.Publish([(fixture.StageA, fixture.A), (null, fixture.B)], [(fixture.Manifest, snapshot)], CancellationToken.None);
        Require(File.ReadAllText(fixture.A) == "new-a" && !File.Exists(fixture.B), "deletion item was not published");
        Require(!Directory.EnumerateFiles(fixture.Root, "*.rocks-backup-*").Any(), "deletion backup leaked");

        using var failing = new Fixture();
        failing.Prepare();
        // 삭제 뒤 첫 쓰기만 실패시킨다(롤백의 복원 쓰기는 성공해야 한다).
        bool failed = false;
        var files = new InjectFiles
        {
            Write = (path, bytes) =>
            {
                if (!failed)
                {
                    failed = true;
                    throw new IOException("injected write failure after deletion");
                }

                MetadataOutputFiles.Instance.WriteAtomic(path, bytes);
            }
        };
        byte[] failingSnapshot = File.ReadAllBytes(failing.Manifest);
        Expect<IOException>(() => MetadataOutputTransaction.Publish([(null, failing.B), (failing.StageA, failing.A)],
            [(failing.Manifest, failingSnapshot)], CancellationToken.None, files));
        Require(File.ReadAllText(failing.A) == "old-a" && File.ReadAllText(failing.B) == "old-b", "deletion was not rolled back");
    }

    // 실행이 읽은 모든 명세를 게시 동안 확인한다. 하나라도 바뀌면 전체를 되돌린다.
    private static void EveryManifestIsCheckedDuringPublication()
    {
        using var fixture = new Fixture();
        fixture.Prepare();
        string second = Path.Combine(fixture.Root, "second.rocksids.json");
        File.WriteAllText(second, "second-manifest");
        byte[] first = File.ReadAllBytes(fixture.Manifest);
        byte[] secondSnapshot = File.ReadAllBytes(second);
        var files = new InjectFiles();
        files.Write = (path, bytes) =>
        {
            MetadataOutputFiles.Instance.WriteAtomic(path, bytes);
            File.WriteAllText(second, "edited-second-manifest");
        };
        Expect<IOException>(() => MetadataOutputTransaction.Publish([(fixture.StageA, fixture.A), (fixture.StageB, fixture.B)],
            [(fixture.Manifest, first), (second, secondSnapshot)], CancellationToken.None, files));
        Require(File.ReadAllText(fixture.A) == "old-a" && File.ReadAllText(fixture.B) == "old-b", "second manifest edit did not roll back");
        Require(File.ReadAllText(second) == "edited-second-manifest", "external manifest edit overwritten");
    }

    private static void BackupNameCollisionDoesNotDeleteForeignFile()
    {
        using var fixture = new Fixture();
        fixture.Prepare();
        var files = new InjectFiles();
        string? collided = null;
        files.Backup = (path, _) =>
        {
            collided = path;
            File.WriteAllText(path, "foreign-file");
            throw new IOException("injected create-new collision");
        };
        Expect<IOException>(() => fixture.Publish(files));
        Require(collided is not null && File.ReadAllText(collided) == "foreign-file", "unowned backup path was deleted");
        Require(File.ReadAllText(fixture.A) == "old-a" && File.ReadAllText(fixture.B) == "old-b", "backup failure changed outputs");
    }

    private static T Expect<T>(Action action) where T : Exception
    {
        try
        {
            action();
        }
        catch (T error)
        {
            return error;
        }

        throw new InvalidOperationException("Expected " + typeof(T).Name);
    }

    private static void Require(bool condition, string message)
    {
        if (!condition)
        {
            throw new InvalidOperationException(message);
        }
    }

    private sealed class InjectFiles : IMetadataOutputFiles
    {
        public Action<string, byte[]>? Write { get; set; }

        public Action<string, byte[]>? Backup { get; set; }

        public bool Exists(string path) => MetadataOutputFiles.Instance.Exists(path);

        public byte[] Read(string path) => MetadataOutputFiles.Instance.Read(path);

        public byte[] ReadBounded(string path, int maximumBytes) => MetadataOutputFiles.Instance.ReadBounded(path, maximumBytes);

        public void CreateBackup(string path, byte[] bytes) => (Backup ?? MetadataOutputFiles.Instance.CreateBackup)(path, bytes);

        public void WriteAtomic(string path, byte[] bytes) => (Write ?? MetadataOutputFiles.Instance.WriteAtomic)(path, bytes);

        public void Delete(string path) => MetadataOutputFiles.Instance.Delete(path);
    }

    private sealed class Fixture : IDisposable
    {
        public Fixture()
        {
            Root = Directory.CreateTempSubdirectory("rocks-meta-transaction-").FullName;
            File.WriteAllText(Manifest, "expected-manifest");
        }

        public string Root { get; }

        public string Manifest => Path.Combine(Root, "input.rocksids.json");

        public string A => Path.Combine(Root, "a.generated.h");

        public string B => Path.Combine(Root, "b.generated.h");

        public string StageA => Path.Combine(Root, "stage-a.generated.h");

        public string StageB => Path.Combine(Root, "stage-b.generated.h");

        public void Prepare()
        {
            File.WriteAllText(A, "old-a");
            File.WriteAllText(B, "old-b");
            File.WriteAllText(StageA, "new-a");
            File.WriteAllText(StageB, "new-b");
        }

        public void Publish(IMetadataOutputFiles? files = null, CancellationToken token = default, string? backupDirectory = null) =>
            MetadataOutputTransaction.Publish([(StageA, A), (StageB, B)],
                [(Manifest, System.Text.Encoding.UTF8.GetBytes("expected-manifest"))], token, files, backupDirectory);

        public void Dispose() => Directory.Delete(Root, recursive: true);
    }
}
