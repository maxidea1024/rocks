using System.Diagnostics;
using System.Formats.Tar;
using System.IO.Compression;
using System.Runtime.InteropServices;
using System.Security.Cryptography;

namespace Rocks.Tooling.Common;

/// <summary>
/// bootstrap 한 라이브러리의 결과다. <see cref="Directory"/>는 저장소 디렉터리(system 도구는 null)다.
/// </summary>
/// <param name="Library">라이브러리 이름.</param>
/// <param name="Status">Installed(이번에 받아 넣음)·Present(이미 있고 검사 통과)·System(저장소에 두지 않는 시스템 도구).</param>
/// <param name="Directory">저장소 디렉터리의 절대 경로.</param>
public sealed record ThirdPartyStoreEntry(string Library, ThirdPartyStoreStatus Status, string? Directory);

/// <summary>bootstrap 결과 상태다.</summary>
public enum ThirdPartyStoreStatus { Installed, Present, System }

/// <summary>
/// bootstrap 설정이다.
/// <list type="bullet">
/// <item><see cref="StoreRoot"/>: 저장소 루트. 기본은 <see cref="ThirdPartyStore.DefaultRoot"/>.</item>
/// <item><see cref="Mirror"/>: asset 이름(<see cref="ThirdPartyArchive.FileName"/>)으로 archive를 찾는 로컬·공유 디렉터리. 있으면 네트워크 대신
/// 쓴다(브라우저로 받은 파일 포함).</item>
/// <item><see cref="Offline"/>: 받지 않는다. 저장소에 이미 있고 검사를 통과한 것만 쓰고, 없으면 오류다.</item>
/// <item><see cref="WindowsSdkRoot"/>: Windows SDK 루트(<c>Windows Kits\10</c>). 없으면 환경(<c>WindowsSdkDir</c>)·레지스트리·기본 위치에서 찾는다.</item>
/// <item><see cref="Fetch"/>: archive를 받는 함수(시험 주입용). 없으면 HTTPS로 받고, GitHub Release asset이 인증 없이 받아지지 않으면(비공개
/// 저장소) <c>gh release download</c>로 받는다.</item>
/// </list>
/// </summary>
public sealed record ThirdPartyStoreOptions
{
    public required string StoreRoot { get; init; }

    public string? Mirror { get; init; }

    public bool Offline { get; init; }

    public string? WindowsSdkRoot { get; init; }

    public Func<ThirdPartyArchive, string, CancellationToken, Task>? Fetch { get; init; }
}

/// <summary>
/// 미리 빌드한 바이너리(manifest <c>kind: binary</c>)의 사용자 단위 내용 주소 저장소다(Docs/Design/ShaderSystem 8절 2항).
/// <list type="bullet">
/// <item>위치: Windows <c>%LOCALAPPDATA%\Rocks\ThirdParty\&lt;키&gt;\</c>, macOS <c>~/Library/Application Support/Rocks/ThirdParty/&lt;키&gt;/</c>. 키는
/// archive SHA-256(build 출처) 또는 파일 SHA-256(sdk 출처)이다(<see cref="ThirdPartyHost.StoreKey"/>). archive 단위로 풀어 두므로 한
/// 도구의 공유 라이브러리가 같은 디렉터리의 다른 라이브러리를 찾는다.</item>
/// <item>명시적인 bootstrap 명령에서만 받는다(UE GitDependencies와 같은 역할, SHA-256 검사). 빌드 action과 워커는 받지 않고
/// <see cref="Verify"/>로 확인만 한다.</item>
/// <item>순서: 저장소 아래 임시 디렉터리(<c>.partial-*</c>)에 받기 → archive SHA-256·크기 검사 → 풀기 → 파일 집합·SHA-256·크기 검사 →
/// (macOS) quarantine 속성 제거 → 최종 이름으로 이동. 이동이 마지막이므로 중간에 끊겨도 최종 디렉터리는 생기지 않고, 다음 실행이
/// 남은 임시 디렉터리를 지운 뒤 처음부터 다시 받는다.</item>
/// <item>동시 실행: 저장소의 잠금 파일(<c>.lock</c>)을 배타로 연다. 다른 bootstrap이 잡고 있으면 기다리지 않고 오류다.</item>
/// </list>
/// 사용 예: <c>await ThirdPartyStore.InstallAsync(manifest, ThirdPartyManifest.CurrentHostKey(), new() { StoreRoot = ThirdPartyStore.DefaultRoot() })</c>.
/// </summary>
public static class ThirdPartyStore
{
    /// <summary>임시 디렉터리 이름 접두사다. 잠금을 잡은 bootstrap만 만들고 지운다.</summary>
    public const string PartialPrefix = ".partial-";

    /// <summary>저장소 잠금 파일 이름이다.</summary>
    public const string LockFileName = ".lock";

    /// <summary>macOS가 받은 파일에 붙이는 quarantine 확장 속성 이름이다(있으면 Gatekeeper가 dlopen을 막는다).</summary>
    public const string QuarantineAttribute = "com.apple.quarantine";

    /// <summary>
    /// 기본 저장소 루트다. OS가 지울 수 있는 캐시 위치(<c>Caches</c>)는 쓰지 않는다.
    /// </summary>
    /// <exception cref="PlatformNotSupportedException">Windows·macOS가 아니다.</exception>
    public static string DefaultRoot()
    {
        if (OperatingSystem.IsWindows())
        {
            return Path.Combine(Environment.GetFolderPath(Environment.SpecialFolder.LocalApplicationData), "Rocks", "ThirdParty");
        }

        if (OperatingSystem.IsMacOS())
        {
            return Path.Combine(Environment.GetFolderPath(Environment.SpecialFolder.UserProfile), "Library", "Application Support", "Rocks", "ThirdParty");
        }

        throw new PlatformNotSupportedException("the third-party binary store exists only on Windows and macOS development hosts");
    }

    /// <summary>
    /// 호스트 항목의 저장소 디렉터리 경로다(존재 여부는 보지 않는다).
    /// </summary>
    /// <exception cref="ArgumentException">system 도구다(저장소에 두지 않는다).</exception>
    public static string DirectoryOf(string storeRoot, ThirdPartyHost host) =>
        Path.Combine(storeRoot, host.StoreKey ?? throw new ArgumentException($"host '{host.Key}' is a system tool and has no store directory", nameof(host)));

    /// <summary>
    /// 저장소 디렉터리의 파일 집합·SHA-256·크기가 manifest와 같은지 본다. 받지 않는다(워커·빌드가 쓰는 확인).
    /// </summary>
    /// <returns>문제 목록. 비어 있으면 그대로 쓸 수 있다. 디렉터리가 없으면 bootstrap을 안내하는 항목 하나다.</returns>
    public static IReadOnlyList<string> Verify(string storeRoot, ThirdPartyManifest manifest, ThirdPartyHost host)
    {
        string directory = DirectoryOf(storeRoot, host);
        if (!Directory.Exists(directory))
        {
            return [$"{manifest.Name}: {directory} is missing (run the bootstrap command: Rocks.Build.Cli bootstrap --root <repository>)"];
        }

        return VerifyFiles(manifest.Name, directory, host.Files);
    }

    /// <summary>
    /// 저장소 루트의 binary 라이브러리(두 외부 라이브러리 루트, 루트·이름 순)를 모두, 또는 <paramref name="library"/> 하나를 bootstrap한다.
    /// 호스트 항목이 없는 sdk·system 라이브러리(호스트 전용)는 결과에서 빠진다. 사용 예(명령):
    /// <c>Rocks.Build.Cli bootstrap --root . [--library DirectXShaderCompiler] [--mirror &lt;dir&gt;] [--offline true]</c>.
    /// </summary>
    /// <exception cref="FormatException">manifest 오류, 없는 라이브러리, binary가 아닌 라이브러리 지정, build 출처의 호스트 누락.</exception>
    /// <exception cref="InvalidOperationException">검사 실패·오프라인 누락 등(<see cref="InstallAsync"/>).</exception>
    public static async Task<IReadOnlyList<ThirdPartyStoreEntry>> BootstrapAsync(string repositoryRoot, string hostKey, ThirdPartyStoreOptions options,
        string? library = null, CancellationToken cancellationToken = default)
    {
        string root = Path.GetFullPath(repositoryRoot);
        IReadOnlyList<string> libraries = library is null
            ? ThirdPartyManifest.LibraryDirectories(root)
            : [Path.Combine(root, ThirdPartyManifest.FindLibrary(root, library))];
        List<ThirdPartyStoreEntry> entries = [];
        foreach (string directory in libraries)
        {
            ThirdPartyManifest manifest = ThirdPartyManifest.Read(directory);
            if (manifest.Kind != ThirdPartyKind.Binary)
            {
                if (library is not null)
                {
                    throw new FormatException($"{library}: not a binary library (kind {manifest.Kind})");
                }

                continue;
            }

            if (await InstallAsync(manifest, hostKey, options, cancellationToken) is ThirdPartyStoreEntry entry)
            {
                entries.Add(entry);
            }
        }

        return entries;
    }

    /// <summary>
    /// 한 라이브러리의 현재 호스트 파일을 저장소에 넣는다. 이미 있으면 검사만 한다.
    /// </summary>
    /// <param name="manifest">binary manifest.</param>
    /// <param name="hostKey">호스트 키(<see cref="ThirdPartyManifest.CurrentHostKey"/>).</param>
    /// <param name="options">저장소·미러·오프라인 설정.</param>
    /// <param name="cancellationToken">취소. 취소되면 임시 디렉터리를 지우고 최종 디렉터리는 만들지 않는다.</param>
    /// <returns>결과. 호스트 항목이 없으면 null이다(sdk·system 출처는 호스트 전용일 수 있다). build 출처의 호스트 누락은 오류다.</returns>
    /// <exception cref="InvalidOperationException">SHA-256·크기·파일 집합 불일치, 오프라인인데 없음, 다른 bootstrap 실행 중, 손상된 저장소 디렉터리.</exception>
    /// <exception cref="FormatException">build 출처 manifest가 이 호스트를 적지 않았다(호스트 누락).</exception>
    public static async Task<ThirdPartyStoreEntry?> InstallAsync(ThirdPartyManifest manifest, string hostKey, ThirdPartyStoreOptions options,
        CancellationToken cancellationToken = default)
    {
        ThirdPartyBinary binary = manifest.Binary ?? throw new ArgumentException($"{manifest.Name}: not a binary library", nameof(manifest));
        if (!binary.Hosts.ContainsKey(hostKey) && binary.Origin != ThirdPartyBinaryOrigin.Build)
        {
            return null;
        }

        ThirdPartyHost host = binary.Host(hostKey);
        if (binary.Origin == ThirdPartyBinaryOrigin.System)
        {
            return new(manifest.Name, ThirdPartyStoreStatus.System, null);
        }

        string storeRoot = Path.GetFullPath(options.StoreRoot);
        string directory = DirectoryOf(storeRoot, host);
        if (Directory.Exists(directory))
        {
            return Present(manifest, host, directory);
        }

        if (options.Offline)
        {
            throw new InvalidOperationException($"{manifest.Name}: {directory} is missing and network access is off (run bootstrap without --offline)");
        }

        Directory.CreateDirectory(storeRoot);
        using FileStream storeLock = Lock(storeRoot);
        RemovePartials(storeRoot);
        if (Directory.Exists(directory))
        {
            return Present(manifest, host, directory); // 잠금을 기다리지 않으므로 생기지 않지만, 잠금 뒤 다시 본다.
        }

        string partial = Path.Combine(storeRoot, PartialPrefix + Guid.NewGuid().ToString("N"));
        string content = Path.Combine(partial, "content");
        Directory.CreateDirectory(content);
        try
        {
            if (host.Archive is ThirdPartyArchive archive)
            {
                string downloaded = Path.Combine(partial, archive.FileName);
                await AcquireAsync(archive, downloaded, options, cancellationToken);
                CheckDigest(manifest.Name, archive.FileName, downloaded, archive.Sha256, archive.Size);
                Extract(manifest.Name, downloaded, content);
            }
            else
            {
                CopyFromSdk(manifest, host, content, options);
            }

            IReadOnlyList<string> problems = VerifyFiles(manifest.Name, content, host.Files);
            if (problems.Count != 0)
            {
                throw new InvalidOperationException(string.Join('\n', problems));
            }

            RemoveQuarantine(content);
            cancellationToken.ThrowIfCancellationRequested();
            Directory.Move(content, directory);
        }
        finally
        {
            Directory.Delete(partial, recursive: true);
        }

        return new(manifest.Name, ThirdPartyStoreStatus.Installed, directory);
    }

    private static ThirdPartyStoreEntry Present(ThirdPartyManifest manifest, ThirdPartyHost host, string directory)
    {
        IReadOnlyList<string> problems = VerifyFiles(manifest.Name, directory, host.Files);
        if (problems.Count != 0)
        {
            // 내용 주소 디렉터리의 내용이 다르면 손상·변조다. 고치지 않고 알린다(지우면 다음 bootstrap이 다시 받는다).
            throw new InvalidOperationException($"{manifest.Name}: store directory {directory} does not match the manifest " +
                                                $"(delete it and run bootstrap again):\n{string.Join('\n', problems)}");
        }

        return new(manifest.Name, ThirdPartyStoreStatus.Present, directory);
    }

    private static FileStream Lock(string storeRoot)
    {
        try
        {
            return new FileStream(Path.Combine(storeRoot, LockFileName), FileMode.OpenOrCreate, FileAccess.ReadWrite, FileShare.None);
        }
        catch (IOException error)
        {
            throw new InvalidOperationException($"another bootstrap is using {storeRoot} (it does not wait; run again after it finishes)", error);
        }
    }

    // 잠금을 잡은 뒤에만 부른다: 남은 임시 디렉터리는 끊긴 이전 실행의 것이다.
    private static void RemovePartials(string storeRoot)
    {
        foreach (string partial in Directory.EnumerateDirectories(storeRoot, PartialPrefix + "*"))
        {
            Directory.Delete(partial, recursive: true);
        }
    }

    private static async Task AcquireAsync(ThirdPartyArchive archive, string destination, ThirdPartyStoreOptions options,
        CancellationToken cancellationToken)
    {
        if (options.Mirror is string mirror)
        {
            string source = Path.Combine(mirror, archive.FileName);
            if (!File.Exists(source))
            {
                throw new InvalidOperationException($"mirror {mirror} has no {archive.FileName}");
            }

            File.Copy(source, destination);
            return;
        }

        if (options.Fetch is not null)
        {
            await options.Fetch(archive, destination, cancellationToken);
            return;
        }

        await DownloadAsync(archive, destination, cancellationToken);
    }

    private static async Task DownloadAsync(ThirdPartyArchive archive, string destination, CancellationToken cancellationToken)
    {
        using var client = new HttpClient();
        using HttpResponseMessage response = await client.GetAsync(archive.Url, HttpCompletionOption.ResponseHeadersRead, cancellationToken);
        if (response.IsSuccessStatusCode)
        {
            await using Stream body = await response.Content.ReadAsStreamAsync(cancellationToken);
            await using var file = new FileStream(destination, FileMode.CreateNew, FileAccess.Write, FileShare.None);
            await body.CopyToAsync(file, cancellationToken);
            return;
        }

        // 비공개 저장소의 Release asset은 인증 없이 404다. gh가 사용자의 인증으로 받는다.
        if (GitHubReleaseAsset(archive.Url) is (string repository, string tag, string asset) &&
            response.StatusCode is System.Net.HttpStatusCode.NotFound or System.Net.HttpStatusCode.Unauthorized or System.Net.HttpStatusCode.Forbidden)
        {
            await GhDownloadAsync(repository, tag, asset, destination, cancellationToken);
            return;
        }

        throw new InvalidOperationException($"download failed: {archive.Url} returned {(int)response.StatusCode}");
    }

    /// <summary>
    /// GitHub Release asset URL(<c>https://github.com/&lt;owner&gt;/&lt;repo&gt;/releases/download/&lt;tag&gt;/&lt;asset&gt;</c>)을 나눈다. 아니면 null이다.
    /// </summary>
    public static (string Repository, string Tag, string Asset)? GitHubReleaseAsset(string url)
    {
        const string prefix = "https://github.com/";
        if (!url.StartsWith(prefix, StringComparison.Ordinal))
        {
            return null;
        }

        string[] parts = url[prefix.Length..].Split('/');
        return parts is [string owner, string repository, "releases", "download", string tag, string asset] &&
               new[] { owner, repository, tag, asset }.All(part => part.Length != 0)
            ? ($"{owner}/{repository}", Uri.UnescapeDataString(tag), Uri.UnescapeDataString(asset))
            : null;
    }

    private static async Task GhDownloadAsync(string repository, string tag, string asset, string destination, CancellationToken cancellationToken)
    {
        string directory = Path.GetDirectoryName(destination)!;
        var start = new ProcessStartInfo("gh") { RedirectStandardOutput = true, RedirectStandardError = true, UseShellExecute = false };
        foreach (string argument in new[] { "release", "download", tag, "--repo", repository, "--pattern", asset, "--dir", directory })
        {
            start.ArgumentList.Add(argument);
        }

        Process process;
        try
        {
            process = Process.Start(start) ?? throw new InvalidOperationException("gh did not start");
        }
        catch (System.ComponentModel.Win32Exception error)
        {
            throw new InvalidOperationException($"{asset}: the release is not public and GitHub CLI (gh) is not available to download it with your login", error);
        }

        using (process)
        {
            Task<string> output = process.StandardOutput.ReadToEndAsync(cancellationToken);
            Task<string> errors = process.StandardError.ReadToEndAsync(cancellationToken);
            await process.WaitForExitAsync(cancellationToken);
            await output;
            if (process.ExitCode != 0)
            {
                throw new InvalidOperationException($"gh release download {tag} --repo {repository} failed ({process.ExitCode}): {(await errors).Trim()} (run 'gh auth login')");
            }
        }

        if (!File.Exists(Path.Combine(directory, asset)) || Path.Combine(directory, asset) != destination)
        {
            throw new InvalidOperationException($"gh did not produce {destination}");
        }
    }

    private static void CheckDigest(string library, string name, string path, string sha256, long size)
    {
        long actualSize = new FileInfo(path).Length;
        string actual = Sha256File(path);
        if (actualSize != size || actual != sha256)
        {
            throw new InvalidOperationException($"{library}: {name} SHA-256/size mismatch (expected {sha256}/{size}, got {actual}/{actualSize}); not installed");
        }
    }

    private static string Sha256File(string path)
    {
        using FileStream stream = File.OpenRead(path);
        return Convert.ToHexStringLower(SHA256.HashData(stream));
    }

    // tar.gz를 푼다. 일반 파일과 디렉터리만 받고, 절대 경로·'..'·링크는 거부한다(manifest가 일반 파일만 적는다).
    private static void Extract(string library, string archive, string destination)
    {
        string root = Path.GetFullPath(destination) + Path.DirectorySeparatorChar;
        using FileStream file = File.OpenRead(archive);
        using var gzip = new GZipStream(file, CompressionMode.Decompress);
        using var reader = new TarReader(gzip);
        while (reader.GetNextEntry() is TarEntry entry)
        {
            string name = entry.Name.Replace('\\', '/');
            if (name.StartsWith('/') || name.Split('/').Any(part => part is ".."))
            {
                throw new InvalidOperationException($"{library}: archive entry '{entry.Name}' escapes the store directory");
            }

            string target = Path.GetFullPath(Path.Combine(destination, name));
            if (!target.StartsWith(root, StringComparison.Ordinal) && target + Path.DirectorySeparatorChar != root)
            {
                throw new InvalidOperationException($"{library}: archive entry '{entry.Name}' escapes the store directory");
            }

            switch (entry.EntryType)
            {
                case TarEntryType.Directory:
                    Directory.CreateDirectory(target);
                    break;
                case TarEntryType.RegularFile or TarEntryType.V7RegularFile:
                    Directory.CreateDirectory(Path.GetDirectoryName(target)!);
                    entry.ExtractToFile(target, overwrite: false);
                    break;
                case TarEntryType.GlobalExtendedAttributes or TarEntryType.ExtendedAttributes:
                    break;
                default:
                    throw new InvalidOperationException($"{library}: archive entry '{entry.Name}' is a {entry.EntryType} (only regular files are allowed)");
            }
        }
    }

    private static void CopyFromSdk(ThirdPartyManifest manifest, ThirdPartyHost host, string content, ThirdPartyStoreOptions options)
    {
        ThirdPartySdk sdk = manifest.Binary!.Sdk!;
        string root = options.WindowsSdkRoot ?? WindowsSdkRoot()
            ?? throw new InvalidOperationException($"{manifest.Name}: {sdk.Name} {sdk.Version} is not installed");
        if (!Directory.Exists(Path.Combine(root, "Include", sdk.Version)))
        {
            throw new InvalidOperationException($"{manifest.Name}: {sdk.Name} {sdk.Version} is not installed under {root} (install that pinned version)");
        }

        foreach (ThirdPartyHostFile item in host.Files)
        {
            string source = Path.Combine(root, item.SdkPath!);
            if (!File.Exists(source))
            {
                throw new InvalidOperationException($"{manifest.Name}: {source} is missing ({sdk.Name} {sdk.Version})");
            }

            string target = Path.Combine(content, item.Path);
            Directory.CreateDirectory(Path.GetDirectoryName(target)!);
            File.Copy(source, target);
            CheckDigest(manifest.Name, item.SdkPath!, target, item.Sha256!, item.Size!.Value);
        }
    }

    // Windows SDK 루트: 환경 WindowsSdkDir → 레지스트리 KitsRoot10 → 기본 설치 위치.
    private static string? WindowsSdkRoot()
    {
        if (!OperatingSystem.IsWindows())
        {
            return null;
        }

        if (Environment.GetEnvironmentVariable("WindowsSdkDir") is string fromEnvironment && Directory.Exists(fromEnvironment))
        {
            return fromEnvironment;
        }

        foreach (string key in new[] { @"SOFTWARE\Microsoft\Windows Kits\Installed Roots", @"SOFTWARE\WOW6432Node\Microsoft\Windows Kits\Installed Roots" })
        {
            using Microsoft.Win32.RegistryKey? registry = Microsoft.Win32.Registry.LocalMachine.OpenSubKey(key);
            if (registry?.GetValue("KitsRoot10") is string fromRegistry && Directory.Exists(fromRegistry))
            {
                return fromRegistry;
            }
        }

        string fallback = Path.Combine(Environment.GetFolderPath(Environment.SpecialFolder.ProgramFilesX86), "Windows Kits", "10");
        return Directory.Exists(fallback) ? fallback : null;
    }

    private static IReadOnlyList<string> VerifyFiles(string library, string directory, IReadOnlyList<ThirdPartyHostFile> files)
    {
        List<string> problems = [];
        var expected = files.ToDictionary(file => file.Path, StringComparer.Ordinal);
        var seen = new HashSet<string>(StringComparer.Ordinal);
        foreach (string path in Directory.EnumerateFiles(directory, "*", SearchOption.AllDirectories))
        {
            string relative = Path.GetRelativePath(directory, path).Replace('\\', '/');
            if (!expected.TryGetValue(relative, out ThirdPartyHostFile? file))
            {
                problems.Add($"{library}: {relative} is not listed in the manifest");
                continue;
            }

            seen.Add(relative);
            long size = new FileInfo(path).Length;
            string sha256 = Sha256File(path);
            if (size != file.Size || sha256 != file.Sha256)
            {
                problems.Add($"{library}: {relative} SHA-256/size mismatch (expected {file.Sha256}/{file.Size}, got {sha256}/{size})");
            }
        }

        foreach (string missing in expected.Keys.Where(path => !seen.Contains(path)).Order(StringComparer.Ordinal))
        {
            problems.Add($"{library}: {missing} is missing");
        }

        return problems;
    }

    // macOS: 미러·브라우저로 받은 파일의 quarantine 속성을 지운다(SHA 검사 뒤). 다른 확장 속성과 파일 내용은 고치지 않는다.
    private static void RemoveQuarantine(string directory)
    {
        if (!OperatingSystem.IsMacOS())
        {
            return;
        }

        foreach (string path in Directory.EnumerateFileSystemEntries(directory, "*", SearchOption.AllDirectories))
        {
            ClearQuarantine(path);
        }
    }

    /// <summary>
    /// macOS에서 경로의 quarantine 확장 속성을 지운다(없으면 아무것도 하지 않는다). 다른 OS에서는 아무것도 하지 않는다.
    /// </summary>
    /// <exception cref="IOException">속성을 지우지 못했다.</exception>
    public static void ClearQuarantine(string path)
    {
        if (!OperatingSystem.IsMacOS())
        {
            return;
        }

        if (MacAttributes.removexattr(path, QuarantineAttribute, MacAttributes.NoFollow) != 0)
        {
            int error = Marshal.GetLastPInvokeError();
            if (error != MacAttributes.NoAttribute)
            {
                throw new IOException($"removexattr({path}, {QuarantineAttribute}) failed with errno {error}");
            }
        }
    }

    /// <summary>macOS에서 경로에 quarantine 확장 속성이 있는지다(시험용). 다른 OS에서는 false다.</summary>
    public static bool HasQuarantine(string path) =>
        OperatingSystem.IsMacOS() && MacAttributes.getxattr(path, QuarantineAttribute, IntPtr.Zero, 0, 0, MacAttributes.NoFollow) >= 0;

    /// <summary>macOS에서 경로에 quarantine 확장 속성을 붙인다(시험 fixture용). 다른 OS에서는 아무것도 하지 않는다.</summary>
    public static void SetQuarantine(string path)
    {
        if (!OperatingSystem.IsMacOS())
        {
            return;
        }

        byte[] value = "0081;00000000;Rocks;"u8.ToArray();
        if (MacAttributes.setxattr(path, QuarantineAttribute, value, value.Length, 0, MacAttributes.NoFollow) != 0)
        {
            throw new IOException($"setxattr({path}, {QuarantineAttribute}) failed with errno {Marshal.GetLastPInvokeError()}");
        }
    }

    private static class MacAttributes
    {
        // <sys/xattr.h> XATTR_NOFOLLOW, <sys/errno.h> ENOATTR(macOS 93).
        public const int NoFollow = 0x0001;

        public const int NoAttribute = 93;

        [DllImport("libc", SetLastError = true)]
        public static extern int removexattr(string path, string name, int options);

        [DllImport("libc", SetLastError = true)]
        public static extern long getxattr(string path, string name, IntPtr value, nint size, uint position, int options);

        [DllImport("libc", SetLastError = true)]
        public static extern int setxattr(string path, string name, byte[] value, nint size, uint position, int options);
    }
}
