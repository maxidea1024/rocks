using System.Diagnostics;
using System.Reflection;
using System.Text;
using System.Text.Json;
using Rocks.Tooling.Common;

namespace Rocks.ToolTests;

internal static class Verify
{
    public static void That(bool condition, string message)
    { if (!condition) throw new InvalidOperationException(message); }
    public static void Equal<T>(T expected, T actual, string message)
    { That(EqualityComparer<T>.Default.Equals(expected, actual), $"{message}: expected={expected}; actual={actual}"); }
    public static void Contains(string expected, string actual, string message)
    { That(actual.Contains(expected, StringComparison.Ordinal), $"{message}: missing '{expected}' in:\n{actual}"); }
    public static void Throws<T>(Action action) where T : Exception
    {
        try { action(); }
        catch (T) { return; }
        throw new InvalidOperationException("Expected " + typeof(T).Name);
    }
    public static async Task ThrowsAsync<T>(Func<Task> action) where T : Exception
    {
        try { await action(); }
        catch (T) { return; }
        throw new InvalidOperationException("Expected " + typeof(T).Name);
    }
}

internal sealed class TemporaryDirectory : IDisposable
{
    private readonly string parent;
    public TemporaryDirectory()
    {
        Root = Directory.CreateTempSubdirectory("rocks-tools-").FullName;
        parent = Path.GetDirectoryName(Root)!;
    }
    public string Root { get; }
    public string FilePath(string relative) => Path.Combine(Root, relative);
    public void Write(string relative, string content)
    {
        string path = FilePath(relative);
        Directory.CreateDirectory(Path.GetDirectoryName(path)!);
        File.WriteAllText(path, content, new UTF8Encoding(false, true));
    }
    public void Dispose()
    {
        string path = Path.GetFullPath(Root);
        if (Path.GetDirectoryName(path) != parent || !Path.GetFileName(path).StartsWith("rocks-tools-", StringComparison.Ordinal))
            throw new InvalidOperationException("Refusing cleanup outside owned fixture");
        if (Directory.Exists(path)) Directory.Delete(path, recursive: true);
    }
}

internal sealed class TestSuite
{
    private readonly SortedDictionary<string, Func<Task>> tests = new(StringComparer.Ordinal);
    public void Add(string name, Action action) => Add(name, () => { action(); return Task.CompletedTask; });
    public void Add(string name, Func<Task> action) => tests.Add(name, action);

    public async Task<int> RunAsync(string? jsonPath, ulong seed)
    {
        if (tests.Count == 0) throw new InvalidOperationException("No test cases registered");
        if (jsonPath is not null)
        {
            jsonPath = Path.GetFullPath(jsonPath);
            if (Directory.Exists(jsonPath)) throw new IOException("JSON report path is a directory");
            Directory.CreateDirectory(Path.GetDirectoryName(jsonPath)!);
            // 실행 전 출력 경로의 접근 가능성을 확인한다. 최종 파일은 AtomicFile로 게시한다.
            string probe = jsonPath + ".probe-" + Guid.NewGuid().ToString("N");
            using (new FileStream(probe, FileMode.CreateNew, FileAccess.Write, FileShare.None)) { }
            File.Delete(probe);
        }
        List<object> results = [];
        int failures = 0;
        foreach ((string name, Func<Task> action) in tests)
        {
            long started = Stopwatch.GetTimestamp();
            string? error = null;
            try { await action(); }
            catch (Exception failure) when (failure is not OutOfMemoryException)
            { error = failure.ToString(); ++failures; }
            long duration = checked(Stopwatch.GetElapsedTime(started).Ticks * 100);
            string status = error is null ? "PASS" : "FAIL";
            Console.WriteLine($"[{status}] {name}");
            if (error is not null) Console.Error.WriteLine(error);
            results.Add(new { name, status, failures = error is null ? 0 : 1, duration_ns = duration, error });
        }
        if (jsonPath is not null)
            AtomicFile.WriteIfChanged(jsonPath, JsonSerializer.SerializeToUtf8Bytes(
                new { schema = 1, seed, repeat = 1, failures, tests = results }, new JsonSerializerOptions { WriteIndented = true }));
        Console.WriteLine($"{tests.Count} test cases, {failures} failures");
        return failures == 0 ? 0 : 1;
    }
}

internal sealed class Options
{
    private readonly Dictionary<string, string> values = new(StringComparer.Ordinal);
    public List<string> ClangArguments { get; } = [];
    public bool Asan { get; private set; }
    // third-party 명령: manifest를 실제로 고친다(없으면 차이만 보고).
    public bool Write { get; private set; }
    public string Root => Path.GetFullPath(Required("root"));
    public string? Json => Optional("json");
    public ulong Seed => values.TryGetValue("seed", out string? value) ? ulong.Parse(value, System.Globalization.CultureInfo.InvariantCulture) : 7351;
    public string Required(string key) => values.TryGetValue(key, out string? value) && value.Length != 0 ? value :
        throw new ArgumentException("Missing --" + key);
    public string? Optional(string key) => values.GetValueOrDefault(key);

    public static Options Parse(string[] arguments)
    {
        Options result = new();
        HashSet<string> allowed = new(StringComparer.Ordinal) { "root", "json", "seed", "native-executable",
            "compiler", "metacompiler", "formatter", "dotnet", "compile-commands" };
        for (int index = 0; index < arguments.Length; ++index)
        {
            string argument = arguments[index];
            if (argument == "--asan") { if (result.Asan) throw new ArgumentException("Duplicate --asan"); result.Asan = true; continue; }
            if (argument == "--write") { if (result.Write) throw new ArgumentException("Duplicate --write"); result.Write = true; continue; }
            if (!argument.StartsWith("--", StringComparison.Ordinal)) throw new ArgumentException("Unknown argument: " + argument);
            int equals = argument.IndexOf('=');
            string key = equals < 0 ? argument[2..] : argument[2..equals];
            string value = equals >= 0 ? argument[(equals + 1)..] : ++index < arguments.Length ? arguments[index] :
                throw new ArgumentException("Missing value: " + argument);
            if (key == "clang-arg") result.ClangArguments.Add(value);
            else if (!allowed.Contains(key) || !result.values.TryAdd(key, value)) throw new ArgumentException("Unknown or duplicate option: " + key);
        }
        return result;
    }
}

internal static class SelfProcess
{
    public static ProcessSpec Command(IReadOnlyList<string> arguments, string workingDirectory,
        TimeSpan? timeout = null, string? input = null)
    {
        string executable = Environment.ProcessPath ?? throw new InvalidOperationException("Cannot locate current process");
        List<string> actual = [];
        if (Path.GetFileNameWithoutExtension(executable).Equals("dotnet", StringComparison.OrdinalIgnoreCase))
            actual.Add(Assembly.GetExecutingAssembly().Location);
        actual.AddRange(arguments);
        return new(executable, actual, workingDirectory, StandardInput: input, Timeout: timeout ?? TimeSpan.FromSeconds(15));
    }
}
