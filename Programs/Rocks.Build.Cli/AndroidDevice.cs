using Rocks.Tooling.Common;

namespace Rocks.Build.Cli;

/// <summary>
/// adb로 Android 기기·에뮬레이터에서 테스트 실행 파일을 돌린다. 실행 파일은 /data/local/tmp 아래의
/// 전용 디렉터리에 올리고, 보고서(JSON)를 host로 회수한다. 연결된 기기가 정확히 하나가 아니면
/// ANDROID_SERIAL로 고르도록 adb가 실패를 알린다. 시험은 저장소 루트 기준 상대 경로로 data 라이브러리
/// 파일(Wycheproof·JSONTestSuite·UCD 등)을 읽고 기기에서는 작업 디렉터리가 전용 디렉터리이므로, data
/// 라이브러리 manifest의 원본 파일을 같은 상대 경로로 함께 올린다.
/// </summary>
internal static class AndroidDevice
{
    private const string DeviceDirectory = "/data/local/tmp/rocks-test";

    // adb push 한 번에 넘기는 파일 수다(Windows 명령줄 길이 제한 안에 머물게 한다).
    private const int PushBatchFiles = 64;

    public static async Task<int> RunTestsAsync(string root, string executable, string report, string? adb,
        IReadOnlyDictionary<string, string?> environment, CancellationToken cancellationToken)
    {
        string tool = adb ?? FindAdb() ?? throw new FileNotFoundException(
            "adb not found. Pass --adb or set ANDROID_HOME/ANDROID_SDK_ROOT.");
        string name = Path.GetFileName(executable);
        string deviceExecutable = DeviceDirectory + "/" + name;
        string deviceReport = DeviceDirectory + "/report.json";
        var steps = new List<string[]>
        {
            new[] { "shell", "mkdir -p " + DeviceDirectory + " && rm -f " + deviceReport },
            new[] { "push", executable, deviceExecutable },
            new[] { "shell", "chmod 755 " + deviceExecutable },
        };
        foreach (var directory in DataFiles(root).GroupBy(file => file[..file.LastIndexOf('/')], StringComparer.Ordinal)
                     .OrderBy(group => group.Key, StringComparer.Ordinal))
        {
            string remote = DeviceDirectory + "/" + directory.Key;
            steps.Add(["shell", "mkdir -p '" + remote + "'"]);
            foreach (string[] batch in directory.Chunk(PushBatchFiles))
            {
                steps.Add(["push", .. batch.Select(file => Path.Combine(root, file)), remote + "/"]);
            }
        }

        foreach (string[] step in steps)
        {
            var prepared = await ProcessRunner.RunAsync(new(tool, step, Directory.GetCurrentDirectory(), environment,
                Timeout: TimeSpan.FromMinutes(5)), cancellationToken);
            if (prepared.ExitCode != 0)
            {
                Console.Error.Write(prepared.StdOut + prepared.StdErr);
                throw new InvalidOperationException("adb " + step[0] + " failed (is exactly one device connected?).");
            }
        }
        // 셸 종료 코드를 그대로 받기 위해 마지막 줄에 표지와 함께 출력한다.
        var run = await ProcessRunner.RunAsync(new(tool,
            ["shell", "cd " + DeviceDirectory + " && ./" + name + " --seed 7351 --json report.json; echo ROCKS_EXIT=$?"],
            Directory.GetCurrentDirectory(), environment, Timeout: TimeSpan.FromMinutes(30)), cancellationToken);
        Console.Write(run.StdOut);
        Console.Error.Write(run.StdErr);
        int exit = 1;
        foreach (string line in run.StdOut.Split('\n'))
        {
            string trimmed = line.Trim();
            if (trimmed.StartsWith("ROCKS_EXIT=", StringComparison.Ordinal) &&
                int.TryParse(trimmed["ROCKS_EXIT=".Length..], out int parsed))
            {
                exit = parsed;
            }
        }

        var pulled = await ProcessRunner.RunAsync(new(tool, ["pull", deviceReport, report],
            Directory.GetCurrentDirectory(), environment, Timeout: TimeSpan.FromMinutes(5)), cancellationToken);
        if (pulled.ExitCode != 0 && exit == 0)
        {
            throw new InvalidOperationException("adb pull of the test report failed.");
        }

        return exit;
    }

    /// <summary>
    /// 시험이 읽는 data 라이브러리(manifest <c>"kind": "data"</c>)의 manifest와 원본 파일의 저장소 상대 경로다. 원본은
    /// manifest의 <c>files</c>를 그대로 쓰므로 정책 검사가 확인한 파일만 올린다. manifest가 없는 디렉터리는 건너뛰고, 읽을 수
    /// 없는 manifest는 예외로 드러낸다.
    /// </summary>
    internal static IReadOnlyList<string> DataFiles(string root)
    {
        string thirdParty = Path.Combine(root, ThirdPartyManifest.ThirdPartyRoot);
        if (!Directory.Exists(thirdParty))
        {
            return [];
        }

        List<string> files = [];
        foreach (string library in Directory.EnumerateDirectories(thirdParty).Order(StringComparer.Ordinal))
        {
            if (!File.Exists(Path.Combine(library, "ThirdParty.json")))
            {
                continue;
            }

            ThirdPartyManifest manifest = ThirdPartyManifest.Read(library);
            if (manifest.Kind != ThirdPartyKind.Data)
            {
                continue;
            }

            // 시험이 manifest로 파일 목록을 읽기도 하므로(JsonTests) manifest도 함께 올린다.
            string libraryPrefix = $"{ThirdPartyManifest.ThirdPartyRoot}/{Path.GetFileName(library)}/";
            files.Add(libraryPrefix + "ThirdParty.json");
            string prefix = libraryPrefix + manifest.UpstreamDirectory + "/";
            files.AddRange(manifest.Files.Keys.Order(StringComparer.Ordinal).Select(file => prefix + file));
        }

        return files;
    }

    private static string? FindAdb()
    {
        string executable = OperatingSystem.IsWindows() ? "adb.exe" : "adb";
        foreach (string? sdk in new[]
                 {
                     Environment.GetEnvironmentVariable("ANDROID_HOME"),
                     Environment.GetEnvironmentVariable("ANDROID_SDK_ROOT"),
                     OperatingSystem.IsWindows()
                         ? Path.Combine(Environment.GetFolderPath(Environment.SpecialFolder.LocalApplicationData), "Android", "Sdk")
                         : Path.Combine(Environment.GetFolderPath(Environment.SpecialFolder.UserProfile), "Library", "Android", "sdk")
                 })
        {
            if (sdk is null)
            {
                continue;
            }

            string candidate = Path.Combine(sdk, "platform-tools", executable);
            if (File.Exists(candidate))
            {
                return candidate;
            }
        }

        return null;
    }
}
