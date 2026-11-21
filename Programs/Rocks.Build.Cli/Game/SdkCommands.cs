using Rocks.Build.Descriptors;

namespace Rocks.Build.Cli;

/// <summary>
/// 설치 SDK 등록과 게임 프로젝트 연결 명령이다(Docs/Design/ProjectFiles.ko.md 10.1·10.2절).
/// <list type="bullet">
/// <item><c>sdk-register --sdk &lt;경로&gt;</c>: 등록하고 ID를 출력한다(같은 경로면 기존 ID).</item>
/// <item><c>sdk-unregister (--sdk &lt;경로&gt; | --id &lt;ID&gt;)</c>: 지운다.</item>
/// <item><c>associate --project &lt;.rocksproject 또는 그 디렉터리&gt; (--sdk &lt;경로&gt; | --id &lt;ID&gt; | --version on)</c>: <c>engineAssociation</c>을
/// 쓰거나(경로는 프로젝트 기준 상대 경로) 지운다(<c>--version on</c> — engineVersion으로 고르게 한다).</item>
/// </list>
/// </summary>
internal static class SdkCommands
{
    public const string Register = "sdk-register";

    public const string Unregister = "sdk-unregister";

    public const string Associate = "associate";

    // 이 명령들만 쓰는 옵션이다(공통 파서가 받는 이름 목록에 더한다).
    internal static readonly string[] Options = ["id", "version", "register"];

    public static int Run(string command, Dictionary<string, string> options, ISdkRegistry registry)
    {
        switch (command)
        {
            case Register:
                {
                    string sdk = RequireSdk(options);
                    Console.WriteLine($"Registered {sdk} as {registry.Register(sdk)}.");
                    return 0;
                }

            case Unregister:
                {
                    string key = options.GetValueOrDefault("id") ?? Path.GetFullPath(options.GetValueOrDefault("sdk")
                        ?? throw new ArgumentException($"'{Unregister}' requires --sdk <path> or --id <id>."));
                    bool removed = registry.Unregister(key);
                    Console.WriteLine(removed ? $"Unregistered {key}." : $"{key} is not registered.");
                    return 0;
                }

            case Associate:
                {
                    string project = ProjectFile(options.GetValueOrDefault("project") ?? throw new ArgumentException($"'{Associate}' requires --project <path>."));
                    int choices = new[] { "sdk", "id", "version" }.Count(options.ContainsKey);
                    if (choices != 1)
                    {
                        throw new ArgumentException($"'{Associate}' takes exactly one of --sdk <path>, --id <id> or --version on.");
                    }

                    string? association;
                    if (options.TryGetValue("version", out string? version))
                    {
                        association = version == "on" ? null : throw new ArgumentException("--version takes on (pick the SDK by engineVersion).");
                    }
                    else if (options.TryGetValue("id", out string? id))
                    {
                        // 등록부의 철자로 쓴다(ID 비교는 대소문자를 가리지 않는다).
                        association = registry.List().FirstOrDefault(entry => SdkRegistries.SameId(entry.Id, id))?.Id
                            ?? throw new ArgumentException($"{id} is not a registered SDK.");
                    }
                    else
                    {
                        string sdk = RequireSdk(options);
                        // 프로젝트 기준 상대 경로로 쓴다(구분자가 있어야 경로로 읽힌다).
                        string relative = Path.GetRelativePath(Path.GetDirectoryName(project)!, sdk).Replace('\\', '/');
                        association = Path.IsPathRooted(relative) || relative.Contains('/') ? relative : "./" + relative;
                    }

                    SdkAssociation.Write(project, association);
                    Console.WriteLine(association is null
                        ? $"{project}: engineAssociation removed; the game tools pick a registered SDK by engineVersion."
                        : $"{project}: engineAssociation = {association}. note: an SDK path or id is specific to this computer; a shared project usually " +
                            "leaves the field out and picks the SDK by engineVersion.");
                    return 0;
                }

            default:
                throw new ArgumentException("Unknown SDK command: " + command);
        }
    }

    private static string RequireSdk(Dictionary<string, string> options)
    {
        string sdk = Path.GetFullPath(options.GetValueOrDefault("sdk") ?? throw new ArgumentException("This command requires --sdk <path>."));
        return File.Exists(Path.Combine(sdk, SdkAssociation.ManifestName)) ? PathSpelling.OnDisk(sdk)
            : throw new ArgumentException($"{sdk} is not an installed SDK ({SdkAssociation.ManifestName} not found).");
    }

    // .rocksproject 파일 또는 그 파일 하나만 있는 디렉터리다.
    private static string ProjectFile(string path)
    {
        string full = Path.GetFullPath(path);
        if (File.Exists(full))
        {
            return full;
        }

        string[] files = Directory.Exists(full) ? Directory.GetFiles(full, "*" + DescriptorReader.ProjectExtension, SearchOption.TopDirectoryOnly) : [];
        return files.Length == 1 ? files[0]
            : throw new ArgumentException($"{full}: expected a {DescriptorReader.ProjectExtension} file or a directory with exactly one.");
    }
}
