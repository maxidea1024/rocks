using System.Runtime.InteropServices;
using System.Text;
using System.Text.Json;

namespace Rocks.Build.Cli;

/// <summary>셸 메뉴 등록 결과다. Sdk는 선택기 SDK, Cli는 그 SDK의 빌드 CLI, Command는 메뉴 명령이다.</summary>
internal sealed record ShellSelection(string Sdk, string Cli, string Command);

/// <summary>셸 메뉴가 쓰는 위치다. Classes·Selector는 HKCU 아래 키, Shim은 메뉴가 부르는 스크립트 파일이다(시험은 임시 위치를 준다).</summary>
internal sealed record ShellLocations(string Classes, string Selector, string Shim)
{
    /// <summary>실제 위치: <c>Software\Classes</c>, <c>Software\Rocks\ProjectSelector</c>, <c>%LOCALAPPDATA%\Rocks\ShellMenu.ps1</c>.</summary>
    public static ShellLocations Default => new(@"Software\Classes", @"Software\Rocks\ProjectSelector",
        Path.Combine(Environment.GetFolderPath(Environment.SpecialFolder.LocalApplicationData), "Rocks", "ShellMenu.ps1"));
}

/// <summary>
/// <c>.rocksproject</c> 탐색기 메뉴 등록이다(Docs/Design/ProjectFiles.ko.md 12절, PJ8·PJD44). <c>shell-register</c>가 명시적으로 부를 때만 HKCU에
/// 쓴다(관리자 권한 불필요).
/// <list type="bullet">
/// <item>동사는 <c>SystemFileAssociations\.rocksproject\shell\rocksgenproj</c>에 둔다 — 기본 연결·UserChoice와 무관하게 메뉴에 나온다.</item>
/// <item><c>.rocksproject</c> 기본값은 비어 있을 때만 ProgID로 쓰고, <c>OpenWithProgids</c>에는 ProgID만 더한다. 해제는 우리가 쓴 값만 지우고
/// 키는 비었을 때만 지운다(다른 프로그램의 연결을 지우지 않는다).</item>
/// <item>메뉴 명령은 cmd 없이 <c>pwsh -File &lt;shim&gt; -Project "%1"</c>이다(경로의 <c>%VAR%</c>·<c>!</c>를 확장하지 않는다). shim이 선택기 SDK의
/// <c>Build/GenerateProjectFiles.ps1</c>을 부른다 — IDE 빌드 잠금 안에서 SDK CLI의 <c>projectfiles --project</c>로 생성하고, 연결 해석과 다른
/// SDK로의 위임은 그 명령이 한다.</item>
/// </list>
/// </summary>
[System.Runtime.Versioning.SupportedOSPlatform("windows")]
internal static class ShellRegistration
{
    public const string Register = "shell-register";

    public const string Unregister = "shell-unregister";

    /// <summary>ProgID다.</summary>
    public const string ProgId = "Rocks.ProjectFile";

    private const string Extension = ".rocksproject";

    private const string VerbName = "rocksgenproj";

    private const string VerbText = "Generate Visual Studio project files";

    private const string TypeText = "Rocks game project";

    private const string OpenWithProgIds = "OpenWithProgids";

    // SHChangeNotify(SHCNE_ASSOCCHANGED, SHCNF_IDLIST)다: 탐색기가 연결 변경을 다시 읽는다.
    private const int AssociationChanged = 0x08000000;

    private const uint IdList = 0;

    public static int Run(string command, ISdkRegistry registry)
    {
        if (command == Register)
        {
            string pwsh = FindPwsh() ?? throw new ArgumentException("shell-register needs PowerShell 7 (pwsh) on PATH; install it from https://aka.ms/powershell.");
            ShellSelection selection = Write(registry, ShellLocations.Default, pwsh);
            Notify();
            Console.WriteLine($"Registered the {Extension} menu \"{VerbText}\" with the SDK {selection.Sdk}.");
            return 0;
        }

        Remove(ShellLocations.Default);
        Notify();
        Console.WriteLine($"Removed the {Extension} menu.");
        return 0;
    }

    /// <summary>선택기 SDK를 고르고 shim·메뉴 키·선택기 값을 쓴다. 유효한 SDK가 없으면 버린 이유를 적은 <see cref="ArgumentException"/>이다.</summary>
    internal static ShellSelection Write(ISdkRegistry registry, ShellLocations locations, string pwsh)
    {
        (string sdk, string cli) = Select(registry);
        Directory.CreateDirectory(Path.GetDirectoryName(locations.Shim)!);
        File.WriteAllText(locations.Shim, ShimText(Path.Combine(sdk, "Build", "GenerateProjectFiles.ps1")), new UTF8Encoding(true));
        string command = MenuCommand(pwsh, locations.Shim);
        string verb = $@"{locations.Classes}\SystemFileAssociations\{Extension}\shell\{VerbName}";
        using (var key = Microsoft.Win32.Registry.CurrentUser.CreateSubKey(verb))
        {
            key.SetValue("", VerbText);
        }

        using (var key = Microsoft.Win32.Registry.CurrentUser.CreateSubKey(verb + @"\command"))
        {
            key.SetValue("", command);
        }

        using (var key = Microsoft.Win32.Registry.CurrentUser.CreateSubKey($@"{locations.Classes}\{ProgId}"))
        {
            key.SetValue("", TypeText);
        }

        using (var key = Microsoft.Win32.Registry.CurrentUser.CreateSubKey($@"{locations.Classes}\{Extension}"))
        {
            // 기본 연결은 비어 있을 때만 잡는다(다른 프로그램이 가져간 연결은 둔다, PJD44).
            if (key.GetValue("") is not string current || current.Length == 0)
            {
                key.SetValue("", ProgId);
            }
        }

        using (var key = Microsoft.Win32.Registry.CurrentUser.CreateSubKey($@"{locations.Classes}\{Extension}\{OpenWithProgIds}"))
        {
            key.SetValue(ProgId, Array.Empty<byte>(), Microsoft.Win32.RegistryValueKind.None);
        }

        using (var key = Microsoft.Win32.Registry.CurrentUser.CreateSubKey(locations.Selector))
        {
            key.SetValue("Sdk", sdk);
            key.SetValue("Cli", cli);
            key.SetValue("Shim", locations.Shim);
        }

        return new(sdk, cli, command);
    }

    /// <summary>우리가 쓴 값만 지우고 빈 키만 지운다.</summary>
    internal static void Remove(ShellLocations locations)
    {
        var user = Microsoft.Win32.Registry.CurrentUser;
        string association = $@"{locations.Classes}\SystemFileAssociations\{Extension}";
        user.DeleteSubKeyTree($@"{association}\shell\{VerbName}", throwOnMissingSubKey: false);
        DeleteIfEmpty($@"{association}\shell");
        DeleteIfEmpty(association);
        DeleteIfEmpty($@"{locations.Classes}\SystemFileAssociations");
        using (var key = user.OpenSubKey($@"{locations.Classes}\{Extension}\{OpenWithProgIds}", writable: true))
        {
            key?.DeleteValue(ProgId, throwOnMissingValue: false);
        }

        DeleteIfEmpty($@"{locations.Classes}\{Extension}\{OpenWithProgIds}");
        using (var key = user.OpenSubKey($@"{locations.Classes}\{Extension}", writable: true))
        {
            if (key?.GetValue("") is string value && value == ProgId)
            {
                key.DeleteValue("", throwOnMissingValue: false);
            }
        }

        DeleteIfEmpty($@"{locations.Classes}\{Extension}");
        user.DeleteSubKeyTree($@"{locations.Classes}\{ProgId}", throwOnMissingSubKey: false);
        user.DeleteSubKeyTree(locations.Selector, throwOnMissingSubKey: false);
        File.Delete(locations.Shim);
    }

    /// <summary>메뉴 명령이다(cmd를 거치지 않는다 — 탐색기가 <c>%1</c>만 바꾼다).</summary>
    internal static string MenuCommand(string pwsh, string shim) => $"\"{pwsh}\" -NoProfile -ExecutionPolicy Bypass -File \"{shim}\" -Project \"%1\"";

    /// <summary>
    /// 메뉴가 부르는 shim 스크립트다. SDK 스크립트가 없으면(SDK를 지움) 다시 등록하라고, dotnet이 없으면 설치하라고 알린다. 생성이 실패하면 Enter를
    /// 기다려 창을 남긴다.
    /// </summary>
    internal static string ShimText(string script) => $$"""
        # Generated by Rocks.Build.Cli shell-register (Docs/Design/ProjectFiles.ko.md section 12). Do not edit; run shell-register again.
        param([Parameter(Mandatory)][string] $Project)

        $script = '{{script.Replace("'", "''", StringComparison.Ordinal)}}'
        function Stop-RocksMenu([string] $Message) {
            Write-Host $Message
            Read-Host 'Press Enter to close' | Out-Null
            exit 1
        }

        if (-not (Test-Path -LiteralPath $script -PathType Leaf)) {
            Stop-RocksMenu "The Rocks SDK of this menu was removed ($script). Run Rocks.Build.Cli shell-register again."
        }

        if (-not (Get-Command dotnet -ErrorAction SilentlyContinue)) {
            Stop-RocksMenu 'Generating project files needs the .NET SDK (dotnet) on PATH. Install it, then try again.'
        }

        & $script -Project $Project
        $code = $LASTEXITCODE
        if ($code -ne 0) {
            Read-Host 'Press Enter to close' | Out-Null
        }

        exit $code

        """;

    // PATH에서 pwsh.exe를 찾는다(메뉴 명령은 절대 경로로 둔다 — 탐색기의 PATH와 무관하게).
    private static string? FindPwsh() => (Environment.GetEnvironmentVariable("PATH") ?? "").Split(Path.PathSeparator, StringSplitOptions.RemoveEmptyEntries)
        .Select(directory => Path.Combine(directory.Trim('"'), "pwsh.exe")).FirstOrDefault(File.Exists);

    private static void DeleteIfEmpty(string subKey)
    {
        bool empty;
        using (var key = Microsoft.Win32.Registry.CurrentUser.OpenSubKey(subKey))
        {
            empty = key is not null && key.SubKeyCount == 0 && key.ValueCount == 0;
        }

        if (empty)
        {
            Microsoft.Win32.Registry.CurrentUser.DeleteSubKey(subKey, throwOnMissingSubKey: false);
        }
    }

    // 유효한 등록 SDK 중 엔진 버전이 가장 높은 것(같으면 등록 ID ordinal 첫째)과 그 CLI다. 읽거나 파싱하지 못한 SDK는 그 SDK만 버린다.
    private static (string Sdk, string Cli) Select(ISdkRegistry registry)
    {
        var reasons = new List<string>();
        var candidates = new List<(SdkRegistration Entry, EngineVersion Version, string Cli)>();
        foreach (SdkRegistration entry in registry.List(reasons))
        {
            string manifest = Path.Combine(entry.Path, SdkAssociation.ManifestName);
            string cli = Path.Combine(entry.Path, "Tools", "Rocks.Build.Cli", "Rocks.Build.Cli.dll");
            if (!File.Exists(manifest))
            {
                reasons.Add($"{entry.Path}: registered but missing");
                continue;
            }

            try
            {
                string text = File.ReadAllText(manifest);
                int? version = InstalledSdk.ManifestVersionOf(text);
                if (version != InstalledSdk.ManifestVersion)
                {
                    reasons.Add($"{entry.Path}: SDK manifest version {version?.ToString(System.Globalization.CultureInfo.InvariantCulture) ?? "missing"}, " +
                        "these tools need " + InstalledSdk.ManifestVersion);
                    continue;
                }

                if (!File.Exists(cli))
                {
                    reasons.Add($"{entry.Path}: no build CLI");
                    continue;
                }

                using JsonDocument document = JsonDocument.Parse(text);
                JsonElement engine = document.RootElement.GetProperty("EngineVersion");
                candidates.Add((entry, new EngineVersion(engine.GetProperty("Major").GetInt32(), engine.GetProperty("Minor").GetInt32(),
                    engine.GetProperty("Patch").GetInt32()), cli));
            }
            catch (Exception error) when (error is JsonException or KeyNotFoundException or InvalidOperationException or FormatException or ArgumentException
                or IOException or UnauthorizedAccessException)
            {
                reasons.Add($"{entry.Path}: unreadable SDK manifest ({error.Message})");
            }
        }

        if (candidates.Count == 0)
        {
            throw new ArgumentException("No registered SDK can run the project files menu" + (reasons.Count == 0 ? " (no SDK is registered; use sdk-register)."
                : ": " + string.Join("; ", reasons)) + " Register an SDK with sdk-register, then run shell-register.");
        }

        var best = candidates.OrderByDescending(candidate => candidate.Version.Major).ThenByDescending(candidate => candidate.Version.Minor)
            .ThenByDescending(candidate => candidate.Version.Patch).ThenBy(candidate => candidate.Entry.Id, StringComparer.Ordinal).First();
        return (best.Entry.Path, best.Cli);
    }

    private static void Notify() => NativeMethods.SHChangeNotify(AssociationChanged, IdList, IntPtr.Zero, IntPtr.Zero);

    private static class NativeMethods
    {
        [DllImport("shell32.dll")]
        public static extern void SHChangeNotify(int eventId, uint flags, IntPtr item1, IntPtr item2);
    }
}
