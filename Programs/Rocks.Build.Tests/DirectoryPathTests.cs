using Rocks.Build;
using Rocks.Tooling.Common;

/// <summary>
/// 디렉터리 포함 판정(DirectoryPath)의 드라이브 루트·UNC·POSIX 루트·끝 구분자·대소문자 경우와, subst 드라이브 루트를 실제로 만든
/// 빌드 입력 검사(출력이 드라이브 루트이고 소스 루트를 담으면 거부)를 검사한다. 예전 접두 계산
/// (<c>TrimEndingDirectorySeparator(root) + 구분자</c>)은 드라이브 루트에서 <c>W:\\</c>가 되어 그 아래 어떤 경로와도 맞지 않았다.
/// </summary>
internal static class DirectoryPathTests
{
    public static async Task RunAsync(NativeBuildRequest request, string root, Action<string> pass)
    {
        Containment();
        pass("directory paths: drive, UNC and POSIX roots, trailing separators, sibling prefixes and case");

        if (OperatingSystem.IsWindows())
        {
            await SubstDriveRootAsync(request, root);
            pass("directory paths: a real subst drive root is a containing root for build input validation");
        }
    }

    private static void Containment()
    {
        if (OperatingSystem.IsWindows())
        {
            Check(DirectoryPath.WithTrailingSeparator(@"W:\") == @"W:\" && DirectoryPath.WithTrailingSeparator(@"C:\work\rocks") == @"C:\work\rocks\" &&
                DirectoryPath.WithTrailingSeparator(@"C:\work\rocks\") == @"C:\work\rocks\" && DirectoryPath.WithTrailingSeparator("C:/work/rocks") == @"C:\work\rocks\" &&
                DirectoryPath.WithTrailingSeparator(@"\\server\share") == @"\\server\share\", "trailing separator form");
            (string Path, string Root, bool AtOrUnder, bool Strictly)[] cases =
            [
                (@"W:\Core\A.h", @"W:\", true, true),
                (@"W:\", @"W:\", true, false),
                (@"w:\core", @"W:\", true, true),
                ("W:/Core/A.h", @"W:\", true, true),
                (@"X:\Core", @"W:\", false, false),
                (@"C:\work\rocks2\x", @"C:\work\rocks", false, false),
                (@"C:\work\rocks", @"C:\work\rocks\", true, false),
                (@"C:\work\rocks\x", @"C:\work\rocks\", true, true),
                (@"C:\work", @"C:\work\rocks", false, false),
                (@"\\server\share\x y", @"\\server\share", true, true),
                (@"\\server\share\x", @"\\server\share\", true, true),
                (@"\\server\share2\x", @"\\server\share", false, false),
                (@"\\?\W:\Core\A.h", @"\\?\W:\", true, true),
                (@"\\?\W:\", @"\\?\W:\", true, false)
            ];
            Run(cases);
            // 예전 Fingerprints.IsUnder는 드라이브 루트 아래를 거짓으로 판정했다.
            Check(Fingerprints.IsUnder(@"W:\src", @"W:\") && Fingerprints.IsUnder(@"W:\", @"W:\") && !Fingerprints.IsUnder(@"V:\src", @"W:\"),
                "Fingerprints.IsUnder must treat a drive root as a containing root");
        }
        else
        {
            Check(DirectoryPath.WithTrailingSeparator("/") == "/" && DirectoryPath.WithTrailingSeparator("/work/rocks") == "/work/rocks/",
                "trailing separator form");
            (string Path, string Root, bool AtOrUnder, bool Strictly)[] cases =
            [
                ("/a/b", "/", true, true),
                ("/", "/", true, false),
                ("/ab", "/a", false, false),
                ("/a", "/a/", true, false),
                ("/a/b/", "/a", true, true),
                ("/A/b", "/a", false, false)
            ];
            Run(cases);
            Check(Fingerprints.IsUnder("/src", "/") && !Fingerprints.IsUnder("/srcx", "/src"), "Fingerprints.IsUnder POSIX root");
        }
    }

    private static void Run((string Path, string Root, bool AtOrUnder, bool Strictly)[] cases)
    {
        foreach (var item in cases)
        {
            Check(DirectoryPath.IsAtOrUnder(item.Path, item.Root) == item.AtOrUnder,
                $"IsAtOrUnder({item.Path}, {item.Root}) must be {item.AtOrUnder}");
            Check(DirectoryPath.IsStrictlyUnder(item.Path, item.Root) == item.Strictly,
                $"IsStrictlyUnder({item.Path}, {item.Root}) must be {item.Strictly}");
        }
    }

    // subst 드라이브 루트를 출력으로 주고 소스 루트를 그 아래에 두면 "출력이 소스 루트의 조상"으로 거부해야 한다(예전에는 통과했다).
    private static async Task SubstDriveRootAsync(NativeBuildRequest request, string root)
    {
        string mapped = Directory.CreateDirectory(Path.Combine(root, "drive root 한글")).FullName;
        string source = Directory.CreateDirectory(Path.Combine(mapped, "src")).FullName;
        File.WriteAllText(Path.Combine(source, "lib.cpp"), "int DriveRootAnswer() noexcept { return 1; }\n");
        // 빈 드라이브 문자를 S부터 내려가며 고른다(IntegrationTests는 K부터 올라가고 BuildInfoTests는 Z부터 내려간다).
        var used = DriveInfo.GetDrives().Select(drive => char.ToUpperInvariant(drive.Name[0])).ToHashSet();
        char letter = Enumerable.Range('K', 'S' - 'K' + 1).Select(value => (char)value).Reverse().First(value => !used.Contains(value));
        string drive = letter + ":";
        ProcessResult created = await ProcessRunner.RunAsync(new("cmd.exe", ["/d", "/c", "subst", drive, mapped], root));
        Check(created.ExitCode == 0, "subst failed: " + created.StdOut + created.StdErr);
        try
        {
            string driveRoot = drive + "\\";
            Check(Fingerprints.IsUnder(Path.Combine(driveRoot, "src", "lib.cpp"), driveRoot), "a file on the subst drive must be under its root");
            var invalid = request with
            {
                RootDirectory = Path.Combine(driveRoot, "src"),
                OutputDirectory = driveRoot,
                Modules = [new() { Name = "DriveRoot", Sources = [Path.Combine(driveRoot, "src", "lib.cpp")] }],
                Targets = ["DriveRoot"]
            };
            try
            {
                await new NativeBuildEngine().BuildAsync(invalid);
                throw new InvalidOperationException("An output at the drive root containing the source root was accepted.");
            }
            catch (ArgumentException error)
            {
                Check(error.Message.Contains("OutputDirectory must not be the source root or its ancestor", StringComparison.Ordinal),
                    "Unexpected rejection: " + error.Message);
            }

            // 출력이 드라이브 루트 아래(소스 밖)면 그대로 빌드된다.
            var valid = invalid with { OutputDirectory = Path.Combine(driveRoot, "out") };
            NativeBuildResult built = await new NativeBuildEngine().BuildAsync(valid);
            Check(built.Succeeded && built.ExecutedActions == 2, "Build under a subst drive root failed: " + built.Failure);
        }
        finally
        {
            await ProcessRunner.RunAsync(new("cmd.exe", ["/d", "/c", "subst", drive, "/D"], root));
        }
    }

    private static void Check(bool condition, string message)
    {
        if (!condition)
        {
            throw new InvalidOperationException(message);
        }
    }
}
