using System.Formats.Tar;
using System.IO.Compression;
using System.Security.Cryptography;
using System.Text;
using System.Text.Json;
using System.Text.Json.Nodes;
using Rocks.Tooling.Common;

namespace Rocks.ToolTests;

/// <summary>
/// 미리 빌드한 바이너리 bootstrap(사용자 단위 내용 주소 저장소)의 시험이다(Docs/Design/ShaderSystem.ko.md 8절 2항 저장 위치).
/// 네트워크를 쓰지 않는다: archive는 미러 디렉터리나 주입한 받기 함수로 공급한다. 중단은 받기 함수가 일부만 쓰고 예외를
/// 던지는 것과, 끊긴 이전 실행이 남긴 임시 디렉터리로 재현한다(시간·경쟁에 기대지 않는다).
/// </summary>
internal static class ThirdPartyStoreTests
{
    private const string Host = "windows-x64";

    private const string Library = "Engine/Source/ThirdParty/FakeTool";

    private static readonly (string Path, string Text, string Role)[] ArchiveFiles =
    [
        ("bin/fake.dll", "fake runtime\n", "runtime"),
        ("bin/fake.pdb", "fake symbols\n", "symbols"),
        ("LICENSE", "Fake tool license\n", "notice")
    ];

    public static void Register(TestSuite suite, Options options)
    {
        // 받는 것은 명시적인 bootstrap 명령뿐이다(빌드 action·워커·시험 경로는 받지 않는다).
        suite.Add("Policy.ThirdPartyStoreInstallsOnlyFromBootstrap", () =>
        {
            string[] allowed = ["Programs/Rocks.Build.Cli/ThirdPartyBootstrapCommand.cs", "Programs/Rocks.Tooling.Common/ThirdPartyStore.cs"];
            List<string> callers = [];
            foreach (string directory in new[] { "Programs", "Build", "Core", "Engine" })
            {
                string full = Path.Combine(options.Root, directory);
                if (!Directory.Exists(full))
                {
                    continue;
                }

                foreach (string path in Directory.EnumerateFiles(full, "*.cs", SearchOption.AllDirectories))
                {
                    string relative = Path.GetRelativePath(options.Root, path).Replace('\\', '/');
                    if (relative.StartsWith("Programs/Rocks.ToolTests/", StringComparison.Ordinal) || allowed.Contains(relative, StringComparer.Ordinal) ||
                        relative.Contains("/obj/", StringComparison.Ordinal) || relative.Contains("/bin/", StringComparison.Ordinal))
                    {
                        continue;
                    }

                    string text = File.ReadAllText(path);
                    if (text.Contains("ThirdPartyStore.InstallAsync", StringComparison.Ordinal) ||
                        text.Contains("ThirdPartyStore.BootstrapAsync", StringComparison.Ordinal))
                    {
                        callers.Add(relative);
                    }
                }
            }

            Verify.That(callers.Count == 0, "third-party binaries are downloaded outside the bootstrap command: " + string.Join(", ", callers));
        });
        suite.Add("ThirdPartyStore.InstallVerifyAndOffline", () => InstallVerifyAndOffline());
        suite.Add("ThirdPartyStore.DigestMismatchInstallsNothing", () => DigestMismatch());
        suite.Add("ThirdPartyStore.InterruptedInstallRetries", () => InterruptedInstallRetries());
        suite.Add("ThirdPartyStore.HostLockAndArchiveControls", () => HostLockAndArchiveControls());
        suite.Add("ThirdPartyStore.SdkAndSystemOrigins", () => SdkAndSystemOrigins());
        suite.Add("ThirdPartyStore.BootstrapRepository", () => BootstrapRepository());
        suite.Add("ThirdPartyStore.QuarantineAttribute", QuarantineAttribute);
    }

    private static async Task InstallVerifyAndOffline()
    {
        using TemporaryDirectory fixture = new();
        (ThirdPartyManifest manifest, string mirror) = WriteFixture(fixture);
        string store = fixture.FilePath("store");
        ThirdPartyHost host = manifest.Binary!.Host(Host);
        Verify.That(ThirdPartyStore.Verify(store, manifest, host).Single().Contains("bootstrap", StringComparison.Ordinal),
            "missing store directory must point to bootstrap");

        // 저장소가 비었으면 오프라인 실행은 받지 않고 실패한다.
        await Verify.ThrowsAsync<InvalidOperationException>(() =>
            ThirdPartyStore.InstallAsync(manifest, Host, new() { StoreRoot = store, Mirror = mirror, Offline = true }));

        ThirdPartyStoreEntry? installed = await ThirdPartyStore.InstallAsync(manifest, Host, new() { StoreRoot = store, Mirror = mirror });
        string directory = Path.Combine(store, host.StoreKey!);
        Verify.That(installed == new ThirdPartyStoreEntry("FakeTool", ThirdPartyStoreStatus.Installed, directory), "install result");
        foreach ((string path, string text, _) in ArchiveFiles)
        {
            Verify.Equal(text, File.ReadAllText(Path.Combine(directory, path)), "installed file " + path);
        }

        Verify.Equal(0, ThirdPartyStore.Verify(store, manifest, host).Count, "installed store must verify");
        Verify.That(!Directory.EnumerateDirectories(store, ThirdPartyStore.PartialPrefix + "*").Any(), "temporary directory left behind");

        // 저장소만으로 하는 오프라인 동작: 미러도 받기 함수도 쓰지 않는다(쓰면 예외).
        ThirdPartyStoreEntry? offline = await ThirdPartyStore.InstallAsync(manifest, Host, new()
        {
            StoreRoot = store,
            Offline = true,
            Fetch = (_, _, _) => throw new InvalidOperationException("offline bootstrap must not fetch")
        });
        Verify.Equal(ThirdPartyStoreStatus.Present, offline!.Status, "offline status");

        // 저장소 파일이 바뀌면(손상·변조) 고치지 않고 알린다.
        File.AppendAllText(Path.Combine(directory, "bin/fake.dll"), "x");
        Verify.That(ThirdPartyStore.Verify(store, manifest, host).Count == 1, "changed store file escaped verification");
        await Verify.ThrowsAsync<InvalidOperationException>(() =>
            ThirdPartyStore.InstallAsync(manifest, Host, new() { StoreRoot = store, Mirror = mirror }));
        File.WriteAllText(Path.Combine(directory, "extra.txt"), "x");
        Verify.That(ThirdPartyStore.Verify(store, manifest, host).Any(problem => problem.Contains("extra.txt", StringComparison.Ordinal)),
            "unlisted store file escaped verification");
    }

    private static async Task DigestMismatch()
    {
        // archive 1 byte 변경: archive SHA-256 검사에서 멈추고 아무것도 남기지 않는다.
        using (TemporaryDirectory fixture = new())
        {
            (ThirdPartyManifest manifest, string mirror) = WriteFixture(fixture);
            string asset = Path.Combine(mirror, manifest.Binary!.Host(Host).Archive!.FileName);
            byte[] bytes = File.ReadAllBytes(asset);
            bytes[^1] ^= 1;
            File.WriteAllBytes(asset, bytes);
            await ExpectNothingInstalled(manifest, fixture.FilePath("store"), new() { StoreRoot = fixture.FilePath("store"), Mirror = mirror });
        }

        // archive는 맞지만 안의 파일 SHA-256이 manifest와 다르다.
        using (TemporaryDirectory fixture = new())
        {
            (ThirdPartyManifest manifest, string mirror) = WriteFixture(fixture, manifest => HostFile(manifest, 0)["sha256"] = new string('d', 64));
            await ExpectNothingInstalled(manifest, fixture.FilePath("store"), new() { StoreRoot = fixture.FilePath("store"), Mirror = mirror });
        }

        // archive에 manifest가 적지 않은 파일이 있다(목록에서 하나를 뺀다).
        using (TemporaryDirectory fixture = new())
        {
            (ThirdPartyManifest manifest, string mirror) = WriteFixture(fixture, manifest => Files(manifest).RemoveAt(2));
            await ExpectNothingInstalled(manifest, fixture.FilePath("store"), new() { StoreRoot = fixture.FilePath("store"), Mirror = mirror });
        }

        // 미러에 asset이 없다.
        using (TemporaryDirectory fixture = new())
        {
            (ThirdPartyManifest manifest, _) = WriteFixture(fixture);
            Directory.CreateDirectory(fixture.FilePath("empty"));
            await ExpectNothingInstalled(manifest, fixture.FilePath("store"), new() { StoreRoot = fixture.FilePath("store"), Mirror = fixture.FilePath("empty") });
        }
    }

    private static async Task InterruptedInstallRetries()
    {
        using TemporaryDirectory fixture = new();
        (ThirdPartyManifest manifest, string mirror) = WriteFixture(fixture);
        string store = fixture.FilePath("store");
        ThirdPartyArchive archive = manifest.Binary!.Host(Host).Archive!;
        byte[] bytes = File.ReadAllBytes(Path.Combine(mirror, archive.FileName));

        // 받는 도중 끊김: 일부만 쓰고 예외. 최종 디렉터리와 임시 디렉터리가 남지 않는다.
        await Verify.ThrowsAsync<IOException>(() => ThirdPartyStore.InstallAsync(manifest, Host, new()
        {
            StoreRoot = store,
            Fetch = (_, destination, _) =>
            {
                File.WriteAllBytes(destination, bytes[..(bytes.Length / 2)]);
                throw new IOException("connection reset");
            }
        }));
        Verify.That(!Directory.Exists(Path.Combine(store, archive.Sha256)), "interrupted install published a store directory");
        Verify.That(!Directory.EnumerateDirectories(store, ThirdPartyStore.PartialPrefix + "*").Any(), "interrupted install left a temporary directory");

        // 강제 종료된 이전 실행이 남긴 임시 디렉터리: 다음 실행이 지우고 처음부터 받는다.
        string stale = Path.Combine(store, ThirdPartyStore.PartialPrefix + "killed");
        Directory.CreateDirectory(Path.Combine(stale, "content", "bin"));
        File.WriteAllBytes(Path.Combine(stale, archive.FileName), bytes[..10]);
        File.WriteAllText(Path.Combine(stale, "content", "bin", "fake.dll"), "partial");
        ThirdPartyStoreEntry? retried = await ThirdPartyStore.InstallAsync(manifest, Host, new()
        {
            StoreRoot = store,
            Fetch = (_, destination, _) =>
            {
                File.WriteAllBytes(destination, bytes);
                return Task.CompletedTask;
            }
        });
        Verify.Equal(ThirdPartyStoreStatus.Installed, retried!.Status, "retry status");
        Verify.That(!Directory.Exists(stale), "stale temporary directory was not removed");
        Verify.Equal(0, ThirdPartyStore.Verify(store, manifest, manifest.Binary.Host(Host)).Count, "retried store must verify");

        // 취소: 받은 뒤 취소되면 최종 디렉터리를 만들지 않는다.
        string cancelledStore = fixture.FilePath("cancelled");
        using var cancellation = new CancellationTokenSource();
        await Verify.ThrowsAsync<OperationCanceledException>(() => ThirdPartyStore.InstallAsync(manifest, Host, new()
        {
            StoreRoot = cancelledStore,
            Fetch = (_, destination, _) =>
            {
                File.WriteAllBytes(destination, bytes);
                cancellation.Cancel();
                return Task.CompletedTask;
            }
        }, cancellation.Token));
        Verify.Equal(0, Directory.GetDirectories(cancelledStore).Length, "cancelled install left directories");
    }

    private static async Task HostLockAndArchiveControls()
    {
        // build 출처 manifest의 호스트 누락은 오류다(다른 호스트 산출물로 대신하지 않는다).
        using (TemporaryDirectory fixture = new())
        {
            (ThirdPartyManifest manifest, string mirror) = WriteFixture(fixture);
            await Verify.ThrowsAsync<FormatException>(() =>
                ThirdPartyStore.InstallAsync(manifest, "macos-arm64", new() { StoreRoot = fixture.FilePath("store"), Mirror = mirror }));
        }

        // 다른 bootstrap이 저장소 잠금을 잡고 있으면 기다리지 않고 실패한다.
        using (TemporaryDirectory fixture = new())
        {
            (ThirdPartyManifest manifest, string mirror) = WriteFixture(fixture);
            string store = fixture.FilePath("store");
            Directory.CreateDirectory(store);
            using (new FileStream(Path.Combine(store, ThirdPartyStore.LockFileName), FileMode.OpenOrCreate, FileAccess.ReadWrite, FileShare.None))
            {
                await Verify.ThrowsAsync<InvalidOperationException>(() =>
                    ThirdPartyStore.InstallAsync(manifest, Host, new() { StoreRoot = store, Mirror = mirror }));
            }

            Verify.Equal(ThirdPartyStoreStatus.Installed,
                (await ThirdPartyStore.InstallAsync(manifest, Host, new() { StoreRoot = store, Mirror = mirror }))!.Status, "install after lock release");
        }

        // archive 항목이 저장소 디렉터리 밖을 가리키거나 링크이면 거부한다.
        foreach ((string label, TarEntry entry) in new (string, TarEntry)[]
                 {
                     ("parent path", new PaxTarEntry(TarEntryType.RegularFile, "../escape.txt") { DataStream = new MemoryStream("x"u8.ToArray()) }),
                     ("symbolic link", new PaxTarEntry(TarEntryType.SymbolicLink, "bin/link.dll") { LinkName = "fake.dll" })
                 })
        {
            using TemporaryDirectory fixture = new();
            (ThirdPartyManifest manifest, string mirror) = WriteFixture(fixture, extraEntry: entry);
            try
            {
                await ExpectNothingInstalled(manifest, fixture.FilePath("store"), new() { StoreRoot = fixture.FilePath("store"), Mirror = mirror });
            }
            catch (InvalidOperationException error)
            {
                throw new InvalidOperationException($"archive control '{label}': {error.Message}", error);
            }

            Verify.That(!File.Exists(fixture.FilePath("escape.txt")) && !File.Exists(fixture.FilePath("store/escape.txt")), "archive entry escaped: " + label);
        }

        // 비공개 저장소 대비: GitHub Release asset URL은 gh로 받을 저장소·태그·asset으로 나뉜다.
        Verify.That(ThirdPartyStore.GitHubReleaseAsset("https://github.com/owner/repo/releases/download/third-party-X-1.0-r1/X-1.0-r1-windows-x64.tar.gz") ==
                    ("owner/repo", "third-party-X-1.0-r1", "X-1.0-r1-windows-x64.tar.gz"), "release asset URL parse");
        Verify.That(ThirdPartyStore.GitHubReleaseAsset("https://example.invalid/X.tar.gz") is null &&
                    ThirdPartyStore.GitHubReleaseAsset("https://github.com/owner/repo/archive/abc.tar.gz") is null, "non-release URL parsed as asset");
    }

    private static async Task SdkAndSystemOrigins()
    {
        using TemporaryDirectory fixture = new();
        string sdk = fixture.FilePath("WindowsKits");
        byte[] dll = "fake d3dcompiler\n"u8.ToArray();
        Directory.CreateDirectory(Path.Combine(sdk, "Include", "10.0.26100.0"));
        Directory.CreateDirectory(Path.Combine(sdk, "Redist", "D3D", "x64"));
        File.WriteAllBytes(Path.Combine(sdk, "Redist", "D3D", "x64", "fake_47.dll"), dll);
        string sha = Convert.ToHexStringLower(SHA256.HashData(dll));
        ThirdPartyManifest manifest = Parse(fixture, "FakeSdk", $$"""
            { "schema": 1, "kind": "binary", "name": "FakeSdk", "version": "10.0.26100.1", "sdk": { "name": "Windows SDK", "version": "10.0.26100.0" },
              "hosts": { "windows-x64": { "files": [ { "path": "fake_47.dll", "sdkPath": "Redist/D3D/x64/fake_47.dll", "sha256": "{{sha}}", "size": {{dll.Length}}, "role": "runtime" } ] } },
              "notes": [ "fixture" ] }
            """);
        string store = fixture.FilePath("store");
        ThirdPartyStoreEntry? entry = await ThirdPartyStore.InstallAsync(manifest, Host, new() { StoreRoot = store, WindowsSdkRoot = sdk });
        Verify.That(entry is { Status: ThirdPartyStoreStatus.Installed } && File.ReadAllBytes(Path.Combine(store, sha, "fake_47.dll")).SequenceEqual(dll),
            "SDK file copied into the store");
        // sdk·system 출처는 호스트 전용일 수 있으므로 다른 호스트에서는 건너뛴다(null).
        Verify.That(await ThirdPartyStore.InstallAsync(manifest, "macos-arm64", new() { StoreRoot = store }) is null, "SDK binary on another host");

        // 설치된 SDK 파일이 고정 SHA와 다르거나 고정 버전이 없다.
        File.AppendAllText(Path.Combine(sdk, "Redist", "D3D", "x64", "fake_47.dll"), "x");
        await ExpectNothingInstalled(manifest, fixture.FilePath("store2"), new() { StoreRoot = fixture.FilePath("store2"), WindowsSdkRoot = sdk });
        File.WriteAllBytes(Path.Combine(sdk, "Redist", "D3D", "x64", "fake_47.dll"), dll);
        Directory.Delete(Path.Combine(sdk, "Include", "10.0.26100.0"));
        await ExpectNothingInstalled(manifest, fixture.FilePath("store3"), new() { StoreRoot = fixture.FilePath("store3"), WindowsSdkRoot = sdk });

        ThirdPartyManifest system = Parse(fixture, "FakeSystem", """
            { "schema": 1, "kind": "binary", "status": "system", "name": "FakeSystem", "minimumVersion": "32023",
              "hosts": { "macos-arm64": { "files": [ { "path": "metal", "role": "tool" } ] } }, "notes": [ "fixture" ] }
            """);
        Verify.That(await ThirdPartyStore.InstallAsync(system, "macos-arm64", new() { StoreRoot = store }) ==
                    new ThirdPartyStoreEntry("FakeSystem", ThirdPartyStoreStatus.System, null), "system tool is not stored");
    }

    private static async Task BootstrapRepository()
    {
        using TemporaryDirectory fixture = new();
        (_, string mirror) = WriteFixture(fixture);
        fixture.Write("Engine/Source/ThirdParty/FakeSystem/ThirdParty.json", """
            { "schema": 1, "kind": "binary", "status": "system", "name": "FakeSystem", "minimumVersion": "32023",
              "hosts": { "macos-arm64": { "files": [ { "path": "metal", "role": "tool" } ] } }, "notes": [ "fixture" ] }
            """);
        string store = fixture.FilePath("store");
        IReadOnlyList<ThirdPartyStoreEntry> entries = await ThirdPartyStore.BootstrapAsync(fixture.Root, Host, new() { StoreRoot = store, Mirror = mirror });
        Verify.That(entries.Select(entry => (entry.Library, entry.Status)).SequenceEqual([("FakeTool", ThirdPartyStoreStatus.Installed)]),
            "bootstrap entries: " + string.Join(", ", entries));
        entries = await ThirdPartyStore.BootstrapAsync(fixture.Root, Host, new() { StoreRoot = store, Offline = true });
        Verify.That(entries.Single().Status == ThirdPartyStoreStatus.Present, "offline bootstrap from the store");
        await Verify.ThrowsAsync<FormatException>(() => ThirdPartyStore.BootstrapAsync(fixture.Root, Host, new() { StoreRoot = store }, "Missing"));
    }

    private static void QuarantineAttribute()
    {
        // macOS 전용: 미러·브라우저로 받은 파일의 quarantine 속성을 지운다. 다른 OS에서는 속성이 없고 함수는 아무것도 하지 않는다.
        using TemporaryDirectory fixture = new();
        fixture.Write("file.dylib", "x");
        string path = fixture.FilePath("file.dylib");
        ThirdPartyStore.SetQuarantine(path);
        Verify.Equal(OperatingSystem.IsMacOS(), ThirdPartyStore.HasQuarantine(path), "quarantine attribute set");
        ThirdPartyStore.ClearQuarantine(path);
        ThirdPartyStore.ClearQuarantine(path);
        Verify.That(!ThirdPartyStore.HasQuarantine(path), "quarantine attribute was not removed");
    }

    // ---- fixture ----

    private static async Task ExpectNothingInstalled(ThirdPartyManifest manifest, string store, ThirdPartyStoreOptions options)
    {
        await Verify.ThrowsAsync<InvalidOperationException>(() => ThirdPartyStore.InstallAsync(manifest, Host, options));
        string[] left = Directory.Exists(store) ? Directory.GetDirectories(store) : [];
        Verify.That(left.Length == 0, "failed install left directories: " + string.Join(", ", left));
    }

    // archive를 만들어 미러 디렉터리에 두고 그 SHA-256을 적은 binary manifest를 쓴다. change는 manifest JSON을 바꾼다.
    private static (ThirdPartyManifest Manifest, string Mirror) WriteFixture(TemporaryDirectory fixture, Action<JsonObject>? change = null,
        TarEntry? extraEntry = null)
    {
        const string asset = "FakeTool-2.0-r1-windows-x64.tar.gz";
        string mirror = fixture.FilePath("mirror");
        Directory.CreateDirectory(mirror);
        using (FileStream file = File.Create(Path.Combine(mirror, asset)))
        using (var gzip = new GZipStream(file, CompressionLevel.Optimal))
        using (var tar = new TarWriter(gzip, TarEntryFormat.Pax))
        {
            tar.WriteEntry(new PaxTarEntry(TarEntryType.Directory, "bin/"));
            foreach ((string path, string text, _) in ArchiveFiles)
            {
                tar.WriteEntry(new PaxTarEntry(TarEntryType.RegularFile, path) { DataStream = new MemoryStream(Encoding.UTF8.GetBytes(text)) });
            }

            if (extraEntry is not null)
            {
                tar.WriteEntry(extraEntry);
            }
        }

        byte[] archive = File.ReadAllBytes(Path.Combine(mirror, asset));
        JsonObject manifest = JsonNode.Parse($$"""
            {
              "schema": 1, "kind": "binary", "name": "FakeTool", "version": "2.0", "upstreamDirectory": "faketool-2.0",
              "licenses": [ { "spdx": "MIT", "upstreamExpression": "MIT", "file": "faketool-2.0/LICENSE", "binaryNoticeRequired": true } ],
              "includeDirectories": [ "faketool-2.0/include" ], "consumerDefines": [], "adapterHeaders": [ "faketool-2.0/include/fake.h" ],
              "files": { "LICENSE": "{{new string('e', 64)}}" }, "pragmaDirectives": [],
              "build": { "tag": "v2.0", "revision": 1,
                "source": { "repository": "https://example.invalid/faketool", "commit": "{{new string('c', 40)}}", "url": "https://example.invalid/faketool.tar.gz", "sha256": "{{new string('b', 64)}}" },
                "dependencies": [], "scripts": [ "Build/Build.ps1" ], "patches": [] },
              "hosts": { "windows-x64": {
                "archive": { "url": "https://example.invalid/releases/download/third-party-FakeTool-2.0-r1/{{asset}}", "sha256": "{{Convert.ToHexStringLower(SHA256.HashData(archive))}}", "size": {{archive.Length}} },
                "files": [] } },
              "notes": []
            }
            """)!.AsObject();
        foreach ((string path, string text, string role) in ArchiveFiles)
        {
            byte[] bytes = Encoding.UTF8.GetBytes(text);
            Files(manifest).Add(new JsonObject
            {
                ["path"] = path,
                ["sha256"] = Convert.ToHexStringLower(SHA256.HashData(bytes)),
                ["size"] = bytes.Length,
                ["role"] = role
            });
        }

        change?.Invoke(manifest);
        fixture.Write(Library + "/ThirdParty.json", manifest.ToJsonString(new JsonSerializerOptions { WriteIndented = true }));
        return (ThirdPartyManifest.Read(fixture.FilePath(Library)), mirror);
    }

    private static JsonArray Files(JsonObject manifest) => manifest["hosts"]![Host]!["files"]!.AsArray();

    private static JsonObject HostFile(JsonObject manifest, int index) => Files(manifest)[index]!.AsObject();

    private static ThirdPartyManifest Parse(TemporaryDirectory fixture, string name, string json)
    {
        fixture.Write($"Engine/Source/ThirdParty/{name}/ThirdParty.json", json);
        return ThirdPartyManifest.Read(fixture.FilePath($"Engine/Source/ThirdParty/{name}"));
    }
}
