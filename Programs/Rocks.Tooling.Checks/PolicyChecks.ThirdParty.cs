using System.IO.Enumeration;
using System.Security.Cryptography;
using System.Text.RegularExpressions;
using Rocks.Tooling.Common;

namespace Rocks.Tooling.Checks;

public static partial class PolicyChecks
{
    // 운영체제가 만드는 메타데이터 파일은 원본으로 보지 않는다(.gitignore 대상).
    private static readonly HashSet<string> SystemMetadataFiles = new(StringComparer.OrdinalIgnoreCase)
        { ".DS_Store", "Thumbs.db", "desktop.ini" };

    private static readonly Regex IncludeDirective = Pattern(
        "^\\s*#\\s*include\\s*([<\"])([^>\"]+)[>\"]");

    /// <summary>
    /// 고정 버전 외부 라이브러리(<c>Core/Source/ThirdParty/&lt;Lib&gt;/</c>)를 검사한다(rule id <c>third-party</c>).
    /// <list type="bullet">
    /// <item>폴더마다 엄격한 <c>ThirdParty.json</c>과 <c>&lt;Lib&gt;.Build.cs</c>가 있어야 한다. 그 밖의 파일은 manifest의
    /// 번역 단위로 등록한 Rocks 글루(.c)뿐이다. 데이터 전용 라이브러리(<c>"kind": "data"</c>)는 모듈 규칙이 없어야 하고
    /// 지시문 검사를 하지 않는다(파일 집합·SHA-256·라이선스 검사는 같다).</item>
    /// <item>원본 디렉터리의 실제 파일 집합이 manifest <c>files</c>와 정확히 같고 SHA-256(byte 기준)이 일치한다.
    /// 링크는 거부하고 따라 들어가지 않는다. manifest <c>licenses</c>의 모든 원문 파일이 등록된 원본 파일이어야 한다.</item>
    /// <item>번역 단위·include 경로·adapter 헤더가 존재하고 허용 위치에 있다.</item>
    /// <item>원본의 컴파일러 지시문(<see cref="ThirdPartyManifest.ScanPragmaDirectives"/>)이 manifest 목록과 정확히 같다.
    /// 업그레이드로 새 지시문이 들어오면 실패한다.</item>
    /// <item>adapter 헤더(엔진 번역 단위가 include하는 외부 헤더)는 진단 억제를 같은 계열의 push/pop 안에서만 쓰고,
    /// 따옴표로 include하는 원본 파일도 adapter 헤더여야 한다. 엔진 소스는 adapter 헤더 외의 원본을 include하지 않는다
    /// (등록된 글루는 자기 라이브러리의 원본 .c를 include할 수 있다).</item>
    /// </list>
    /// </summary>
    public static IReadOnlyList<CheckFailure> ThirdParty(string root)
    {
        root = Path.GetFullPath(root);
        List<CheckFailure> failures = [];
        string thirdParty = Path.Combine(root, ThirdPartyManifest.ThirdPartyRoot);
        if (!Directory.Exists(thirdParty))
        {
            return failures;
        }

        List<(string Library, ThirdPartyManifest Manifest)> libraries = [];
        foreach (string entry in Directory.EnumerateFileSystemEntries(thirdParty).Order(StringComparer.Ordinal))
        {
            string relative = Relative(root, entry);
            if (SystemMetadataFiles.Contains(Path.GetFileName(entry)))
            {
                continue;
            }

            if (!Directory.Exists(entry) || IsLink(entry))
            {
                failures.Add(new(relative, 0, "third-party", "only library directories are allowed under ThirdParty"));
                continue;
            }

            if (CheckLibrary(root, entry, failures) is ThirdPartyManifest manifest)
            {
                libraries.Add((entry, manifest));
            }
        }

        CheckOwnedIncludes(root, libraries, failures);
        return failures;
    }

    /// <summary>
    /// 추적 가능한 수정(manifest <c>modifications</c>·<c>patch</c>)을 확인한다. patch가 다루는 파일 집합과 manifest 목록이
    /// 같아야 하고, 현재 파일에 patch를 거꾸로 적용한 결과의 SHA-256이 <c>upstreamSha256</c>과 같아야 한다. 추가된 파일
    /// (<c>upstreamSha256: null</c>)은 patch의 새 파일 내용과 현재 내용이 정확히 같아야 한다.
    /// </summary>
    private static void CheckModifications(ThirdPartyManifest manifest, string library, string relativeLibrary, string relativeUpstream,
        IReadOnlyDictionary<string, byte[]> actual, Action<string, int, string> fail)
    {
        if (manifest.Patch is null)
        {
            return;
        }

        string patchPath = Path.Combine(library, manifest.Patch);
        if (!File.Exists(patchPath) || IsLink(patchPath))
        {
            fail($"{relativeLibrary}/{manifest.Patch}", 0, "patch file is missing or is a link");
            return;
        }

        IReadOnlyList<ThirdPartyPatch.FilePatch> patches;
        try
        {
            patches = ThirdPartyPatch.Parse(new System.Text.UTF8Encoding(false, true).GetString(File.ReadAllBytes(patchPath)));
        }
        catch (Exception error) when (error is FormatException or System.Text.DecoderFallbackException)
        {
            fail($"{relativeLibrary}/{manifest.Patch}", 0, error.Message);
            return;
        }

        var byPath = patches.ToDictionary(item => item.Path, StringComparer.Ordinal);
        foreach (ThirdPartyModification modification in manifest.Modifications)
        {
            string path = $"{relativeUpstream}/{modification.File}";
            if (!byPath.TryGetValue(modification.File, out ThirdPartyPatch.FilePatch? filePatch))
            {
                fail(path, 0, "modified file has no entry in the patch");
                continue;
            }

            if (filePatch.Added != (modification.UpstreamSha256 is null))
            {
                fail(path, 0, "patch and manifest disagree on whether the file was added");
                continue;
            }

            if (!actual.TryGetValue(modification.File, out byte[]? current))
            {
                continue; // 파일 집합 검사가 이미 보고했다.
            }

            try
            {
                byte[]? original = ThirdPartyPatch.Reverse(filePatch, current);
                if (original is not null && Convert.ToHexStringLower(SHA256.HashData(original)) != modification.UpstreamSha256)
                {
                    fail(path, 0, "reverting the patch does not reproduce upstreamSha256");
                }
            }
            catch (Exception error) when (error is FormatException or System.Text.DecoderFallbackException)
            {
                fail(path, 0, "patch does not apply in reverse to the current file: " + error.Message);
            }
        }

        foreach (string file in byPath.Keys.Where(file => manifest.Modifications.All(item => item.File != file)).Order(StringComparer.Ordinal))
        {
            fail($"{relativeUpstream}/{file}", 0, "patch touches a file that is not listed in manifest modifications");
        }
    }

    private static ThirdPartyManifest? CheckLibrary(string root, string library, List<CheckFailure> failures)
    {
        string name = Path.GetFileName(library);
        string relativeLibrary = Relative(root, library);
        void Fail(string path, int line, string message) => failures.Add(new(path, line, "third-party", message));

        ThirdPartyManifest manifest;
        try
        {
            manifest = ThirdPartyManifest.Read(library);
        }
        catch (FormatException error)
        {
            Fail($"{relativeLibrary}/{ThirdPartyManifest.FileName}", 0, error.Message);
            return null;
        }

        string upstream = Path.Combine(library, manifest.UpstreamDirectory);
        string relativeUpstream = $"{relativeLibrary}/{manifest.UpstreamDirectory}";
        if (!Directory.Exists(upstream) || IsLink(upstream))
        {
            Fail(relativeUpstream, 0, "upstream directory is missing or is a link");
            return null;
        }

        // 라이브러리 폴더 바로 아래: manifest·규칙·등록한 글루·원본 디렉터리만 허용한다.
        var glue = manifest.TranslationUnits.Where(unit => !unit.StartsWith(manifest.UpstreamDirectory + "/", StringComparison.Ordinal))
            .ToHashSet(StringComparer.Ordinal);
        bool data = manifest.Kind == ThirdPartyKind.Data;
        bool hasRule = File.Exists(Path.Combine(library, name + ".Build.cs"));
        if (!data && !hasRule)
        {
            Fail(relativeLibrary, 0, $"missing module rule {name}.Build.cs");
        }

        if (data && hasRule)
        {
            Fail($"{relativeLibrary}/{name}.Build.cs", 0, "data library must not have a module rule (it is not built)");
        }

        foreach (string entry in Directory.EnumerateFileSystemEntries(library).Order(StringComparer.Ordinal))
        {
            string child = Path.GetFileName(entry);
            bool allowed = child == ThirdPartyManifest.FileName || child == name + ".Build.cs" ||
                           child == manifest.UpstreamDirectory || glue.Contains(child) || SystemMetadataFiles.Contains(child) ||
                           child == manifest.Patch;
            if (!allowed)
            {
                Fail(Relative(root, entry), 0, "unregistered file or directory in third-party library folder");
            }
        }

        foreach (string unit in glue)
        {
            if (unit.Contains('/'))
            {
                Fail($"{relativeLibrary}/{unit}", 0, "glue translation unit must be directly in the library folder");
            }
            else if (!File.Exists(Path.Combine(library, unit)))
            {
                Fail($"{relativeLibrary}/{unit}", 0, "registered glue translation unit is missing");
            }
        }

        // 원본 파일 집합·해시. 링크는 보고하고 따라 들어가지 않는다(순환 junction 방지).
        var actual = new SortedDictionary<string, byte[]>(StringComparer.Ordinal);
        EnumerationOptions options = new()
        {
            RecurseSubdirectories = true,
            AttributesToSkip = 0,
            IgnoreInaccessible = false
        };
        var enumeration = new FileSystemEnumerable<string>(upstream, (ref FileSystemEntry entry) => entry.ToFullPath(), options)
        {
            ShouldRecursePredicate = (ref FileSystemEntry entry) => (entry.Attributes & FileAttributes.ReparsePoint) == 0
        };
        foreach (string entry in enumeration)
        {
            string relative = Path.GetRelativePath(upstream, entry).Replace('\\', '/');
            if (SystemMetadataFiles.Contains(Path.GetFileName(entry)))
            {
                continue;
            }

            if (IsLink(entry))
            {
                Fail($"{relativeUpstream}/{relative}", 0, "links are not allowed in upstream sources");
                continue;
            }

            if (File.Exists(entry))
            {
                actual.Add(relative, File.ReadAllBytes(entry));
            }
        }

        foreach ((string file, string hash) in manifest.Files)
        {
            if (!actual.TryGetValue(file, out byte[]? bytes))
            {
                Fail($"{relativeUpstream}/{file}", 0, "file listed in manifest is missing");
            }
            else if (Convert.ToHexStringLower(SHA256.HashData(bytes)) != hash)
            {
                Fail($"{relativeUpstream}/{file}", 0, "SHA-256 differs from manifest (upstream sources must stay byte-identical)");
            }
        }

        foreach (string file in actual.Keys.Where(file => !manifest.Files.ContainsKey(file)))
        {
            Fail($"{relativeUpstream}/{file}", 0, "file is not listed in manifest");
        }

        CheckModifications(manifest, library, relativeLibrary, relativeUpstream, actual, Fail);

        // 경로 참조.
        bool InUpstream(string path) => path.StartsWith(manifest.UpstreamDirectory + "/", StringComparison.Ordinal) &&
                                        manifest.Files.ContainsKey(path[(manifest.UpstreamDirectory.Length + 1)..]);
        foreach (ThirdPartyLicense license in manifest.Licenses)
        {
            if (!InUpstream(license.File))
            {
                Fail($"{relativeLibrary}/{license.File}", 0, "license file must be a listed upstream file");
            }
        }

        foreach (string unit in manifest.TranslationUnits.Where(unit => !glue.Contains(unit)))
        {
            if (!InUpstream(unit))
            {
                Fail($"{relativeLibrary}/{unit}", 0, "translation unit must be a listed upstream file or registered glue");
            }
        }

        foreach (string header in manifest.AdapterHeaders)
        {
            if (!InUpstream(header))
            {
                Fail($"{relativeLibrary}/{header}", 0, "adapter header must be a listed upstream file");
            }
        }

        foreach (string directory in manifest.IncludeDirectories)
        {
            if ((directory != manifest.UpstreamDirectory &&
                 !directory.StartsWith(manifest.UpstreamDirectory + "/", StringComparison.Ordinal)) ||
                !Directory.Exists(Path.Combine(library, directory)))
            {
                Fail($"{relativeLibrary}/{directory}", 0, "include directory must exist inside the upstream directory");
            }
        }

        // 컴파일러 지시문 목록은 정확히 같아야 한다(파일·논리 줄 시작·원문). 데이터는 컴파일하지 않으므로 검사하지 않는다
        // (manifest의 목록은 리더가 비어 있음을 강제한다).
        var expected = manifest.PragmaDirectives.ToHashSet();
        HashSet<ThirdPartyPragma> found = data
            ? []
            : actual.SelectMany(file => ThirdPartyManifest.ScanPragmaDirectives(file.Key, file.Value)).ToHashSet();
        foreach (var item in found.Where(item => !expected.Contains(item)).OrderBy(item => item.File, StringComparer.Ordinal).ThenBy(item => item.Line))
        {
            Fail($"{relativeUpstream}/{item.File}", item.Line, "compiler directive is not recorded in manifest pragmaDirectives");
        }

        foreach (var item in expected.Where(item => !found.Contains(item)).OrderBy(item => item.File, StringComparer.Ordinal).ThenBy(item => item.Line))
        {
            Fail($"{relativeUpstream}/{item.File}", item.Line, "manifest pragmaDirectives entry does not match the source");
        }

        // adapter 헤더: 진단 억제는 같은 계열 push/pop 안에서만, 따옴표 include는 adapter 헤더끼리만.
        var adapters = manifest.AdapterHeaders.Where(InUpstream)
            .Select(header => header[(manifest.UpstreamDirectory.Length + 1)..]).ToHashSet(StringComparer.Ordinal);
        foreach (string file in adapters.Where(actual.ContainsKey).Order(StringComparer.Ordinal))
        {
            string path = $"{relativeUpstream}/{file}";
            CheckDirectiveBalance(path, ThirdPartyManifest.ScanPragmaDirectives(file, actual[file]), failures);
            string directory = Path.GetDirectoryName(file)?.Replace('\\', '/') ?? "";
            string[] lines = System.Text.Encoding.UTF8.GetString(actual[file]).Split('\n');
            for (int index = 0; index < lines.Length; ++index)
            {
                Match include = IncludeDirective.Match(lines[index]);
                if (!include.Success || include.Groups[1].Value != "\"")
                {
                    continue;
                }

                string target = Normalize(directory.Length == 0 ? include.Groups[2].Value : $"{directory}/{include.Groups[2].Value}");
                if (actual.ContainsKey(target) && !adapters.Contains(target))
                {
                    Fail(path, index + 1, $"adapter header includes non-adapter upstream file '{target}'");
                }
            }
        }

        return manifest;
    }

    // 진단 억제 지시문의 push/pop 균형을 계열별로 센다. MSVC warning 스택과 clang/GCC diagnostic 스택은 따로이며,
    // pack 등 진단과 무관한 push/pop은 세지 않는다. 억제가 스택 밖에 있거나 균형이 맞지 않으면 실패다.
    private static readonly Regex MsvcWarningAction = Pattern(
        "(?:#\\s*pragma\\s+warning|__pragma\\s*\\(\\s*warning)\\s*\\(\\s*(push|pop|disable|[0-4]|once|default|error|suppress)\\b");

    private static readonly Regex GnuDiagnosticAction = Pattern(
        "(?:#\\s*pragma|_Pragma\\s*\\(\\s*\")\\s*(?:clang|GCC)\\s+diagnostic\\s+(push|pop|ignored|warning|error)\\b");

    private static void CheckDirectiveBalance(string path, IReadOnlyList<ThirdPartyPragma> directives, List<CheckFailure> failures)
    {
        int msvc = 0;
        int gnu = 0;
        foreach (ThirdPartyPragma directive in directives)
        {
            foreach (Match match in MsvcWarningAction.Matches(directive.Text))
            {
                Apply(match.Groups[1].Value, ref msvc, directive.Line, suppression: match.Groups[1].Value is "disable" or "0" or "1" or "2" or "3" or "suppress");
            }

            foreach (Match match in GnuDiagnosticAction.Matches(directive.Text))
            {
                Apply(match.Groups[1].Value, ref gnu, directive.Line, suppression: match.Groups[1].Value is "ignored" or "warning");
            }
        }

        if (msvc != 0 || gnu != 0)
        {
            failures.Add(new(path, 0, "third-party", "unbalanced diagnostic push in adapter header"));
        }

        void Apply(string action, ref int depth, int line, bool suppression)
        {
            if (action == "push")
            {
                ++depth;
            }
            else if (action == "pop" && --depth < 0)
            {
                failures.Add(new(path, line, "third-party", "unbalanced diagnostic pop in adapter header"));
                depth = 0;
            }
            else if (suppression && depth == 0)
            {
                failures.Add(new(path, line, "third-party", "adapter header changes diagnostics outside push/pop"));
            }
        }
    }

    // 엔진 소유 소스가 외부 원본을 include하면 adapter 헤더여야 한다. include 경로는 manifest includeDirectories 기준이다.
    private static void CheckOwnedIncludes(string root, IReadOnlyList<(string Library, ThirdPartyManifest Manifest)> libraries,
        List<CheckFailure> failures)
    {
        var reachable = new Dictionary<string, (bool Adapter, string Library)>(StringComparer.Ordinal);
        var glueAllowed = new Dictionary<string, string>(StringComparer.Ordinal);
        foreach ((string library, ThirdPartyManifest manifest) in libraries)
        {
            string upstreamPrefix = manifest.UpstreamDirectory + "/";
            foreach (string directory in manifest.IncludeDirectories)
            {
                string inside = directory == manifest.UpstreamDirectory ? "" : directory[upstreamPrefix.Length..] + "/";
                foreach (string file in manifest.Files.Keys.Where(file => file.StartsWith(inside, StringComparison.Ordinal)))
                {
                    reachable.TryAdd(file[inside.Length..], (manifest.AdapterHeaders.Contains(upstreamPrefix + file), library));
                }
            }

            foreach (string unit in manifest.TranslationUnits.Where(unit => !unit.StartsWith(upstreamPrefix, StringComparison.Ordinal)))
            {
                glueAllowed[Path.GetFullPath(Path.Combine(library, unit))] = library;
            }
        }

        if (reachable.Count == 0)
        {
            return;
        }

        foreach (string path in NativeSources(root))
        {
            glueAllowed.TryGetValue(Path.GetFullPath(path), out string? glueLibrary);
            int line = 0;
            foreach (string text in File.ReadLines(path, StrictUtf8))
            {
                ++line;
                Match include = IncludeDirective.Match(text);
                if (!include.Success || !reachable.TryGetValue(include.Groups[2].Value, out var target) || target.Adapter)
                {
                    continue;
                }

                if (glueLibrary != null && string.Equals(glueLibrary, target.Library, StringComparison.Ordinal))
                {
                    continue;
                }

                failures.Add(new(Relative(root, path), line, "third-party",
                    $"includes non-adapter third-party file '{include.Groups[2].Value}' (list it in adapterHeaders or use the adapter)"));
            }
        }
    }

    private static string Normalize(string path)
    {
        var parts = new List<string>();
        foreach (string part in path.Split('/'))
        {
            if (part == "..")
            {
                if (parts.Count > 0)
                {
                    parts.RemoveAt(parts.Count - 1);
                }
            }
            else if (part.Length != 0 && part != ".")
            {
                parts.Add(part);
            }
        }

        return string.Join('/', parts);
    }

    private static bool IsLink(string path) => new FileInfo(path).Attributes.HasFlag(FileAttributes.ReparsePoint);

    /// <summary>
    /// compile DB 항목의 언어 인자가 파일 확장자와 맞는지 검사한다(rule id <c>warning-language</c>). 마지막 지정이
    /// 이기는 컴파일러 규칙대로 해석한다(<c>-x</c>·<c>-x&lt;lang&gt;</c>·<c>--language</c>·<c>/TC</c>·<c>/TP</c>, std 지정).
    /// <list type="bullet">
    /// <item>정확히 <c>.c</c>인 파일: C 모드와 C17이어야 하고 <c>Core/Source/ThirdParty/</c> 아래에만 있어야 한다.</item>
    /// <item>그 밖의 파일: C++ 모드와 C++23이어야 한다.</item>
    /// </list>
    /// <paramref name="directory"/>는 상대 <paramref name="file"/>의 기준 폴더다(compile DB의 directory).
    /// </summary>
    public static IReadOnlyList<CheckFailure> LanguageArguments(IReadOnlyList<string> arguments, string file, string? directory = null)
    {
        List<CheckFailure> failures = [];
        void Fail(string message) => failures.Add(new(file, 0, "warning-language", message));
        bool windows = arguments.Contains("/WX", StringComparer.Ordinal);
        bool c = Path.GetExtension(file).Equals(".c", StringComparison.Ordinal);

        string? language = null;
        string? standard = null;
        for (int index = 0; index < arguments.Count; ++index)
        {
            string argument = arguments[index];
            if (windows && argument is "/TC" or "/TP")
            {
                language = argument == "/TC" ? "c" : "c++";
            }
            else if (windows && argument.Length > 3 && (argument.StartsWith("/Tc", StringComparison.Ordinal) || argument.StartsWith("/Tp", StringComparison.Ordinal)))
            {
                language = argument[2] == 'c' ? "c" : "c++";
            }
            else if (argument == "-x" && index + 1 < arguments.Count)
            {
                language = arguments[++index];
            }
            else if (argument.StartsWith("-x", StringComparison.Ordinal) && argument.Length > 2)
            {
                language = argument[2..];
            }
            else if (argument.StartsWith("--language=", StringComparison.Ordinal))
            {
                language = argument["--language=".Length..];
            }
            else if (argument == "--language" && index + 1 < arguments.Count)
            {
                language = arguments[++index];
            }

            string stripped = argument.StartsWith("/clang:", StringComparison.Ordinal) ? argument["/clang:".Length..] : argument;
            if (stripped.StartsWith("-std=", StringComparison.Ordinal))
            {
                standard = stripped["-std=".Length..];
            }
            else if (windows && argument.StartsWith("/std:", StringComparison.Ordinal))
            {
                standard = argument["/std:".Length..];
            }
        }
        // POSIX driver(clang++)에서 언어 지정이 없으면 C++로 처리한다.
        language ??= windows ? null : "c++";

        if (c)
        {
            if (language != "c")
            {
                Fail("C source must be compiled in C mode (" + (windows ? "/TC" : "-x c") + ")");
            }

            if (standard != "c17")
            {
                Fail("C source must use the C17 standard");
            }

            string full = directory != null && !Path.IsPathRooted(file) ? Path.Combine(directory, file) : file;
            if (!Path.GetFullPath(full).Replace('\\', '/').Contains("/" + ThirdPartyManifest.ThirdPartyRoot + "/", StringComparison.Ordinal))
            {
                Fail("C sources are allowed only for third-party libraries under " + ThirdPartyManifest.ThirdPartyRoot);
            }
        }
        else
        {
            if (language != "c++")
            {
                Fail("C++ source must be compiled in C++ mode" + (windows ? " (/TP)" : ""));
            }

            if (standard != "c++23")
            {
                Fail("C++ source must use the C++23 standard");
            }
        }

        return failures;
    }
}
