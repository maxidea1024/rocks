using System.Diagnostics;
using System.Security.Cryptography;
using System.Text;
using System.Text.Json;

namespace Rocks.Tooling.Common;

/// <summary>생성물/캐시를 제외한 저장소 입력의 content snapshot이다. Git 상태와 별도로 비교한다.</summary>
public sealed record SourceSnapshot(string Algorithm, string Digest, int FileCount)
{
    public static SourceSnapshot Capture(string root)
    {
        root = Path.GetFullPath(root);
        if (!Directory.Exists(root)) throw new DirectoryNotFoundException(root);
        List<string> files = [];
        foreach (string directory in new[] { "Engine", "Tests", "Build", "Tools", "Templates", ".github" })
            if (Directory.Exists(Path.Combine(root, directory))) Visit(Path.Combine(root, directory));
        foreach (string name in new[] { "CMakeLists.txt", "CMakePresets.json", ".clang-format", ".editorconfig",
            "global.json", "NuGet.Config", "NuGet.config", "Rocks.slnx", "Rocks.proj", "Directory.Build.props", "Directory.Build.targets", "Directory.Build.rsp" })
            if (File.Exists(Path.Combine(root, name))) files.Add(Path.Combine(root, name));
        files = files.Distinct(OperatingSystem.IsWindows() ? StringComparer.OrdinalIgnoreCase : StringComparer.Ordinal)
            .OrderBy(path => Relative(path), StringComparer.Ordinal).ToList();
        using IncrementalHash digest = IncrementalHash.CreateHash(HashAlgorithmName.SHA256);
        foreach (string path in files)
        {
            digest.AppendData(Encoding.UTF8.GetBytes(Relative(path) + "\0"));
            using FileStream input = File.OpenRead(path);
            digest.AppendData(SHA256.HashData(input));
        }
        return new("sha256(path-NUL-sha256(file))", Convert.ToHexStringLower(digest.GetHashAndReset()), files.Count);

        string Relative(string path) => Path.GetRelativePath(root, path).Replace('\\', '/');
        void Visit(string directory)
        {
            foreach (string entry in Directory.EnumerateFileSystemEntries(directory))
            {
                string relative = Relative(entry);
                string[] parts = relative.Split('/');
                if (parts.Any(part => part is "bin" or "obj" or "__pycache__") ||
                    (parts.Length >= 2 && parts[0] == "Build" && parts[1] is "Artifacts" or "Cache"))
                    continue;
                FileAttributes attributes = File.GetAttributes(entry);
                if ((attributes & FileAttributes.ReparsePoint) != 0)
                    throw new InvalidDataException("Source snapshot does not follow linked inputs: " + relative);
                if ((attributes & FileAttributes.Directory) != 0) Visit(entry); else files.Add(entry);
            }
        }
    }
}

public sealed record GitSnapshot(string? Head, bool? Dirty, IReadOnlyList<string> DirtyEntries,
    string? HeadError = null, string? StatusError = null);
public sealed record EvidenceStep(string Name, IReadOnlyList<string> Command, int ExitCode,
    long DurationNs, string Status, bool Tests, string? Error = null);

public sealed class BuildEvidenceDocument
{
    public int Schema { get; init; } = 2;
    public required string Preset { get; init; }
    public required string Platform { get; init; }
    public required string Architecture { get; set; }
    public ulong Seed { get; set; } = 7351;
    public long StartedUnixNs { get; init; }
    public long FinishedUnixNs { get; set; }
    public required SourceSnapshot SourceBefore { get; init; }
    public SourceSnapshot? SourceAfter { get; set; }
    public required GitSnapshot GitBefore { get; init; }
    public GitSnapshot? GitAfter { get; set; }
    public List<EvidenceStep> Steps { get; } = [];
    public string? Configuration { get; set; }
    public string? Target { get; set; }
    public string? Sanitizer { get; set; }
    public string? Compiler { get; set; }
    public string? CompilerVersion { get; set; }
    public bool SourceStableDuringRun { get; set; }
    public string Status { get; set; } = "NOT RUN";
    public string TestsStatus { get; set; } = "NOT RUN";
    public string? Error { get; set; }
    public string? RunnerReport { get; set; }
    public bool RunnerReportCurrent { get; set; }
    public string? RunnerReportSha256 { get; set; }
    public string? RunnerReportError { get; set; }
}

/// <summary>
/// 순차 호출하는 한 build/test 실행의 증거다. 네이티브 엔진의 병렬 작업은 종료 후 RecordStep으로
/// 실제 결과를 전달한다. 실패 step과 source 변경은 caller가 error를 생략해도 성공으로 기록하지 않는다.
/// 전체 환경 변수나 자격 증명은 보고서에 기록하지 않는다.
/// </summary>
public sealed class BuildEvidence
{
    private readonly string root;
    private readonly IReadOnlyDictionary<string, string?>? environment;
    private readonly BuildEvidenceDocument document;
    private string? runnerReport;
    private long testsStartedUnixNs;
    private bool finished;
    private static readonly JsonSerializerOptions Json = new() { PropertyNamingPolicy = JsonNamingPolicy.CamelCase, WriteIndented = true };

    private BuildEvidence(string root, string preset, IReadOnlyDictionary<string, string?>? environment,
        SourceSnapshot source, GitSnapshot git)
    {
        this.root = root;
        this.environment = environment;
        OutputDirectory = Path.Combine(root, "out", preset);
        document = new() { Preset = preset, Platform = OperatingSystem.IsWindows() ? "Windows" :
            OperatingSystem.IsMacOS() ? "Darwin" : OperatingSystem.IsLinux() ? "Linux" : "Unknown",
            Architecture = System.Runtime.InteropServices.RuntimeInformation.ProcessArchitecture.ToString(),
            StartedUnixNs = UnixNanoseconds(DateTime.UtcNow), SourceBefore = source, GitBefore = git };
    }

    public string OutputDirectory { get; }
    public string EvidencePath => Path.Combine(OutputDirectory, "build-evidence.json");

    public static async Task<BuildEvidence> StartAsync(string root, string preset,
        IReadOnlyDictionary<string, string?>? environment = null, CancellationToken cancellationToken = default)
    {
        if (string.IsNullOrEmpty(preset) || Path.IsPathRooted(preset) ||
            preset.Replace('\\', '/').Split('/').Any(part => part is "" or "." or ".." ||
                part.Any(character => !(char.IsAsciiLetterOrDigit(character) || character is '-' or '_' or '.'))))
            throw new ArgumentException("Preset must be a safe path relative to root/out", nameof(preset));
        preset = preset.Replace('\\', '/');
        root = Path.GetFullPath(root);
        SourceSnapshot source = SourceSnapshot.Capture(root);
        GitSnapshot git = await CaptureGitAsync(root, environment, cancellationToken);
        return new(root, preset, environment, source, git);
    }

    public void SetConfiguration(string configuration, string target, string? sanitizer,
        string compiler, string? compilerVersion = null, string? architecture = null, ulong seed = 7351)
    {
        EnsureOpen();
        document.Configuration = configuration; document.Target = target; document.Sanitizer = sanitizer;
        document.Compiler = compiler; document.CompilerVersion = compilerVersion; document.Seed = seed;
        if (architecture is not null) document.Architecture = architecture;
    }

    /// <summary>직전 실행 report를 제거한다. 이 evidence의 out/preset 내부 파일만 지정할 수 있다.</summary>
    public void BeginTests(string? reportPath = null)
    {
        EnsureOpen();
        if (testsStartedUnixNs != 0) throw new InvalidOperationException("Tests already started for this evidence");
        string output = Path.GetFullPath(OutputDirectory);
        runnerReport = Path.GetFullPath(reportPath ?? Path.Combine(output, "foundation-report.json"));
        string prefix = Path.TrimEndingDirectorySeparator(output) + Path.DirectorySeparatorChar;
        if (!runnerReport.StartsWith(prefix, OperatingSystem.IsWindows() ? StringComparison.OrdinalIgnoreCase : StringComparison.Ordinal))
            throw new ArgumentException("Runner report must remain inside this evidence output directory", nameof(reportPath));
        if (Directory.Exists(runnerReport)) throw new IOException("Runner report path is a directory");
        if (File.Exists(runnerReport))
        {
            if ((File.GetAttributes(runnerReport) & FileAttributes.ReparsePoint) != 0)
                throw new IOException("Runner report must not be a symbolic link");
            File.Delete(runnerReport);
        }
        testsStartedUnixNs = UnixNanoseconds(DateTime.UtcNow);
    }

    public async Task<ProcessResult> RunAsync(ProcessSpec spec, bool tests = false,
        CancellationToken cancellationToken = default)
    {
        EnsureOpen();
        if (tests && testsStartedUnixNs == 0) BeginTests();
        long started = Stopwatch.GetTimestamp();
        try
        {
            ProcessResult result = await ProcessRunner.RunAsync(spec, cancellationToken);
            RecordStep(Path.GetFileName(spec.FileName), [spec.FileName, .. spec.Arguments], result.ExitCode,
                Stopwatch.GetElapsedTime(started), tests);
            return result;
        }
        catch (Exception error) when (error is not OutOfMemoryException)
        {
            RecordStep(Path.GetFileName(spec.FileName), [spec.FileName, .. spec.Arguments], -1,
                Stopwatch.GetElapsedTime(started), tests, error.Message);
            throw;
        }
    }

    public void RecordStep(string name, IReadOnlyList<string> command, int exitCode, TimeSpan duration,
        bool tests = false, string? error = null)
    {
        EnsureOpen();
        if (duration < TimeSpan.Zero) throw new ArgumentOutOfRangeException(nameof(duration));
        document.Steps.Add(new(name, command.ToArray(), exitCode, checked(duration.Ticks * 100),
            exitCode == 0 && error is null ? "PASS" : "FAIL", tests, error));
    }

    /// <summary>항상 실제 실패 상태를 저장한다. 반환 Status를 확인하고 CLI 종료 코드를 정한다.</summary>
    public async Task<BuildEvidenceDocument> FinishAsync(string? error = null,
        CancellationToken cancellationToken = default)
    {
        EnsureOpen();
        finished = true;
        string? snapshotError = null;
        try
        {
            document.SourceAfter = SourceSnapshot.Capture(root);
            document.SourceStableDuringRun = document.SourceBefore == document.SourceAfter;
        }
        catch (Exception captureError) when (captureError is IOException or UnauthorizedAccessException)
        {
            document.SourceStableDuringRun = false;
            snapshotError = "Cannot capture final source inputs: " + captureError.Message;
        }
        document.GitAfter = await CaptureGitAsync(root, environment, cancellationToken);
        document.FinishedUnixNs = UnixNanoseconds(DateTime.UtcNow);
        List<EvidenceStep> testSteps = document.Steps.Where(step => step.Tests).ToList();
        document.TestsStatus = testSteps.Count == 0 ? "NOT RUN" : testSteps.All(step => step.Status == "PASS") ? "PASS" : "FAIL";
        document.Error = snapshotError ?? (!document.SourceStableDuringRun ? "Source inputs changed during the build/test interval" : error);
        if (document.Error is null && document.Steps.Any(step => step.Status == "FAIL"))
            document.Error = "One or more executed steps failed";
        InspectRunner(testSteps.Count != 0);
        if (testSteps.Count != 0 && !document.RunnerReportCurrent)
            document.Error ??= "No current valid runner report for this test execution";
        document.Status = document.Error is null ? "PASS" : "FAIL";
        Directory.CreateDirectory(OutputDirectory);
        AtomicFile.WriteIfChanged(EvidencePath, JsonSerializer.SerializeToUtf8Bytes(document, Json));
        return document;
    }

    private void InspectRunner(bool testsRan)
    {
        document.RunnerReport = Path.GetRelativePath(root, runnerReport ?? Path.Combine(OutputDirectory, "foundation-report.json")).Replace('\\', '/');
        if (!testsRan || testsStartedUnixNs == 0 || runnerReport is null || !File.Exists(runnerReport)) return;
        if (UnixNanoseconds(File.GetLastWriteTimeUtc(runnerReport)) + 2000000000L < testsStartedUnixNs)
        { document.RunnerReportError = "Runner report predates this test execution"; return; }
        try
        {
            byte[] bytes = File.ReadAllBytes(runnerReport);
            using JsonDocument report = JsonDocument.Parse(bytes);
            if (!report.RootElement.TryGetProperty("failures", out JsonElement failures) || !failures.TryGetInt32(out int count) || count < 0 ||
                !report.RootElement.TryGetProperty("tests", out JsonElement tests) || tests.ValueKind != JsonValueKind.Array || tests.GetArrayLength() == 0)
                throw new InvalidDataException("Runner report lacks failures/tests contract");
            document.RunnerReportCurrent = true;
            document.RunnerReportSha256 = Convert.ToHexStringLower(SHA256.HashData(bytes));
            bool nonPassing = tests.EnumerateArray().Any(test => test.ValueKind != JsonValueKind.Object ||
                !test.TryGetProperty("status", out JsonElement status) || status.ValueKind != JsonValueKind.String || status.GetString() != "PASS");
            if (count != 0 || nonPassing)
            { document.TestsStatus = "FAIL"; document.Error ??= "Runner report contains test failures"; }
        }
        catch (Exception error) when (error is IOException or JsonException or InvalidDataException)
        { document.RunnerReportError = error.Message; }
    }

    private void EnsureOpen()
    { if (finished) throw new InvalidOperationException("Evidence already finished"); }
    private static long UnixNanoseconds(DateTime value) => checked((value.ToUniversalTime().Ticks - DateTime.UnixEpoch.Ticks) * 100);

    private static async Task<GitSnapshot> CaptureGitAsync(string root,
        IReadOnlyDictionary<string, string?>? environment, CancellationToken cancellationToken)
    {
        string? head = null, headError = null, statusError = null;
        bool? dirty = null;
        string[] entries = [];
        foreach (bool status in new[] { false, true })
        {
            try
            {
                ProcessResult result = await ProcessRunner.RunAsync(new("git", status ?
                    ["-C", root, "status", "--porcelain=v1", "--untracked-files=all"] : ["-C", root, "rev-parse", "HEAD"],
                    root, environment, Timeout: TimeSpan.FromSeconds(30)), cancellationToken);
                if (result.ExitCode != 0)
                { if (status) statusError = result.StdErr.Trim(); else headError = result.StdErr.Trim(); }
                else if (status)
                { entries = result.StdOut.Split(['\r', '\n'], StringSplitOptions.RemoveEmptyEntries); dirty = entries.Length != 0; }
                else head = result.StdOut.Trim();
            }
            catch (Exception error) when (error is IOException or System.ComponentModel.Win32Exception or TimeoutException)
            { if (status) statusError = error.Message; else headError = error.Message; }
        }
        return new(head, dirty, entries, headError, statusError);
    }
}
