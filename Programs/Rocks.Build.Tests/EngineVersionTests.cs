using System.Text;
using Rocks.Build;
using Rocks.Build.Descriptors;

/// <summary>
/// 엔진 버전 단일 출처(Build/Build.version, Docs/Design/ReflectionFoundations.ko.md 8.2절)를 검사한다: 엄격한 읽기(파일 없음·키
/// 누락·추가·중복·범위 밖·0.0·정수 아님·1 KiB 초과·주석·후행 쉼표·UTF-8 아님), 생성자 규칙, 프로젝트 연결 판정(주·부 같음, 패치
/// 무시), 저장소 파일의 한 줄 정규형과 게임 템플릿 engineVersion = Build.version 주·부.
/// </summary>
internal static class EngineVersionTests
{
    public static void Run(string root, string? repository)
    {
        string directory = Directory.CreateDirectory(Path.Combine(root, "engine-version")).FullName;
        ReadsValidFiles(directory);
        RejectsMalformedFiles(directory);
        ConstructorAndProjectLink();
        if (repository is not null)
        {
            RepositoryFileMatchesTemplate(repository);
        }
    }

    private static void ReadsValidFiles(string directory)
    {
        EngineVersion canonical = EngineVersion.Read(Write(directory, "canonical", "{ \"MajorVersion\": 0, \"MinorVersion\": 1, \"PatchVersion\": 0 }\n"));
        Check(canonical == new EngineVersion(0, 1, 0) && canonical.ToString() == "0.1.0" && !canonical.IsEmpty, "canonical file: " + canonical);

        // 키 순서·공백은 자유이고 경계 값(65535, 주 0이어도 부가 있으면)을 받는다.
        EngineVersion largest = EngineVersion.Read(Write(directory, "largest",
            "{\"PatchVersion\":65535,\r\n\"MajorVersion\":65535,\"MinorVersion\":0}"));
        Check(largest.Major == ushort.MaxValue && largest.Minor == 0 && largest.Patch == ushort.MaxValue && largest.ToString() == "65535.0.65535",
            "largest values: " + largest);

        // 정확히 1 KiB는 받는다(초과만 거부).
        string padded = "{ \"MajorVersion\": 1, \"MinorVersion\": 0, \"PatchVersion\": 0 }";
        padded += new string(' ', EngineVersion.MaximumFileBytes - padded.Length);
        Check(EngineVersion.Read(Write(directory, "exact-limit", padded)) == new EngineVersion(1, 0, 0), "a 1 KiB file must be accepted");
    }

    private static void RejectsMalformedFiles(string directory)
    {
        Rejects(Path.Combine(directory, "missing.version"), "does not exist");
        string tooLarge = "{ \"MajorVersion\": 0, \"MinorVersion\": 1, \"PatchVersion\": 0 }";
        tooLarge += new string(' ', EngineVersion.MaximumFileBytes + 1 - tooLarge.Length);
        Rejects(Write(directory, "too-large", tooLarge), "larger than 1024 bytes");
        string invalidUtf8 = Path.Combine(directory, "invalid-utf8.version");
        File.WriteAllBytes(invalidUtf8, [.. Encoding.ASCII.GetBytes("{ \"MajorVersion\": 0, \"MinorVersion\": 1, \"PatchVersion\": 0 } "), 0xff]);
        Rejects(invalidUtf8, "not valid UTF-8");

        (string Name, string Text, string Diagnostic)[] cases =
        [
            ("empty", "", "invalid JSON"),
            ("comment", "{ \"MajorVersion\": 0, /* 주석 */ \"MinorVersion\": 1, \"PatchVersion\": 0 }", "invalid JSON"),
            ("trailing-comma", "{ \"MajorVersion\": 0, \"MinorVersion\": 1, \"PatchVersion\": 0, }", "invalid JSON"),
            ("array", "[0, 1, 0]", "root must be a JSON object"),
            ("missing-key", "{ \"MajorVersion\": 0, \"MinorVersion\": 1 }", "'PatchVersion' is required"),
            ("extra-key", "{ \"MajorVersion\": 0, \"MinorVersion\": 1, \"PatchVersion\": 0, \"Changelist\": 7 }", "unknown field 'Changelist'"),
            ("case-differs", "{ \"majorVersion\": 0, \"MinorVersion\": 1, \"PatchVersion\": 0 }", "unknown field 'majorVersion'"),
            ("duplicate", "{ \"MajorVersion\": 0, \"MinorVersion\": 1, \"PatchVersion\": 0, \"MinorVersion\": 2 }", "duplicate field 'MinorVersion'"),
            ("too-big", "{ \"MajorVersion\": 65536, \"MinorVersion\": 1, \"PatchVersion\": 0 }", "'MajorVersion' must be an integer from 0 to 65535"),
            ("huge", "{ \"MajorVersion\": 0, \"MinorVersion\": 99999999999999999999, \"PatchVersion\": 0 }", "'MinorVersion' must be an integer"),
            ("negative", "{ \"MajorVersion\": 0, \"MinorVersion\": 1, \"PatchVersion\": -1 }", "'PatchVersion' must be an integer"),
            ("fraction", "{ \"MajorVersion\": 0, \"MinorVersion\": 1.5, \"PatchVersion\": 0 }", "'MinorVersion' must be an integer"),
            ("fraction-zero", "{ \"MajorVersion\": 1.0, \"MinorVersion\": 1, \"PatchVersion\": 0 }", "'MajorVersion' must be an integer"),
            ("exponent", "{ \"MajorVersion\": 1e0, \"MinorVersion\": 1, \"PatchVersion\": 0 }", "'MajorVersion' must be an integer"),
            ("string", "{ \"MajorVersion\": \"0\", \"MinorVersion\": 1, \"PatchVersion\": 0 }", "'MajorVersion' must be an integer"),
            ("null", "{ \"MajorVersion\": 0, \"MinorVersion\": null, \"PatchVersion\": 0 }", "'MinorVersion' must be an integer"),
            ("zero-zero", "{ \"MajorVersion\": 0, \"MinorVersion\": 0, \"PatchVersion\": 3 }", "must not both be 0")
        ];
        foreach ((string name, string text, string diagnostic) in cases)
        {
            Rejects(Write(directory, name, text), diagnostic);
        }
    }

    private static void ConstructorAndProjectLink()
    {
        Check(default(EngineVersion).IsEmpty && default(EngineVersion).ToString() == "0.0.0", "default must be the empty value");
        foreach ((int major, int minor, int patch) in new[] { (65536, 0, 0), (0, -1, 0), (0, 1, 65536) })
        {
            try
            {
                _ = new EngineVersion(major, minor, patch);
                throw new InvalidOperationException($"out-of-range engine version accepted: {major}.{minor}.{patch}");
            }
            catch (ArgumentOutOfRangeException)
            {
            }
        }

        try
        {
            _ = new EngineVersion(0, 0, 1);
            throw new InvalidOperationException("0.0.1 accepted as an engine version");
        }
        catch (ArgumentException error) when (error is not ArgumentOutOfRangeException)
        {
        }

        // 프로젝트 연결: 주·부가 같아야 하고 패치는 보지 않는다.
        var engine = new EngineVersion(1, 4, 9);
        Check(engine.CanLinkProject(1, 4), "the same major.minor must link");
        Check(!engine.CanLinkProject(1, 3) && !engine.CanLinkProject(1, 5) && !engine.CanLinkProject(0, 4) && !engine.CanLinkProject(2, 4),
            "a different major or minor must not link (older and newer projects alike)");
    }

    // 저장소 Build/Build.version은 한 줄 정규형이고 게임 템플릿 engineVersion은 그 주·부와 같다(버전을 올릴 때 함께 바꾼다 —
    // Build/README.md 올리는 규칙).
    private static void RepositoryFileMatchesTemplate(string repository)
    {
        string path = EngineVersion.PathIn(repository);
        EngineVersion version = EngineVersion.Read(path);
        string canonical = $"{{ \"MajorVersion\": {version.Major}, \"MinorVersion\": {version.Minor}, \"PatchVersion\": {version.Patch} }}\n";
        Check(File.ReadAllText(path) == canonical, $"{EngineVersion.RelativePath} must be the one-line canonical form:\n{canonical}");
        ProjectDescriptor template = DescriptorReader.ReadProject(Path.Combine(repository, "Templates", "Game", "Foundation", "Game.rocksproject"));
        Check(version.CanLinkProject(template.EngineVersion.Major, template.EngineVersion.Minor),
            $"Templates/Game/Foundation/Game.rocksproject engineVersion {template.EngineVersion} must equal the major.minor of " +
            $"{EngineVersion.RelativePath} ({version})");
    }

    private static string Write(string directory, string name, string text)
    {
        string path = Path.Combine(directory, name + ".version");
        File.WriteAllText(path, text, new UTF8Encoding(false));
        return path;
    }

    private static void Rejects(string path, string diagnostic)
    {
        try
        {
            EngineVersion.Read(path);
        }
        catch (InvalidDataException error)
        {
            if (!error.Message.StartsWith(path + ": ", StringComparison.Ordinal) || !error.Message.Contains(diagnostic, StringComparison.Ordinal))
            {
                throw new InvalidOperationException($"engine version error for {Path.GetFileName(path)} had an unexpected message: {error.Message}");
            }

            return;
        }

        throw new InvalidOperationException("malformed engine version file accepted: " + Path.GetFileName(path));
    }

    private static void Check(bool condition, string message)
    {
        if (!condition)
        {
            throw new InvalidOperationException(message);
        }
    }
}
