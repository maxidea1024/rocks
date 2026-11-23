using Rocks.Tooling.Common;

namespace Rocks.Build.Cli;

/// <summary>
/// <c>bootstrap</c> 명령이다. 미리 빌드한 바이너리(manifest <c>kind: binary</c>)를 사용자 단위 내용 주소 저장소에 받아 검사한다
/// (Docs/Design/ShaderSystem 8절 2항). 빌드·시험(<c>build</c>·<c>test</c>)은 이 명령을 부르지 않으며 받지도 않는다.
/// <list type="bullet">
/// <item><c>--root</c>: 저장소 루트(기본 현재 디렉터리).</item>
/// <item><c>--library</c>: 한 라이브러리만(폴더 이름). 없으면 모든 binary 라이브러리.</item>
/// <item><c>--store</c>: 저장소 루트(기본 <see cref="ThirdPartyStore.DefaultRoot"/>).</item>
/// <item><c>--mirror</c>: asset 이름으로 archive를 찾는 로컬·공유 디렉터리(네트워크 대신).</item>
/// <item><c>--offline true</c>: 받지 않고 저장소에 있는 것만 검사한다.</item>
/// <item><c>--windows-sdk</c>: Windows SDK 루트(<c>Windows Kits\10</c>, 기본은 자동 탐색).</item>
/// </list>
/// 사용 예: <c>dotnet out/dotnet/bin/Rocks.Build.Cli/Debug/net10.0/Rocks.Build.Cli.dll bootstrap --root .</c>
/// </summary>
internal static class ThirdPartyBootstrapCommand
{
    public static async Task<int> RunAsync(IReadOnlyDictionary<string, string> options, CancellationToken cancellationToken)
    {
        string root = Path.GetFullPath(options.GetValueOrDefault("root", Directory.GetCurrentDirectory()));
        bool offline = options.GetValueOrDefault("offline", "false") switch
        {
            "true" => true,
            "false" => false,
            _ => throw new ArgumentException("--offline must be true or false.")
        };
        var storeOptions = new ThirdPartyStoreOptions
        {
            StoreRoot = options.TryGetValue("store", out string? store) ? Path.GetFullPath(store) : ThirdPartyStore.DefaultRoot(),
            Mirror = options.TryGetValue("mirror", out string? mirror) ? Path.GetFullPath(mirror) : null,
            Offline = offline,
            WindowsSdkRoot = options.TryGetValue("windows-sdk", out string? sdk) ? Path.GetFullPath(sdk) : null
        };
        string host = ThirdPartyManifest.CurrentHostKey();
        IReadOnlyList<ThirdPartyStoreEntry> entries =
            await ThirdPartyStore.BootstrapAsync(root, host, storeOptions, options.GetValueOrDefault("library"), cancellationToken);
        foreach (ThirdPartyStoreEntry entry in entries)
        {
            Console.WriteLine($"Third-party binary {entry.Library}: {entry.Status}{(entry.Directory is null ? " (system tool, not stored)" : " " + entry.Directory)}");
        }

        Console.WriteLine($"Third-party bootstrap ({host}): {entries.Count} libraries ready in {storeOptions.StoreRoot}");
        return 0;
    }
}
