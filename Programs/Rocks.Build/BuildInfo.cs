using System.Globalization;
using System.Security.Cryptography;
using System.Text;
using Rocks.Build.Rules;
using Rocks.Tooling.Common;

namespace Rocks.Build;

/// <summary>
/// 한 빌드 안의 파일 내용 digest·디렉터리 열거 메모다(Docs/Design/ReflectionFoundations.ko.md 8.1절 내용 해시). 파일을 한 번
/// 읽어 원본 byte의 SHA-256(InputSnapshot·액션 key의 FileHash와 같은 값)과 줄 끝 정규화 SHA-256을 함께 계산한다. 확장자가
/// header(<see cref="Fingerprints.HeaderExtensions"/>)이거나 <c>.cpp .cc .cxx .c++ .m .mm</c>인 텍스트 파일만 <c>\r\n</c>을 <c>\n</c>으로
/// 바꾸고(단독 CR·BOM은 그대로), 그 밖(prebuilt <c>.lib</c>·<c>.a</c> 등)은 원본 byte다. 판정은 확장자로만 한다. mtime·크기 캐시는
/// 두지 않는다(증분이 내용 해시만 믿는 계약과 같다).
/// </summary>
internal sealed class FileDigests
{
    // 스트리밍 읽기 버퍼다. 버퍼 끝의 CR은 다음 버퍼로 이월한다(시험이 경계 사례를 만든다).
    internal const int BufferSize = 64 * 1024;

    private static readonly HashSet<string> TextExtensions = new(
        Fingerprints.HeaderExtensions.Concat([".cpp", ".cc", ".cxx", ".c++", ".m", ".mm"]), StringComparer.OrdinalIgnoreCase);

    private readonly Dictionary<string, (string Raw, string Normalized)> files = new(NativeToolchains.PathComparer);

    private readonly Dictionary<string, string[]> trees = new(NativeToolchains.PathComparer);

    public static bool IsText(string path) => TextExtensions.Contains(Path.GetExtension(path));

    public string Raw(string path) => Get(path).Raw;

    // 지금까지 읽은 파일의 (정규 경로, 원본 SHA-256)이다. BuildInputs가 같은 파일을 다시 읽지 않도록 넘긴다.
    public IEnumerable<KeyValuePair<string, string>> RawEntries() => files.Select(pair => KeyValuePair.Create(pair.Key, pair.Value.Raw));

    public string Normalized(string path) => Get(path).Normalized;

    public (string Raw, string Normalized) Get(string path)
    {
        string full = Path.GetFullPath(path);
        if (!files.TryGetValue(full, out var digest))
        {
            digest = Compute(full);
            files.Add(full, digest);
        }

        return digest;
    }

    // Fingerprints.TreeFiles와 같은 열거를 메모한다. 같은 인자의 두 번째 요청은 디스크를 다시 열지 않는다.
    public IReadOnlyList<string> Tree(string root, bool headersOnly, string? exclude, string? buildRoot)
    {
        string key = string.Join('\0', Path.GetFullPath(root), headersOnly ? "1" : "0", exclude ?? "", buildRoot ?? "");
        if (!trees.TryGetValue(key, out string[]? list))
        {
            list = Fingerprints.TreeFiles(root, headersOnly, exclude, buildRoot).ToArray();
            trees.Add(key, list);
        }

        return list;
    }

    /// <summary>파일을 한 번 읽어 (원본 SHA-256, 줄 끝 정규화 SHA-256)을 소문자 hex로 돌려준다.</summary>
    internal static (string Raw, string Normalized) Compute(string path)
    {
        bool text = IsText(path);
        using var raw = IncrementalHash.CreateHash(HashAlgorithmName.SHA256);
        using var normalized = IncrementalHash.CreateHash(HashAlgorithmName.SHA256);
        byte[] buffer = new byte[BufferSize];
        byte[] carriageReturn = [(byte)'\r'];
        bool pendingCarriageReturn = false;
        using (var stream = new FileStream(path, FileMode.Open, FileAccess.Read, FileShare.Read, 1, FileOptions.SequentialScan))
        {
            int count;
            while ((count = stream.Read(buffer, 0, buffer.Length)) > 0)
            {
                raw.AppendData(buffer, 0, count);
                if (!text)
                {
                    continue;
                }

                int start = 0;
                if (pendingCarriageReturn)
                {
                    // 앞 버퍼 끝의 CR: 이 버퍼가 LF로 시작하면 CRLF이므로 CR을 버린다.
                    if (buffer[0] != (byte)'\n')
                    {
                        normalized.AppendData(carriageReturn);
                    }

                    pendingCarriageReturn = false;
                }

                for (int index = 0; index < count; ++index)
                {
                    if (buffer[index] != (byte)'\r')
                    {
                        continue;
                    }

                    if (index + 1 == count)
                    {
                        normalized.AppendData(buffer, start, index - start);
                        start = count;
                        pendingCarriageReturn = true;
                    }
                    else if (buffer[index + 1] == (byte)'\n')
                    {
                        normalized.AppendData(buffer, start, index - start);
                        start = index + 1;
                    }
                }

                normalized.AppendData(buffer, start, count - start);
            }
        }

        if (pendingCarriageReturn)
        {
            normalized.AppendData(carriageReturn);
        }

        string rawHex = Convert.ToHexStringLower(raw.GetHashAndReset());
        string normalizedHex = Convert.ToHexStringLower(normalized.GetHashAndReset());
        return (rawHex, text ? normalizedHex : rawHex);
    }
}

/// <summary>
/// 도구 빌드 ID의 호스트 무관 경로 키다(8.1절 경로 키). 판정은 <see cref="Path.GetRelativePath"/>로 한다(결과가 rooted이거나
/// <c>..</c>로 시작하면 그 루트 밖) — subst 드라이브 루트, 8.3 짧은 경로, junction에서도 열거된 철자 그대로 맞는다. 가장 긴 루트부터
/// <c>out:</c> → <c>sdk:</c> → <c>src:</c>(구분자 <c>/</c>, 루트 자체는 접두만), OwnedRoots 아래는 <c>toolchain:&lt;이름&gt;/</c>다. 루트 밖
/// include 디렉터리는 <c>ext&lt;i&gt;:</c>, 그 밖의 파일은 <c>ext:&lt;파일 이름&gt;</c>이며 둘 다 <see cref="HostDependent"/>를 켠다. 철자는
/// 대소문자를 접지 않고 NFC로 바꾼 뒤 제어 문자와 <c>%</c>를 percent-encoding한다(<see cref="Encode"/>). OwnedRoots는 src·out·sdk 안에
/// 있어도 되지만 그 루트들을 포함하면 ArgumentException이다.
/// </summary>
internal sealed class PortablePaths
{
    private readonly List<(string Root, string Prefix, int Order)> roots = [];

    private readonly List<string> externalIncludes = [];

    public PortablePaths(string source, string output, string? sdk, IReadOnlyDictionary<string, string> owned)
    {
        roots.Add((Path.GetFullPath(output), "out:", 0));
        if (sdk is not null)
        {
            roots.Add((Path.GetFullPath(sdk), "sdk:", 1));
        }

        roots.Add((Path.GetFullPath(source), "src:", 2));
        foreach ((string name, string path) in owned.OrderBy(pair => pair.Key, StringComparer.Ordinal))
        {
            if (name.Length == 0 || name.Any(value => !char.IsAsciiLetterOrDigit(value) && value is not ('-' or '_')) ||
                !Path.IsPathFullyQualified(path))
            {
                throw new ArgumentException($"Toolchain.OwnedRoots: invalid entry '{name}' = '{path}' (ASCII name, absolute path).");
            }

            // 소유 루트가 src·out·sdk 안에 있는 것은 허용한다(<root>/out/toolchains/... 설치 관례 — 가장 긴 루트가 toolchain: 키를
            // 고른다). 소유 루트가 src·out·sdk를 포함하거나 같으면 그 아래 모든 입력이 빠지므로 거부한다.
            string full = Path.GetFullPath(path);
            foreach ((string root, string prefix, _) in roots.Where(item => item.Order <= 2).ToArray())
            {
                if (Contains(full, root))
                {
                    throw new ArgumentException($"Toolchain.OwnedRoots: '{name}' ({full}) contains the {prefix[..^1]} root {root}.");
                }
            }

            roots.Add((full, "toolchain:" + name + "/", 3));
        }
    }

    // 루트 밖 경로를 썼는지다(ext 키). 그 ID는 호스트에 따라 달라질 수 있다.
    public bool HostDependent { get; private set; }

    /// <summary>루트 밖 include 디렉터리를 순번(ext&lt;i&gt;) 목록에 더한다. 루트 안이면 아무것도 하지 않는다.</summary>
    public void AddIncludeDirectory(string directory)
    {
        string full = Path.GetFullPath(directory);
        if (Best(full) is null && !externalIncludes.Contains(full, NativeToolchains.PathComparer))
        {
            externalIncludes.Add(full);
        }
    }

    public bool IsOwned(string path) => Best(Path.GetFullPath(path)) is { Order: 3 };

    public string Key(string path)
    {
        string full = Path.GetFullPath(path);
        if (Best(full) is (string root, string prefix, _))
        {
            return prefix + Relative(root, full);
        }

        string? include = null;
        int index = -1;
        for (int candidate = 0; candidate < externalIncludes.Count; ++candidate)
        {
            if (Contains(externalIncludes[candidate], full) && (include is null || externalIncludes[candidate].Length > include.Length))
            {
                include = externalIncludes[candidate];
                index = candidate;
            }
        }

        HostDependent = true;
        return include is null ? "ext:" + Encode(Path.GetFileName(full))
            : "ext" + index.ToString(CultureInfo.InvariantCulture) + ":" + Relative(include, full);
    }

    /// <summary>
    /// NFC 정규화 뒤 C0·C1 제어 문자(U+0000–001F, U+007F–009F), 줄·문단 구분자(U+2028·U+2029), <c>%</c>를 UTF-8 byte 단위
    /// <c>%XX</c>(대문자 hex)로, 짝 없는 surrogate를 <c>%uXXXX</c>로 바꾼다. 경로 키를 만들 때 한 번만 부른다(이미 인코딩한 키를 다시
    /// 넣지 않는다).
    /// </summary>
    public static string Encode(string text)
    {
        string normalized;
        try
        {
            normalized = text.Normalize(NormalizationForm.FormC);
        }
        catch (ArgumentException)
        {
            normalized = text;
        }

        var result = new StringBuilder(normalized.Length);
        for (int index = 0; index < normalized.Length; ++index)
        {
            char value = normalized[index];
            if (char.IsHighSurrogate(value) && index + 1 < normalized.Length && char.IsLowSurrogate(normalized[index + 1]))
            {
                result.Append(value).Append(normalized[index + 1]);
                ++index;
            }
            else if (char.IsSurrogate(value))
            {
                result.Append("%u").Append(((int)value).ToString("X4", CultureInfo.InvariantCulture));
            }
            else if (value < 0x20 || value is >= '\u007f' and <= '\u009f' || value == (char)0x2028 || value == (char)0x2029 || value == '%')
            {
                foreach (byte octet in Encoding.UTF8.GetBytes(value.ToString()))
                {
                    result.Append('%').Append(octet.ToString("X2", CultureInfo.InvariantCulture));
                }
            }
            else
            {
                result.Append(value);
            }
        }

        return result.ToString();
    }

    // 가장 긴 루트(같으면 out → sdk → src → toolchain 순)다.
    private (string Root, string Prefix, int Order)? Best(string full)
    {
        (string Root, string Prefix, int Order)? best = null;
        foreach (var item in roots)
        {
            if (Contains(item.Root, full) && (best is null || item.Root.Length > best.Value.Root.Length ||
                (item.Root.Length == best.Value.Root.Length && item.Order < best.Value.Order)))
            {
                best = item;
            }
        }

        return best;
    }

    private static bool Contains(string root, string path)
    {
        string relative = Path.GetRelativePath(root, path);
        return !Path.IsPathRooted(relative) && relative != ".." &&
            !relative.StartsWith(".." + Path.DirectorySeparatorChar, StringComparison.Ordinal) &&
            !relative.StartsWith("../", StringComparison.Ordinal);
    }

    private static string Relative(string root, string full)
    {
        string relative = Path.GetRelativePath(root, full);
        return relative == "." ? "" : Encode(relative.Replace('\\', '/'));
    }
}

/// <summary>Prepare 결과다. Plan은 요청이 바뀌지 않았을 때(BuildInfo 대상 없음) 다시 세우지 않으려고 넘기는 1단계 계획이다.</summary>
internal sealed record BuildInfoPreparation(NativeBuildRequest Request, NativePlan? Plan, FileDigests Digests,
    string? ToolchainFingerprint, IReadOnlyDictionary<string, string> ToolBuildIds);

/// <summary>
/// 빌드 주입 상수(Docs/Design/ReflectionFoundations.ko.md 8.1절)를 만든다. 계획의 Executable 모듈 중 도달 집합에
/// <see cref="NativeModuleInput.ProvidesBuildInfo"/> 모듈이 있는 것마다 1단계 계획(BuildInfo를 더하기 전)의 Compile·Archive·Link
/// 액션으로 manifest를 만들고, ID = SHA-256(UTF-8(manifest))와 툴체인 지문·엔진 버전(<see cref="NativeBuildRequest.EngineVersion"/>,
/// 8.2절)을 담은 정의 TU를 <c>&lt;out&gt;/BuildInfo/&lt;Module&gt;.BuildInfo.cpp</c>에 써서 그 모듈 소스에 더한 요청을 돌려준다. 엔진
/// 버전은 파일에서 읽지 않는다(호출자가 넘긴 값만 쓴다). 출력 잠금 아래에서만 부른다.
/// </summary>
internal static class BuildInfoGenerator
{
    public const string DirectoryName = "BuildInfo";

    public static string DirectoryOf(string output) => Path.Combine(output, DirectoryName);

    public static BuildInfoPreparation Prepare(NativeBuildRequest request)
    {
        string output = request.OutputDirectory;
        string directory = DirectoryOf(output);
        // 앞 실행이 AtomicFile 쓰기 중 끝났으면 임시 파일이 남는다. 지우지 못하면(다른 프로그램이 잡고 있음 등) 경고만 하고 빌드는
        // 계속한다 — 임시 파일은 입력이 아니다. 이번 실행에 없는 실행 파일의 BuildInfo는 지우지 않는다.
        if (Directory.Exists(directory))
        {
            foreach (string temporary in Directory.EnumerateFiles(directory, "*.tmp"))
            {
                try
                {
                    File.Delete(temporary);
                }
                catch (Exception error) when (error is IOException or UnauthorizedAccessException)
                {
                    Console.Error.WriteLine($"BuildInfo: warning: cannot delete stale temporary file {temporary}: {error.Message}");
                }
            }
        }

        var digests = new FileDigests();
        NativePlan plan = ActionGraph.Create(request);
        // 정의 헤더는 manifest의 입력(내용 해시)이다. 읽기 전에 쓴다 — 엔진도 최종 계획의 생성 파일을 다시 쓰고, 같으면 건드리지 않는다.
        foreach (PlannedFile file in plan.GeneratedFiles)
        {
            AtomicFile.WriteIfChanged(file.Path, new UTF8Encoding(false).GetBytes(file.Content));
        }

        var modules = request.Modules.ToDictionary(module => module.Name, StringComparer.OrdinalIgnoreCase);
        // 소스가 없는 실행 파일은 대상이 아니다(생성 TU만으로 main 없는 link를 시도하지 않는다 — 지금은 ActionGraph가 먼저 거부한다).
        string[] targets = plan.Closures
            .Where(pair => modules[pair.Key].Kind == NativeModuleKind.Executable && modules[pair.Key].Sources.Count != 0 &&
                pair.Value.Any(name => modules[name].ProvidesBuildInfo))
            .Select(pair => modules[pair.Key].Name).Order(StringComparer.Ordinal).ToArray();
        if (targets.Length == 0)
        {
            return new(request, plan, digests, null, new Dictionary<string, string>(StringComparer.Ordinal));
        }

        // 엔진 버전과 툴체인 신원은 출력을 쓰기 전에 확인한다(빈 값으로 생성 TU를 만들지 않는다).
        EngineVersion engineVersion = RequireEngineVersion(request);
        NativeToolchain toolchain = request.Toolchain;
        foreach (string field in toolchain.AbiFields)
        {
            if (field.IndexOfAny([';', '\r', '\n']) >= 0)
            {
                throw new ArgumentException($"Toolchain.AbiFields: '{field}' must not contain ';' or line breaks.");
            }
        }

        string canonical = ToolchainFingerprint.CanonicalText(toolchain);
        byte[] fingerprint = SHA256.HashData(Encoding.UTF8.GetBytes(canonical));
        string fingerprintHex = Convert.ToHexStringLower(fingerprint);
        // 모든 manifest를 먼저 만든다. 입력을 읽지 못하면 아무 출력도 바꾸지 않는다.
        var manifests = new SortedDictionary<string, byte[]>(StringComparer.Ordinal);
        foreach (string target in targets)
        {
            manifests.Add(target, Encoding.UTF8.GetBytes(Manifest(request, plan, target, fingerprintHex, digests)));
        }

        // 쓰기 순서는 실행 파일마다 cpp → txt, 마지막에 ToolchainFingerprint.txt다. 중간에 실패해도(예외는 빌드를 멈춘다) 각 파일은
        // AtomicFile로 통째 교체되고, 다음 Prepare가 같은 입력에서 같은 내용을 다시 계산해 WriteIfChanged로 맞춘다. 생성 TU의 액션
        // key는 cpp 내용이므로 낡은 ID가 다음 빌드에 남지 않는다.
        var ids = new Dictionary<string, string>(StringComparer.Ordinal);
        var sources = new Dictionary<string, string>(StringComparer.OrdinalIgnoreCase);
        foreach ((string target, byte[] manifest) in manifests)
        {
            byte[] id = SHA256.HashData(manifest);
            string source = Path.Combine(directory, target + ".BuildInfo.cpp");
            AtomicFile.WriteIfChanged(source, Encoding.ASCII.GetBytes(Source(target, fingerprint, id, engineVersion, request.Target)));
            AtomicFile.WriteIfChanged(Path.Combine(directory, target + ".BuildInfo.txt"), manifest);
            ids.Add(target, Convert.ToHexStringLower(id));
            sources.Add(target, source);
        }

        AtomicFile.WriteIfChanged(Path.Combine(directory, "ToolchainFingerprint.txt"), Encoding.UTF8.GetBytes(canonical));
        // Monolithic은 정의 TU를 실행 파일 소스에 넣는다. Modular는 그 타깃이 닿는 빌드 정보 제공 모듈(Core)의 공유 라이브러리 소스에 넣는다
        // (Docs/Design/BuildTargets 3.8절 BS8d1 — 실행 파일이 정의하면 Core 공유 라이브러리에 미해결 기호가 남는다). Modular 요청은
        // 타깃이 하나라 제공 모듈의 정의 TU도 하나다.
        if (request.LinkType == NativeLinkType.Modular)
        {
            var hosted = new Dictionary<string, string>(StringComparer.OrdinalIgnoreCase);
            foreach ((string target, string source) in sources)
            {
                string provider = plan.Closures[target].Single(name => modules[name].ProvidesBuildInfo);
                // 입력 검사가 그런 실행 파일을 하나 이하로 막는다(InputValidation.ValidateLinkType).
                if (!hosted.TryAdd(provider, source))
                {
                    throw new InvalidOperationException($"Internal error: more than one Modular executable reaches the build information provider {provider}.");
                }
            }

            sources = hosted;
        }

        var injected = request with
        {
            Modules = request.Modules.Select(module => sources.TryGetValue(module.Name, out string? source)
                ? module with { Sources = [.. module.Sources, source] } : module).ToArray()
        };
        return new(injected, null, digests, fingerprintHex, ids);
    }

    /// <summary>
    /// BuildInfo 대상이 있을 때의 엔진 버전이다. 요청에 없거나 빈 값(0.0.x)이면 ArgumentException이다(8.1절 빈 툴체인 신원 규칙과
    /// 같다 — 빈 값을 조용히 주입하지 않는다).
    /// </summary>
    internal static EngineVersion RequireEngineVersion(NativeBuildRequest request) =>
        request.EngineVersion is { IsEmpty: false } version
            ? version
            : throw new ArgumentException("NativeBuildRequest.EngineVersion is required for BuildInfo executables " +
                $"(Rocks.Build.Cli reads {EngineVersion.RelativePath}; 0.0.x is the empty value).");

    /// <summary>
    /// 실행 파일 하나의 manifest다(8.1절 형식). 액션은 (종류, Compile이면 소스 키·나머지는 모듈 이름)의 ordinal 순, 인자는 원래
    /// 순서, 입력은 키 ordinal 순(같은 키는 원래 경로 UTF-8 byte 순)·경로 중복 제거다.
    /// </summary>
    internal static string Manifest(NativeBuildRequest request, NativePlan plan, string executable, string fingerprintHex,
        FileDigests digests)
    {
        EngineVersion engineVersion = RequireEngineVersion(request);
        NativeToolchain toolchain = request.Toolchain;
        string root = request.RootDirectory;
        string output = request.OutputDirectory;
        string directory = DirectoryOf(output);
        var paths = new PortablePaths(root, output, request.SdkDirectory, toolchain.OwnedRoots);
        var modules = request.Modules.ToDictionary(module => module.Name, StringComparer.OrdinalIgnoreCase);
        IReadOnlyList<string> closure = plan.Closures[executable];
        var reached = closure.ToHashSet(StringComparer.OrdinalIgnoreCase);
        var produced = plan.Actions.SelectMany(action => action.Outputs).ToHashSet(NativeToolchains.PathComparer);
        // object마다 그 번역 단위를 이루는 원본 소스다(단독 TU면 하나, 묶음이면 구성원 전부).
        // ISPC object도 원본 소스 키로 적는다(Docs/Design/BuildTargets 3.12절 BD50 — object 경로는 절대 경로 해시다).
        var objectSources = plan.Actions.Where(action => action.Kind is "Compile" or "Ispc")
            .ToDictionary(action => action.Outputs[0], action => action.SourceMembers, NativeToolchains.PathComparer);
        NativeAction[] actions = plan.Actions.Where(action => reached.Contains(action.Module) &&
            action.Kind is "Compile" or "Archive" or "Link" or "Ispc").ToArray();
        // 외부 include 루트 순번은 도달 집합 순서(실행 파일, 그다음 의존 순서)와 각 액션의 header 디렉터리 순서로 정한다.
        foreach (string name in closure)
        {
            foreach (NativeAction action in actions.Where(action => action.Kind == "Compile" &&
                string.Equals(action.Module, name, StringComparison.OrdinalIgnoreCase)))
            {
                foreach (string header in action.HeaderDirectories)
                {
                    paths.AddIncludeDirectory(header);
                }
            }
        }

        string[] rootStrings = new[] { root, output, request.SdkDirectory }.Where(value => value is not null)
            .Select(value => Path.TrimEndingDirectorySeparator(Path.GetFullPath(value!))).ToArray();
        var blocks = new List<(string Kind, string Order, string Module, string Text)>();
        void AddBlock(string kind, string id, string order, NativeModuleInput module, List<string> arguments)
        {
            var text = new StringBuilder();
            text.Append("action: ").Append(kind).Append(' ').Append(order).Append('\n');
            // archive·link의 object 인자는 원본 소스 키 목록(obj:<소스 키>, ordinal 순)으로 대신 적는다(8.7절). 묶음 object는 구성이
            // 바뀌면 이름과 개수가 바뀌므로, 물리 object가 아니라 그 object들이 담는 원본 소스의 집합을 적어야 유니티 모드와 묶음 구성이
            // ID를 바꾸지 않는다. 목록은 첫 object 인자의 자리에 둔다.
            var objectKeys = new List<string>();
            int objectPosition = -1;
            var lines = new List<string>();
            foreach (string argument in arguments)
            {
                if (objectSources.TryGetValue(argument, out IReadOnlyList<string>? members))
                {
                    objectPosition = objectPosition < 0 ? lines.Count : objectPosition;
                    objectKeys.AddRange(members.Select(member => "obj:" + paths.Key(member)));
                    continue;
                }

                string normalized = NormalizeArgument(argument, paths);
                foreach (string rootString in rootStrings)
                {
                    if (ContainsHostPath(normalized, rootString, OperatingSystem.IsWindows()))
                    {
                        throw new InvalidOperationException($"BuildInfo: {id} argument '{argument}' still contains the host path {rootString} " +
                            "after normalization; add the argument form to the BuildInfo path normalization.");
                    }
                }

                lines.Add(normalized);
            }

            if (objectPosition >= 0)
            {
                objectKeys.Sort(StringComparer.Ordinal);
                lines.InsertRange(objectPosition, objectKeys);
            }

            foreach (string line in lines)
            {
                text.Append("arg: ").Append(line).Append('\n');
            }

            blocks.Add((kind, order, module.Name, text.ToString()));
        }

        // compile 부분은 물리 action이 아니라 원본 소스마다의 논리 명령으로 만든다(Docs/Design/UnityBuildPch.ko.md 8.7절 — 유니티 모드,
        // 묶음 구성, depfile 같은 실행 전용 인자가 ID를 바꾸지 않는다). archive·link 부분은 물리 action이다.
        foreach (LogicalCompile compile in plan.LogicalCompiles.Where(compile => reached.Contains(compile.Module)))
        {
            NativeModuleInput module = modules[compile.Module];
            List<string> arguments = compile.Arguments.ToList();
            RemoveSubsequence(arguments, module.AdditionalCompileArguments.Count, toolchain.CompileArguments, compile.Action);
            AddBlock("Compile", compile.Action, paths.Key(compile.Source), module, arguments);
        }

        foreach (NativeAction action in actions.Where(action => action.Kind != "Compile"))
        {
            List<string> arguments = action.Arguments.ToList();
            NativeModuleInput module = modules[action.Module];
            if (action.Kind == "Link")
            {
                RemoveSubsequence(arguments, 0, toolchain.LinkArguments, action.Id);
            }

            // Ispc action(BD50)은 원본 키로 줄을 세우고, 도구는 경로가 아니라 고정 버전으로 적는다(설치 경로는 호스트마다 다르다).
            if (action.Kind == "Ispc")
            {
                AddBlock(action.Kind, action.Id, paths.Key(action.Source!), module, ["ispc-version=" + action.Identity[1], .. arguments]);
                continue;
            }

            AddBlock(action.Kind, action.Id, module.Name, module, arguments);
        }

        // 입력: 1단계 액션의 Inputs·DeclaredInputs·header 디렉터리 inventory. 액션 출력·<out>/BuildInfo·OwnedRoots 아래는 뺀다.
        var inputs = new Dictionary<string, string>(NativeToolchains.PathComparer);
        void Add(string file)
        {
            string full = Path.GetFullPath(file);
            if (produced.Contains(full) || Fingerprints.IsUnder(full, directory) || paths.IsOwned(full) || inputs.ContainsKey(full))
            {
                return;
            }

            inputs.Add(full, paths.Key(full));
        }

        IReadOnlyList<string> Enumerate(string path, bool headersOnly, string? exclude, string? buildRoot)
        {
            try
            {
                return digests.Tree(path, headersOnly, exclude, buildRoot);
            }
            catch (Exception error) when (error is IOException or UnauthorizedAccessException)
            {
                throw new InvalidOperationException($"BuildInfo: cannot read {paths.Key(path)}: {error.Message}", error);
            }
        }

        // compile action의 입력은 원본 소스다(묶음 파일은 구성에 따라 바뀌는 생성물이라 넣지 않는다 — 위 논리 명령과 같은 이유다).
        // 강제 include하는 생성 헤더(정의 헤더)와 PCH 헤더도 입력이다(8.7절 — 내용 해시).
        foreach (LogicalCompile compile in plan.LogicalCompiles.Where(compile => reached.Contains(compile.Module)))
        {
            Add(compile.Source);
            foreach (string forced in compile.ForcedIncludes)
            {
                Add(forced);
            }
        }

        foreach (NativeAction action in actions)
        {
            foreach (string input in action.Kind == "Compile" ? [] : action.Inputs)
            {
                Add(input);
            }

            // 다른 action의 산출물인 선언 입력(ISPC 생성 헤더 — Docs/Design/BuildTargets 3.12절 BD48)은 빌드 전이라 없을 수 있고, 그 내용은
            // 산출한 action의 입력이 대신한다.
            foreach (string declared in action.DeclaredInputs.Where(value => !produced.Contains(Path.GetFullPath(value))))
            {
                if (File.Exists(declared))
                {
                    Add(declared);
                }
                else if (Directory.Exists(declared))
                {
                    foreach (string file in Enumerate(declared, false, null, null))
                    {
                        Add(file);
                    }
                }
                else
                {
                    throw new InvalidOperationException($"BuildInfo: cannot read {paths.Key(declared)}: the declared input does not exist.");
                }
            }

            foreach (string header in action.HeaderDirectories)
            {
                foreach (string file in Enumerate(header, true, output, root))
                {
                    Add(file);
                }
            }
        }

        var lines = new List<(string Key, byte[] Original, string Text)>();
        foreach ((string full, string key) in inputs)
        {
            string digest;
            try
            {
                digest = digests.Normalized(full);
            }
            catch (Exception error) when (error is IOException or UnauthorizedAccessException)
            {
                throw new InvalidOperationException($"BuildInfo: cannot read {key}: {error.Message}", error);
            }

            lines.Add((key, Encoding.UTF8.GetBytes(full), "input: " + key + " " + digest + "\n"));
        }

        lines.Sort((left, right) =>
        {
            int compared = string.CompareOrdinal(left.Key, right.Key);
            return compared != 0 ? compared : left.Original.AsSpan().SequenceCompareTo(right.Original);
        });
        var manifest = new StringBuilder();
        // 형식 2: executable 다음에 engine-version 줄(정규 10진 "주.부.패치")을 더했다.
        manifest.Append("rocks-tool-build-id 2\n");
        manifest.Append("executable: ").Append(modules[executable].Name).Append('\n');
        manifest.Append("engine-version: ").Append(engineVersion.ToString()).Append('\n');
        manifest.Append("toolchain-fingerprint: ").Append(fingerprintHex).Append('\n');
        manifest.Append("toolchain-target: ").Append(toolchain.TargetTriple).Append('\n');
        manifest.Append("toolchain-abi: ").Append(string.Join(';', toolchain.AbiFields.Order(StringComparer.Ordinal))).Append('\n');
        manifest.Append("host-dependent: ").Append(paths.HostDependent ? "yes" : "no").Append('\n');
        foreach (var block in blocks.OrderBy(block => block.Kind, StringComparer.Ordinal).ThenBy(block => block.Order, StringComparer.Ordinal)
            .ThenBy(block => block.Module, StringComparer.Ordinal))
        {
            manifest.Append(block.Text);
        }

        foreach (var line in lines)
        {
            manifest.Append(line.Text);
        }

        return manifest.ToString();
    }

    /// <summary>
    /// 생성 TU다(ASCII, LF, 결정적). 자유 텍스트(컴파일러 줄·경로)는 넣지 않는다 — 원문은 .txt에 있다. 형식 2는
    /// <c>CurrentEngineVersion()</c>(집합체 <c>return {주, 부, 패치};</c>)을 더했다. 형식 3은 <c>CurrentTargetType()</c>과, 타깃 종류와 타깃
    /// 매크로가 맞는지 보는 static_assert를 더했다(Docs/Design/BuildTargets 3.2절 BS2a).
    /// </summary>
    internal static string Source(string module, byte[] fingerprint, byte[] id, EngineVersion engineVersion, TargetKind target)
    {
        static string Bytes(byte[] value)
        {
            var text = new StringBuilder();
            for (int row = 0; row < 4; ++row)
            {
                text.Append("        ");
                for (int column = 0; column < 8; ++column)
                {
                    int index = row * 8 + column;
                    text.Append("0x").Append(value[index].ToString("x2", CultureInfo.InvariantCulture));
                    if (index != 31)
                    {
                        text.Append(column == 7 ? ",\n" : ", ");
                    }
                }
            }

            return text.ToString();
        }

        string version = string.Create(CultureInfo.InvariantCulture,
            $"{engineVersion.Major}, {engineVersion.Minor}, {engineVersion.Patch}");
        string type = target.Type.ToString();
        // 타깃 종류와 Program 속성이 정하는 매크로 값 넷이다(TargetKind.Defines의 "이름=값"을 "이름 == 값"으로).
        string check = string.Join(" && ", target.Defines.Select(define => define.Replace("=", " == ", StringComparison.Ordinal)));
        return "// Generated by Rocks.Build BuildInfo 3 for module " + module + ". Do not edit.\n" +
            "#include <Core/Build/BuildInfo.h>\n" +
            "\n" +
            "static_assert(rocks::Sha256Digest::Bytes == 32);\n" +
            "\n" +
            "namespace rocks::build {\n" +
            "\n" +
            "Sha256Digest ToolchainFingerprint() noexcept {\n" +
            "    return Sha256Digest{{\n" + Bytes(fingerprint) + "}};\n" +
            "}\n" +
            "\n" +
            "Sha256Digest ToolBuildId() noexcept {\n" +
            "    return Sha256Digest{{\n" + Bytes(id) + "}};\n" +
            "}\n" +
            "\n" +
            "EngineVersion CurrentEngineVersion() noexcept {\n" +
            "    return {" + version + "};\n" +
            "}\n" +
            "\n" +
            "static_assert(" + check + ", \"The target macros do not match the " + type + " target type.\");\n" +
            "\n" +
            "TargetType CurrentTargetType() noexcept {\n" +
            "    return TargetType::" + type + ";\n" +
            "}\n" +
            "\n" +
            "} // namespace rocks::build\n";
    }

    // 인자 정규화 (2)·(3): object는 호출자가 obj:<소스 키> 목록으로 바꾼다(Manifest의 AddBlock). 전체가 절대 경로이거나 /I·-I·/WHOLEARCHIVE: 뒤가 절대 경로면 경로 키, 접두 매핑
    // (-fmacro-prefix-map=<루트>=)은 루트를 경로 키로. 경로 키는
    // PortablePaths.Key가 이미 인코딩했으므로 다시 인코딩하지 않고, 경로가 아닌 인자만 여기서 한 번 인코딩한다.
    private static string NormalizeArgument(string argument, PortablePaths paths)
    {
        if (Path.IsPathFullyQualified(argument))
        {
            return paths.Key(argument);
        }

        foreach (string prefix in new[] { "/I", "-I", "/FI", "/WHOLEARCHIVE:" })
        {
            if (argument.Length > prefix.Length && argument.StartsWith(prefix, StringComparison.Ordinal) &&
                Path.IsPathFullyQualified(argument[prefix.Length..]))
            {
                return prefix + paths.Key(argument[prefix.Length..]);
            }
        }

        // ActionGraph.TargetCompileArguments의 소스 경로 접두 매핑(<접두>-fmacro-prefix-map=<루트><구분자>=)은 루트를 경로 키로 바꾼다.
        foreach (string prefix in new[] { "/clang:-fmacro-prefix-map=", "-fmacro-prefix-map=" })
        {
            if (argument.Length > prefix.Length + 1 && argument.StartsWith(prefix, StringComparison.Ordinal) && argument.EndsWith('=') &&
                Path.IsPathFullyQualified(argument[prefix.Length..^1]))
            {
                return prefix + paths.Key(Path.TrimEndingDirectorySeparator(argument[prefix.Length..^1])) + "=";
            }
        }

        return PortablePaths.Encode(argument);
    }

    /// <summary>
    /// 인자 정규화 (4)의 남은 호스트 경로 검사다. root가 text 안에 경로로 나타날 때만 참이다: 바로 뒤가 구분자이거나 끝(root가 구분자로
    /// 끝나는 드라이브 루트면 그것으로 충분)이고, 바로 앞이 시작, <c>= : , ; " '</c>·공백, 또는 <c>-I</c>·<c>/I</c>·<c>-L</c> 접두 바로 뒤일
    /// 때다 — POSIX의 짧은 루트(<c>/src</c>·<c>/c</c>)가 다른 낱말의 일부로 오탐되지 않게. Windows는 대소문자를 무시하고 두 구분자를
    /// 모두 본다.
    /// </summary>
    internal static bool ContainsHostPath(string text, string root, bool windows)
    {
        StringComparison comparison = windows ? StringComparison.OrdinalIgnoreCase : StringComparison.Ordinal;
        bool rootEndsWithSeparator = root.EndsWith('/') || (windows && root.EndsWith('\\'));
        string[] spellings = windows ? [root, root.Replace('\\', '/')] : [root];
        foreach (string spelling in spellings.Distinct(StringComparer.Ordinal))
        {
            for (int index = text.IndexOf(spelling, comparison); index >= 0;
                 index = index + 1 < text.Length ? text.IndexOf(spelling, index + 1, comparison) : -1)
            {
                int end = index + spelling.Length;
                bool after = rootEndsWithSeparator || end == text.Length || text[end] == '/' || (windows && text[end] == '\\');
                bool before = index == 0 || text[index - 1] is '=' or ':' or ',' or ';' or '"' or '\'' or ' ' ||
                    (index == 2 && (text.StartsWith("-I", StringComparison.Ordinal) || text.StartsWith("/I", StringComparison.Ordinal) ||
                                    text.StartsWith("-L", StringComparison.Ordinal)));
                if (after && before)
                {
                    return true;
                }
            }
        }

        return false;
    }

    // 인자 정규화 (1): 툴체인 인자 부분열(macOS -isysroot <Xcode 경로> 같은 호스트 경로)을 뺀다. SDK·배포 대상은 toolchain-abi가
    // 대신한다. 부분열이 그 자리에 없으면 ActionGraph의 인자 배치가 바뀐 것이므로 InvalidOperationException이다.
    private static void RemoveSubsequence(List<string> arguments, int position, IReadOnlyList<string> subsequence, string action)
    {
        if (position + subsequence.Count > arguments.Count ||
            !arguments.Skip(position).Take(subsequence.Count).SequenceEqual(subsequence, StringComparer.Ordinal))
        {
            throw new InvalidOperationException($"BuildInfo: {action} does not carry the toolchain arguments at position {position}.");
        }

        arguments.RemoveRange(position, subsequence.Count);
    }
}
