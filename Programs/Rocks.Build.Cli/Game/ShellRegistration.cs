using System.Runtime.InteropServices;
using System.Text.Json;

namespace Rocks.Build.Cli;

/// <summary>셸 메뉴 등록 결과다. Sdk는 선택기 SDK, Cli는 그 SDK의 빌드 CLI, Command는 메뉴 명령이다.</summary>
internal sealed record ShellSelection(string Sdk, string Cli, string Command);

/// <summary>
/// <c>.rocksproject</c> 탐색기 메뉴 등록이다(Docs/Design/ProjectFiles.ko.md 12절, PJ8). <c>shell-register</c>가 명시적으로 부를 때만 HKCU에 쓴다
/// (관리자 권한 불필요). 메뉴는 선택기 SDK의 CLI로 <c>projectfiles --project "%1"</c>을 부르고, 연결 해석과 다른 SDK CLI로의 위임은 그 명령이
/// 한다. 선택기 SDK는 유효한 등록 SDK 중 엔진 버전이 가장 높은 것이다.
/// </summary>
[System.Runtime.Versioning.SupportedOSPlatform("windows")]
internal static class ShellRegistration
{
    public const string Register = "shell-register";

    public const string Unregister = "shell-unregister";

    /// <summary>ProgID다.</summary>
    public const string ProgId = "Rocks.ProjectFile";

    /// <summary>HKCU 아래 Classes 키다(시험은 임시 하위 키를 준다).</summary>
    public const string ClassesKey = @"Software\Classes";

    /// <summary>선택기 경로를 두는 HKCU 아래 키다.</summary>
    public const string SelectorKey = @"Software\Rocks\ProjectSelector";

    private const string Extension = ".rocksproject";

    private const string VerbName = "rocksgenproj";

    private const string VerbText = "Generate Visual Studio project files";

    private const string TypeText = "Rocks game project";

    // SHChangeNotify(SHCNE_ASSOCCHANGED, SHCNF_IDLIST)다: 탐색기가 연결 변경을 다시 읽는다.
    private const int AssociationChanged = 0x08000000;

    private const uint IdList = 0;

    public static int Run(string command, ISdkRegistry registry)
    {
        if (command == Register)
        {
            ShellSelection selection = Write(registry, ClassesKey, SelectorKey);
            Notify();
            Console.WriteLine($"Registered the {Extension} menu \"{VerbText}\" with the SDK {selection.Sdk} ({selection.Cli}).");
            return 0;
        }

        Remove(ClassesKey, SelectorKey);
        Notify();
        Console.WriteLine($"Removed the {Extension} menu.");
        return 0;
    }

    /// <summary>선택기 SDK를 고르고 메뉴 키와 선택기 값을 쓴다. 유효한 SDK가 없으면 버린 이유를 적은 <see cref="ArgumentException"/>이다.</summary>
    internal static ShellSelection Write(ISdkRegistry registry, string classes, string selector)
    {
        (string sdk, string cli) = Select(registry);
        string command = MenuCommand(cli);
        using (var key = Microsoft.Win32.Registry.CurrentUser.CreateSubKey(classes + "\\" + Extension))
        {
            key.SetValue("", ProgId);
        }

        using (var key = Microsoft.Win32.Registry.CurrentUser.CreateSubKey(classes + "\\" + ProgId))
        {
            key.SetValue("", TypeText);
        }

        using (var key = Microsoft.Win32.Registry.CurrentUser.CreateSubKey($"{classes}\\{ProgId}\\shell\\{VerbName}"))
        {
            key.SetValue("", VerbText);
        }

        using (var key = Microsoft.Win32.Registry.CurrentUser.CreateSubKey($"{classes}\\{ProgId}\\shell\\{VerbName}\\command"))
        {
            key.SetValue("", command);
        }

        using (var key = Microsoft.Win32.Registry.CurrentUser.CreateSubKey(selector))
        {
            key.SetValue("Sdk", sdk);
            key.SetValue("Cli", cli);
        }

        return new(sdk, cli, command);
    }

    /// <summary>메뉴와 선택기 값을 지운다. <c>.rocksproject</c>는 기본값이 이 ProgID일 때만 지운다(다른 프로그램의 연결은 둔다).</summary>
    internal static void Remove(string classes, string selector)
    {
        bool ours;
        using (var key = Microsoft.Win32.Registry.CurrentUser.OpenSubKey(classes + "\\" + Extension))
        {
            ours = key?.GetValue("") is string value && value == ProgId;
        }

        if (ours)
        {
            Microsoft.Win32.Registry.CurrentUser.DeleteSubKeyTree(classes + "\\" + Extension, throwOnMissingSubKey: false);
        }

        Microsoft.Win32.Registry.CurrentUser.DeleteSubKeyTree(classes + "\\" + ProgId, throwOnMissingSubKey: false);
        Microsoft.Win32.Registry.CurrentUser.DeleteSubKeyTree(selector, throwOnMissingSubKey: false);
    }

    /// <summary>
    /// 메뉴 명령이다. 선택기 CLI가 없어졌으면(SDK를 지움) 다시 등록하라고 알리고, 생성이 실패하면 창을 멈춰 오류를 보인다. echo 문구에는 경로를 넣지
    /// 않는다(괄호가 든 경로가 cmd 괄호 묶음을 깨뜨린다).
    /// </summary>
    internal static string MenuCommand(string cli) =>
        $"cmd.exe /d /s /c \"if exist \"{cli}\" (dotnet \"{cli}\" projectfiles --project \"%1\" || pause) else " +
        "(echo The Rocks SDK of this menu was removed. Run Rocks.Build.Cli shell-register again. & pause)\"";

    // 유효한 등록 SDK 중 엔진 버전이 가장 높은 것(같으면 등록 ID ordinal 첫째)과 그 CLI다.
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

            string text = File.ReadAllText(manifest);
            int? version = InstalledSdk.ManifestVersionOf(text);
            if (version != InstalledSdk.ManifestVersion)
            {
                reasons.Add($"{entry.Path}: SDK manifest version {version?.ToString(System.Globalization.CultureInfo.InvariantCulture) ?? "missing"}, these tools need " +
                    InstalledSdk.ManifestVersion);
                continue;
            }

            if (!File.Exists(cli))
            {
                reasons.Add($"{entry.Path}: no build CLI");
                continue;
            }

            try
            {
                using JsonDocument document = JsonDocument.Parse(text);
                JsonElement engine = document.RootElement.GetProperty("EngineVersion");
                candidates.Add((entry, new EngineVersion(engine.GetProperty("Major").GetInt32(), engine.GetProperty("Minor").GetInt32(),
                    engine.GetProperty("Patch").GetInt32()), cli));
            }
            catch (Exception error) when (error is JsonException or KeyNotFoundException or InvalidOperationException or FormatException or ArgumentException)
            {
                reasons.Add($"{entry.Path}: unreadable engine version");
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
