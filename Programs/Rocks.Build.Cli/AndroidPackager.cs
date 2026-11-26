using System.IO.Compression;
using System.Security;
using Rocks.Build;
using Rocks.Tooling.Common;

/// <summary>
/// Android APK 패키징이다(Docs/Design/AndroidPlatform.ko.md 7절, AD11). Gradle 없이 SDK 도구로 만든다: manifest 생성 → aapt2 link →
/// <c>lib/&lt;abi&gt;/lib&lt;Program&gt;.so</c>를 비압축으로 추가 → zipalign(16 KB 페이지) → apksigner(Debug keystore). Java 코드가 없으므로 dex
/// 단계가 없다. 출력은 <c>bin/&lt;Program&gt;.apk</c>다.
/// </summary>
internal static class AndroidPackager
{
    // 정렬이다: zip 항목 4 byte, 비압축 .so는 16 KB 페이지(zipalign -P 16 — 단위 KB).
    private const string ZipAlignment = "4";
    private const string PageAlignmentKilobytes = "16";

    // Debug keystore다(Android SDK 관례 — 비밀번호·별칭이 고정값이다).
    private const string DebugKeystorePassword = "android";
    private const string DebugKeyAlias = "androiddebugkey";
    private const string DebugKeyName = "CN=Android Debug,O=Android,C=US";
    private const string DebugKeyValidityDays = "10000";
    private const string DebugKeySizeBits = "2048";

    // 도구 실행 상한이다(멈춘 도구를 끊는 장치다).
    private static readonly TimeSpan ToolTimeout = TimeSpan.FromMinutes(5);

    /// <summary>
    /// 링크된 <paramref name="library"/>를 APK로 묶어 <c>bin/&lt;module&gt;.apk</c>를 쓰고 그 경로를 돌려준다. <paramref name="abi"/>는 Rocks ABI
    /// 이름(arm64·x86_64), <paramref name="minimumApi"/>는 Toolchain의 Android API다. SDK·JDK가 없으면 <see cref="FileNotFoundException"/>이다.
    /// </summary>
    public static async Task<string> PackageAsync(NativeModuleInput module, string library, string output, string abi, int minimumApi,
        NativeConfiguration configuration, CancellationToken cancellationToken)
    {
        AndroidPackage package = module.AndroidPackage ?? throw new ArgumentException($"Module {module.Name} has no AndroidPackage.");
        string sdk = FindSdk() ?? throw new FileNotFoundException("Android SDK not found. Set ANDROID_HOME or ANDROID_SDK_ROOT.");
        string buildTools = LatestVersionDirectory(Path.Combine(sdk, "build-tools"), directory => File.Exists(Path.Combine(directory, Tool("aapt2"))))
            ?? throw new FileNotFoundException("Android SDK build-tools with aapt2 not found under " + sdk);
        (string platform, int targetApi) = LatestPlatform(Path.Combine(sdk, "platforms"))
            ?? throw new FileNotFoundException("Android SDK platform (android-<N>/android.jar) not found under " + sdk);
        string java = FindJava() ?? throw new FileNotFoundException("Java not found (apksigner and keytool need a JDK). Set JAVA_HOME.");

        string work = Directory.CreateDirectory(Path.Combine(output, "android", module.Name)).FullName;
        string manifest = Path.Combine(work, "AndroidManifest.xml");
        bool debuggable = configuration != NativeConfiguration.Release;
        AtomicFile.WriteIfChanged(manifest, Manifest(package, module.Name, minimumApi, targetApi));

        string unaligned = Path.Combine(work, module.Name + ".unaligned.apk");
        string aligned = Path.Combine(work, module.Name + ".aligned.apk");
        File.Delete(unaligned);
        File.Delete(aligned);
        var link = new List<string> { "link", "-o", unaligned, "-I", Path.Combine(platform, "android.jar"), "--manifest", manifest,
            "--min-sdk-version", minimumApi.ToString(System.Globalization.CultureInfo.InvariantCulture),
            "--target-sdk-version", targetApi.ToString(System.Globalization.CultureInfo.InvariantCulture) };
        if (debuggable)
        {
            link.Add("--debug-mode");
        }

        await RunAsync(Path.Combine(buildTools, Tool("aapt2")), link, work, null, cancellationToken);

        // NativeActivity가 APK 안의 .so를 바로 mmap하도록(extractNativeLibs=false) 비압축으로 넣는다.
        // assets/는 비압축으로 넣는다(AAsset을 fd·오프셋으로도 읽을 수 있게 — AndroidPlatform 7절).
        using (ZipArchive archive = ZipFile.Open(unaligned, ZipArchiveMode.Update))
        {
            archive.CreateEntryFromFile(library, "lib/" + AndroidAbiDirectory(abi) + "/" + AndroidPackage.LibraryFileName(module.Name),
                CompressionLevel.NoCompression);
            if (package.AssetsDirectory is string assets && Directory.Exists(assets))
            {
                foreach (string file in Directory.GetFiles(assets, "*", SearchOption.AllDirectories).Order(StringComparer.Ordinal))
                {
                    string relative = Path.GetRelativePath(assets, file).Replace('\\', '/');
                    archive.CreateEntryFromFile(file, "assets/" + relative, CompressionLevel.NoCompression);
                }
            }
        }

        await RunAsync(Path.Combine(buildTools, Tool("zipalign")), ["-P", PageAlignmentKilobytes, "-f", ZipAlignment, unaligned, aligned],
            work, null, cancellationToken);

        string keystore = await DebugKeystoreAsync(java, cancellationToken);
        string apk = Path.Combine(output, "bin", module.Name + ".apk");
        File.Delete(apk);
        string apksigner = Path.Combine(buildTools, "lib", "apksigner.jar");
        await RunAsync(java, ["-jar", apksigner, "sign", "--ks", keystore, "--ks-pass", "pass:" + DebugKeystorePassword,
            "--key-pass", "pass:" + DebugKeystorePassword, "--ks-key-alias", DebugKeyAlias, "--out", apk, aligned], work, null, cancellationToken);
        return apk;
    }

    // Android ABI 폴더 이름이다.
    internal static string AndroidAbiDirectory(string abi) => abi switch
    {
        "arm64" => "arm64-v8a",
        "x86_64" => "x86_64",
        _ => throw new ArgumentException("Unsupported Android ABI: " + abi)
    };

    // manifest다. 플랫폼 NativeActivity가 lib<module>.so를 싣는다. 회전·크기 변경에 액티비티를 다시 만들지 않는다(configChanges — UE
    // UEDeployAndroid.cs:2833). debuggable은 aapt2 --debug-mode가 넣는다. VIBRATE는 게임패드 진동이다(UE UEDeployAndroid.cs:3080의 기본
    // manifest처럼 늘 넣는다 — Docs/Design/InputActions 6.4절).
    internal static string Manifest(AndroidPackage package, string module, int minimumApi, int targetApi) => $"""
        <?xml version="1.0" encoding="utf-8"?>
        <manifest xmlns:android="http://schemas.android.com/apk/res/android"
            package="{SecurityElement.Escape(package.PackageName)}" android:versionCode="1" android:versionName="1.0">
            <uses-sdk android:minSdkVersion="{minimumApi}" android:targetSdkVersion="{targetApi}" />
            <uses-feature android:glEsVersion="0x00030001" android:required="true" />
            <uses-permission android:name="android.permission.VIBRATE" />
            <application android:label="{SecurityElement.Escape(module)}" android:hasCode="false" android:extractNativeLibs="false">
                <activity android:name="android.app.NativeActivity" android:exported="true" android:launchMode="singleTask"
                    android:theme="@android:style/Theme.NoTitleBar.Fullscreen"
                    android:configChanges="orientation|screenSize|screenLayout|smallestScreenSize|keyboard|keyboardHidden|navigation|uiMode|density">
                    <meta-data android:name="android.app.lib_name" android:value="{SecurityElement.Escape(module)}" />
                    <intent-filter>
                        <action android:name="android.intent.action.MAIN" />
                        <category android:name="android.intent.category.LAUNCHER" />
                    </intent-filter>
                </activity>
            </application>
        </manifest>

        """;

    // ~/.android/debug.keystore다. 없으면 JDK keytool로 만든다.
    private static async Task<string> DebugKeystoreAsync(string java, CancellationToken cancellationToken)
    {
        string directory = Directory.CreateDirectory(Path.Combine(Environment.GetFolderPath(Environment.SpecialFolder.UserProfile), ".android")).FullName;
        string keystore = Path.Combine(directory, "debug.keystore");
        if (File.Exists(keystore))
        {
            return keystore;
        }

        string keytool = Path.Combine(Path.GetDirectoryName(java)!, OperatingSystem.IsWindows() ? "keytool.exe" : "keytool");
        await RunAsync(keytool, ["-genkeypair", "-keystore", keystore, "-storepass", DebugKeystorePassword, "-alias", DebugKeyAlias,
            "-keypass", DebugKeystorePassword, "-keyalg", "RSA", "-keysize", DebugKeySizeBits, "-validity", DebugKeyValidityDays,
            "-dname", DebugKeyName], directory, null, cancellationToken);
        return keystore;
    }

    private static async Task RunAsync(string tool, IReadOnlyList<string> arguments, string directory,
        IReadOnlyDictionary<string, string?>? environment, CancellationToken cancellationToken)
    {
        ProcessResult result = await ProcessRunner.RunAsync(new ProcessSpec(tool, arguments, directory, environment, Timeout: ToolTimeout),
            cancellationToken);
        if (result.ExitCode != 0)
        {
            throw new InvalidOperationException($"{Path.GetFileName(tool)} failed with exit code {result.ExitCode}:\n{result.StdErr}\n{result.StdOut}");
        }
    }

    private static string Tool(string name) => OperatingSystem.IsWindows() ? name + ".exe" : name;

    internal static string? FindSdk()
    {
        foreach (string? sdk in new[]
                 {
                     Environment.GetEnvironmentVariable("ANDROID_HOME"),
                     Environment.GetEnvironmentVariable("ANDROID_SDK_ROOT"),
                     OperatingSystem.IsWindows()
                         ? Path.Combine(Environment.GetFolderPath(Environment.SpecialFolder.LocalApplicationData), "Android", "Sdk")
                         : Path.Combine(Environment.GetFolderPath(Environment.SpecialFolder.UserProfile), "Library", "Android", "sdk")
                 })
        {
            if (sdk is not null && Directory.Exists(Path.Combine(sdk, "build-tools")))
            {
                return sdk;
            }
        }

        return null;
    }

    // 가장 높은 버전 폴더(숫자 버전 이름)다.
    private static string? LatestVersionDirectory(string parent, Func<string, bool> usable) => !Directory.Exists(parent)
        ? null
        : Directory.GetDirectories(parent)
            .Select(directory => (directory, version: Version.TryParse(Path.GetFileName(directory), out Version? value) ? value : null))
            .Where(entry => entry.version is not null && usable(entry.directory))
            .OrderByDescending(entry => entry.version)
            .Select(entry => entry.directory)
            .FirstOrDefault();

    // 가장 높은 android-<N> platform(android.jar가 있는 것)이다. 미리보기 이름(android-Baklava)·확장(android-35-ext14)은 고르지 않는다.
    private static (string Directory, int Api)? LatestPlatform(string parent)
    {
        if (!Directory.Exists(parent))
        {
            return null;
        }

        (string Directory, int Api)? best = null;
        foreach (string directory in Directory.GetDirectories(parent))
        {
            string name = Path.GetFileName(directory);
            if (name.StartsWith("android-", StringComparison.Ordinal) && int.TryParse(name["android-".Length..], out int api) &&
                File.Exists(Path.Combine(directory, "android.jar")) && (best is null || api > best.Value.Api))
            {
                best = (directory, api);
            }
        }

        return best;
    }

    private static string? FindJava()
    {
        string executable = OperatingSystem.IsWindows() ? "java.exe" : "java";
        if (Environment.GetEnvironmentVariable("JAVA_HOME") is string home && File.Exists(Path.Combine(home, "bin", executable)))
        {
            return Path.Combine(home, "bin", executable);
        }

        foreach (string directory in (Environment.GetEnvironmentVariable("PATH") ?? "").Split(Path.PathSeparator, StringSplitOptions.RemoveEmptyEntries))
        {
            string candidate = Path.Combine(directory.Trim('"'), executable);
            if (File.Exists(candidate))
            {
                return candidate;
            }
        }

        return null;
    }
}
