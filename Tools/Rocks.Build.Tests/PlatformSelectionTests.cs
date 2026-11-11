using Rocks.Build;
using Rocks.Build.Rules;

internal static class PlatformSelectionTests
{
    public static async Task RunAsync(NativeBuildRequest request, string root)
    {
        var context = new BuildContext(root, Path.Combine(root, "platform-output"),
            NativePlatform.Windows, NativeConfiguration.Debug);
        Check(context.PlatformDirectory == "Win64", "Windows must select Win64 sources.");
        Check((context with { Platform = NativePlatform.MacOS }).PlatformDirectory == "Mac",
            "macOS must select Mac sources.");
        Check((context with { Platform = NativePlatform.Android }).PlatformDirectory == "Android",
            "Android must select Android sources.");

        var builder = new NativeBuildEngine();
        foreach (NativePlatform invalid in new[] { (NativePlatform)(-1), (NativePlatform)3,
                     (NativePlatform)int.MaxValue })
        {
            try
            {
                _ = (context with { Platform = invalid }).PlatformDirectory;
                throw new InvalidOperationException("Unknown platform selected a source directory.");
            }
            catch (ArgumentOutOfRangeException error)
            {
                Check(error.ParamName == "Platform", "Unknown platform diagnostic must name Platform.");
            }

            string output = Path.Combine(root, "invalid-platform-" + (int)invalid);
            var invalidRequest = request with
            {
                OutputDirectory = output,
                Toolchain = request.Toolchain with { Platform = invalid }
            };
            try
            {
                await builder.BuildAsync(invalidRequest);
                throw new InvalidOperationException("Unknown platform was accepted by the build engine.");
            }
            catch (ArgumentException error)
            {
                Check(error.Message.Contains("Toolchain.Platform", StringComparison.Ordinal),
                    "Unknown target diagnostic must identify Toolchain.Platform.");
            }
            Check(!Directory.Exists(output), "Invalid target must fail before creating its output.");
        }

        // 서로 다른 host/target은 플랫폼 열거형에 있더라도 현재 cross-compile을 지원하지 않는다.
        NativePlatform other = request.Toolchain.Platform == NativePlatform.Windows
            ? NativePlatform.MacOS : NativePlatform.Windows;
        string mismatchOutput = Path.Combine(root, "mismatched-platform");
        try
        {
            await builder.BuildAsync(request with
            {
                OutputDirectory = mismatchOutput,
                Toolchain = request.Toolchain with { Platform = other }
            });
            throw new InvalidOperationException("Unsupported cross-platform build was accepted.");
        }
        catch (PlatformNotSupportedException error)
        {
            Check(error.Message.Contains("Toolchain.Platform", StringComparison.Ordinal),
                "Mismatched target diagnostic must identify Toolchain.Platform.");
        }
        Check(!Directory.Exists(mismatchOutput), "Mismatched target must fail before creating its output.");
    }

    private static void Check(bool condition, string message)
    {
        if (!condition) throw new InvalidOperationException(message);
    }
}
