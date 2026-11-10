using Rocks.Build.Cli;

namespace Rocks.Build.IntegrationTests;

internal static class MetadataOutputTransactionTests
{
    public static void Run()
    {
        SuccessAndNoOp();
        StagedFailurePublishesNothing();
        PartialWriteFailureRollsBack();
        CancellationAndManifestMutationRollBack();
        RollbackFailureRetainsRecoveryBackups();
        BackupNameCollisionDoesNotDeleteForeignFile();
        DuplicateAndAliasedOutputsAreRejected();
        Console.WriteLine("PASS metadata publication transaction: 7 groups");
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
            if (++calls == 1) cancellation.Cancel();
        };
        Expect<OperationCanceledException>(() => fixture.Publish(files, cancellation.Token));
        Require(File.ReadAllText(fixture.A) == "old-a" && File.ReadAllText(fixture.B) == "old-b", "cancel did not restore originals");

        calls = 0;
        files.Write = (path, bytes) =>
        {
            MetadataOutputFiles.Instance.WriteAtomic(path, bytes);
            if (++calls == 2) File.WriteAllText(fixture.Manifest, "changed-manifest");
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
            if (calls == 3) throw new IOException("rollback write failed");
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

    private static void DuplicateAndAliasedOutputsAreRejected()
    {
        using var fixture = new Fixture();
        fixture.Prepare();
        byte[] snapshot = File.ReadAllBytes(fixture.Manifest);
        Expect<ArgumentException>(() => MetadataOutputTransaction.Publish(
            [(fixture.StageA, fixture.A), (fixture.StageB, fixture.A)], fixture.Manifest, snapshot, CancellationToken.None));
        Expect<ArgumentException>(() => MetadataOutputTransaction.Publish(
            [(fixture.StageA, fixture.A), (fixture.A, fixture.B)], fixture.Manifest, snapshot, CancellationToken.None));
        Require(File.ReadAllText(fixture.A) == "old-a" && File.ReadAllText(fixture.B) == "old-b", "invalid paths changed output");
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
        try { action(); }
        catch (T error) { return error; }
        throw new InvalidOperationException("Expected " + typeof(T).Name);
    }
    private static void Require(bool condition, string message) { if (!condition) throw new InvalidOperationException(message); }

    private sealed class InjectFiles : IMetadataOutputFiles
    {
        public Action<string, byte[]>? Write { get; set; }
        public Action<string, byte[]>? Backup { get; set; }
        public bool Exists(string path) => MetadataOutputFiles.Instance.Exists(path);
        public byte[] Read(string path) => MetadataOutputFiles.Instance.Read(path);
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
        public void Publish(IMetadataOutputFiles? files = null, CancellationToken token = default) =>
            MetadataOutputTransaction.Publish([(StageA, A), (StageB, B)], Manifest,
                System.Text.Encoding.UTF8.GetBytes("expected-manifest"), token, files);
        public void Dispose() => Directory.Delete(Root, recursive: true);
    }
}
