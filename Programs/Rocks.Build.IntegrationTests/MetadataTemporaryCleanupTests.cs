using Rocks.Build.Cli;

namespace Rocks.Build.IntegrationTests;

internal static class MetadataTemporaryCleanupTests
{
    public static void Run()
    {
        List<string> attempts = [], diagnostics = [];
        Action<string> failingDelete = path =>
        {
            attempts.Add(path);
            if (path != "second") throw new IOException("injected " + path);
        };
        int RunWithExit(int code)
        {
            bool failed = false;
            try { if (code != 0) failed = true; return code; }
            finally { MetadataTemporaryCleanup.Finish(["first", "second", "third"], failed,
                diagnostics.Add, failingDelete); }
        }
        Require(RunWithExit(17) == 17, "generator exit was lost");
        Require(attempts.SequenceEqual(["first", "second", "third"]), "cleanup stopped after first error");
        Require(diagnostics.Count == 1 && diagnostics[0].Contains("first: injected first", StringComparison.Ordinal) &&
            diagnostics[0].Contains("third: injected third", StringComparison.Ordinal), "cleanup diagnostics missing");
        attempts.Clear(); diagnostics.Clear();
        Require(RunWithExit(42) == 42, "verify exit was lost");
        try { RunWithExit(0); throw new InvalidOperationException("successful operation ignored cleanup failure"); }
        catch (IOException error) { Require(error.Message.Contains("injected first", StringComparison.Ordinal), "cleanup failure diagnostic missing"); }

        var original = new OperationCanceledException("original cancellation");
        try
        {
            try { throw original; }
            finally { MetadataTemporaryCleanup.Finish(["first"], true, diagnostics.Add, failingDelete); }
        }
        catch (OperationCanceledException actual) { Require(ReferenceEquals(original, actual), "cancellation was replaced"); }
        var failure = new InvalidOperationException("original process error");
        try
        {
            try { throw failure; }
            finally { MetadataTemporaryCleanup.Finish(["first"], true, _ => throw new IOException("stderr closed"), failingDelete); }
        }
        catch (InvalidOperationException actual) { Require(ReferenceEquals(failure, actual), "reporting error replaced original exception"); }
        Console.WriteLine("PASS metadata temporary cleanup: failure, exit codes, cancellation, broken diagnostics");
    }

    private static void Require(bool condition, string message)
    { if (!condition) throw new InvalidOperationException(message); }
}
