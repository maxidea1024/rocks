using System.Text;
using System.Text.Json;
using System.Text.RegularExpressions;
using Rocks.Tooling.Common;

namespace Rocks.MetaCompiler;

internal static class CompilerDriver
{
    // 테스트 주입점은 명세 변경 시점을 결정적으로 검증하기 위한 내부 경계다.
    internal static async Task RunAsync(CompilerOptions options, Action? afterManifestRead = null,
        Action? beforeManifestVerification = null)
    {
        string? repository = FindRoot();
        string root = repository ?? Directory.GetCurrentDirectory();
        foreach (ModuleInput input in options.Inputs)
        {
            if (!File.Exists(input.Path))
            {
                throw new MetadataException($"input header not found: {input.Path}", "RMC1000", "Pass an existing input header.");
            }

            if (input.Path.IndexOfAny(['\r', '\n', '"']) >= 0)
            {
                throw new MetadataException("unsupported input path spelling", "RMC1000");
            }
        }

        Dictionary<string, string?> environment = await NativeEnvironment.CreateAsync(options.Compiler);
        string compiler = NativeEnvironment.FindExecutable(options.Compiler ?? (OperatingSystem.IsWindows() ? "clang-cl" : "clang++"), environment);
        bool clangCl = Path.GetFileNameWithoutExtension(compiler).Contains("clang-cl", StringComparison.OrdinalIgnoreCase);
        List<string> arguments = clangCl
            ? ["/nologo", "/TP", "/utf-8", "/clang:-std=c++23", "/W4", "/WX", "/GR-", "/EHs-c-", "/Zc:__cplusplus", "/clang:-fsyntax-only", "/clang:-Xclang", "/clang:-ast-dump=json"]
            : ["-x", "c++", "-std=c++23", "-Wall", "-Wextra", "-Wpedantic", "-Werror", "-fno-exceptions", "-fno-rtti", "-fsyntax-only", "-Xclang", "-ast-dump=json"];
        // 실제 빌드가 include 문맥을 제공하면 그 순서를 그대로 사용한다. 저장소 전체 헤더를 몰래
        // 추가하여 선언되지 않은 모듈 의존성을 통과시키지 않는다. 자동 탐색은 standalone fallback뿐이다.
        List<string> includes;
        if (options.IncludeDirectories.Count != 0)
        {
            includes = options.IncludeDirectories.Select(Path.GetFullPath).ToList();
        }
        else
        {
            IEnumerable<string> engineModules = repository is null ? [] : Directory.EnumerateDirectories(Path.Combine(repository, "Core", "Source", "Runtime"));
            includes = engineModules.Select(path => Path.Combine(path, "Public")).Where(Directory.Exists)
                .Order(StringComparer.Ordinal).Append(root).Concat(options.Inputs.Select(input => Path.GetDirectoryName(input.Path)!)).Distinct().ToList();
        }

        await ValidateCompilerArgumentsAsync(options.ClangArguments, clangCl, root, environment);
        foreach (ModuleInput input in options.Inputs)
        {
            ValidateIncludeBinding(input.Path, input.Include, includes);
        }

        void AddIncludes(List<string> command, IEnumerable<string> directories)
        {
            foreach (string path in directories)
            {
                if (clangCl)
                {
                    command.Add("/I" + path);
                }
                else
                {
                    command.Add("-I");
                    command.Add(path);
                }
            }
        }

        // 검증 컴파일은 같은 플래그에 include만 바꿔 쓴다(AST dump 제외).
        List<string> validationArguments = arguments.Where(argument => argument is not ("/clang:-Xclang" or "/clang:-ast-dump=json" or "-Xclang" or "-ast-dump=json")).ToList();
        AddIncludes(arguments, includes);
        arguments.AddRange(options.ClangArguments);
        arguments.Add("-");
        // 모듈의 리플렉션 헤더 전부를 한 번역 단위로 읽는다(같은 모듈 안 헤더 사이의 base·참조가 풀린다).
        string unit = string.Concat(options.Inputs.Select(input => $"#include \"{input.Path.Replace('\\', '/')}\"\n"));
        ProcessResult compiled = await ProcessRunner.RunAsync(new(compiler, arguments, root, environment, unit, TimeSpan.FromMinutes(2)));
        if (compiled.ExitCode != 0 || compiled.StdErr.Contains("warning:", StringComparison.OrdinalIgnoreCase))
        {
            throw new MetadataException("Clang rejected reflection input:\n" + compiled.StdErr, "RMC3000",
                "Fix the Clang diagnostic at the reported source location. Place ROCKS_CLASS before class or struct; do not suppress or downgrade warnings.");
        }

        (Dictionary<string, ClassModel> imported, List<string> importedModules) = LoadImports(options.Imports, options.Module);
        string? ResolveHeader(string spelling)
        {
            foreach (string directory in includes)
            {
                string candidate = Path.GetFullPath(Path.Combine(directory, spelling));
                if (File.Exists(candidate))
                {
                    return CanonicalPath.Resolve(candidate);
                }
            }

            return null;
        }

        byte[]? identitySnapshot = options.IdentityManifest is null ? null : ReadIdentitySnapshot(options.IdentityManifest);
        afterManifestRead?.Invoke();
        StableIdManifest? manifest = options.IdentityManifest is null ? null : StableIdManifest.Load(options.IdentityManifest);
        VerifyIdentitySnapshot(options.IdentityManifest, identitySnapshot);
        var parser = new ClangAstParser(options.Inputs, manifest, imported, ResolveHeader);
        SchemaModel schema;
        try
        {
            schema = parser.Parse(compiled.StdOut);
        }
        catch (Exception error) when (error is JsonException or KeyNotFoundException or InvalidOperationException or FormatException)
        {
            throw new MetadataException("unsupported Clang AST shape: " + error.Message, "RMC2001",
                "Use the supported Clang/toolchain and metadata subset; add a reviewed parser case before accepting a new AST shape.");
        }

        foreach (ClassModel type in schema.Ordered)
        {
            // 의존 모듈이 이미 리플렉션한 이름을 이 모듈이 다시 정의하면(헤더를 include하지 않은 재정의) 한 프로그램에서
            // 두 정의가 되므로 거부한다.
            if (imported.TryGetValue(type.Name, out ClassModel? other))
            {
                throw new MetadataException($"reflected type {type.Name} is declared by this module and by imported module {other.Module}", "RMC2004",
                    "Keep each reflected type in exactly one module; include the owning module's header instead of redeclaring it.").At(type.Site);
            }

            if (!type.Private && schema.Classes.TryGetValue(type.Base, out ClassModel? ownBase) && ownBase.Private)
            {
                throw new MetadataException($"{type.Name}: public reflected type derives from private type {ownBase.Name}", "RMC2006",
                    "Move the base to a public header or make the derived type private.").At(type.Site);
            }
        }

        // 생성물: 입력 헤더마다 하나, public·private 집계 헤더, descriptor.
        List<(string Path, string Text, bool Private)> outputs = [];
        foreach (ModuleInput input in options.Inputs)
        {
            outputs.Add((options.OutputPath(input), CppEmitter.Emit(schema, input), input.Private));
        }

        if (options.Inputs.Any(input => !input.Private))
        {
            outputs.Add((options.AggregatePath(false), CppEmitter.EmitAggregate(options.Module, schema, options.Inputs, importedModules, false), false));
        }

        if (options.Inputs.Any(input => input.Private))
        {
            outputs.Add((options.AggregatePath(true), CppEmitter.EmitAggregate(options.Module, schema, options.Inputs, importedModules, true), true));
        }

        string formatter = NativeEnvironment.FindExecutable(Environment.GetEnvironmentVariable("ROCKS_CLANG_FORMAT") ?? "clang-format", environment);
        ProcessResult version = await ProcessRunner.RunAsync(new(formatter, ["--version"], root, environment, Timeout: TimeSpan.FromSeconds(20)));
        if (version.ExitCode != 0 || !version.StdOut.Contains("version 21.1.6", StringComparison.Ordinal))
        {
            throw new MetadataException("Rocks requires clang-format 21.1.6", "RMC3100", "Set ROCKS_CLANG_FORMAT to the pinned formatter executable.");
        }

        UTF8Encoding utf8 = new(false, true);
        List<(string Path, byte[] Bytes, bool Private)> formattedOutputs = [];
        foreach ((string path, string text, bool isPrivate) in outputs)
        {
            ProcessResult formatted = await ProcessRunner.RunAsync(new(formatter,
                ["--style=file:" + Path.Combine(AppContext.BaseDirectory, ".clang-format"), "--assume-filename=Metadata.h"], root, environment, text, TimeSpan.FromSeconds(30)));
            if (formatted.ExitCode != 0 || formatted.StdErr.Contains("warning:", StringComparison.OrdinalIgnoreCase))
            {
                throw new MetadataException("clang-format failed: " + formatted.StdErr, "RMC3100", "Fix the formatter/style error without suppressing diagnostics.");
            }

            formattedOutputs.Add((path, utf8.GetBytes(formatted.StdOut.Replace("\r\n", "\n", StringComparison.Ordinal)), isPrivate));
        }

        // C++ 생성/소멸·noexcept·private friend·typed accessor의 실제 제약을 출력 게시 전에 검사한다. 소비자가
        // 생성 헤더를 하나씩 include하므로 헤더마다 따로 컴파일한다. 생성물은 임시 루트에 두고, include 검색 순서에서
        // 출력 루트(Public·Private)가 있던 자리를 그 임시 루트로 바꾼다 — 소비자와 같은 순서로 풀리고 이전 실행의 낡은
        // 게시본을 보지 않는다. 검색 순서에 출력 루트가 없으면(단독 실행) 맨 앞에 둔다.
        string validationRoot = Path.Combine(Path.GetDirectoryName(options.OutputRoot)!, ".rocks-meta-validation-" + Guid.NewGuid().ToString("N"));
        string outputPublic = Path.Combine(options.OutputRoot, "Public");
        string outputPrivate = Path.Combine(options.OutputRoot, "Private");
        StringComparer pathComparer = OperatingSystem.IsWindows() ? StringComparer.OrdinalIgnoreCase : StringComparer.Ordinal;
        List<string> validationIncludes = includes.Select(directory =>
            pathComparer.Equals(directory, outputPublic) ? Path.Combine(validationRoot, "Public")
            : pathComparer.Equals(directory, outputPrivate) ? Path.Combine(validationRoot, "Private")
            : directory).ToList();
        if (!includes.Contains(outputPublic, pathComparer) && !includes.Contains(outputPrivate, pathComparer))
        {
            validationIncludes.InsertRange(0, [Path.Combine(validationRoot, "Public"), Path.Combine(validationRoot, "Private")]);
        }

        AddIncludes(validationArguments, validationIncludes);
        validationArguments.AddRange(options.ClangArguments);
        try
        {
            foreach ((string path, byte[] bytes, bool isPrivate) in formattedOutputs)
            {
                string relative = Path.GetRelativePath(options.OutputRoot, path);
                string staged = Path.Combine(validationRoot, relative);
                Directory.CreateDirectory(Path.GetDirectoryName(staged)!);
                File.WriteAllBytes(staged, bytes);
            }

            Directory.CreateDirectory(Path.Combine(validationRoot, "Public"));
            Directory.CreateDirectory(Path.Combine(validationRoot, "Private"));
            foreach ((string path, byte[] _, bool isPrivate) in formattedOutputs)
            {
                string relative = Path.GetRelativePath(Path.Combine(options.OutputRoot, isPrivate ? "Private" : "Public"), path).Replace('\\', '/');
                string source = Path.Combine(validationRoot, "check-" + Hashing.Sha256Text(relative)[..16] + ".cpp");
                File.WriteAllText(source, $"#include <{relative}>\n", new UTF8Encoding(false, true));
                List<string> command = [.. validationArguments, source];
                ProcessResult validated = await ProcessRunner.RunAsync(new(compiler, command, root, environment, Timeout: TimeSpan.FromMinutes(2)));
                if (validated.ExitCode != 0 || validated.StdErr.Contains("warning:", StringComparison.OrdinalIgnoreCase))
                {
                    throw new MetadataException($"generated metadata contract rejected by Clang ({relative}):\n" + validated.StdErr + SourceNotes(schema, validated.StdErr), "RMC3000",
                        "Fix constructor/destructor noexcept, CLASS/PROPERTY access and GENERATED_BODY friendship; every generated typed operation must compile before output is accepted.");
                }
            }
        }
        finally
        {
            if (Directory.Exists(validationRoot))
            {
                Directory.Delete(validationRoot, true);
            }
        }

        byte[] descriptor = ModuleDescriptor.Write(options.Module, schema.Ordered);
        beforeManifestVerification?.Invoke();
        // 외부 편집은 파일 잠금을 따르지 않을 수 있다. 이 비교는 확인 시점의 변경을 감지하지만
        // 검사 이후 게시 사이에 다른 프로세스가 바꾸는 것까지 OS 원자 CAS로 막지는 않는다.
        VerifyIdentitySnapshot(options.IdentityManifest, identitySnapshot);
        if (options.Check)
        {
            foreach ((string path, byte[] bytes, bool _) in formattedOutputs)
            {
                if (!File.Exists(path) || !File.ReadAllBytes(path).AsSpan().SequenceEqual(bytes))
                {
                    throw new MetadataException($"generated output is stale: {path}", "RMC4000", "Regenerate with the same command without --check, then rebuild.");
                }
            }

            if (!File.Exists(options.Descriptor) || !File.ReadAllBytes(options.Descriptor).AsSpan().SequenceEqual(descriptor))
            {
                throw new MetadataException($"generated descriptor is stale: {options.Descriptor}", "RMC4000", "Regenerate with the same command without --check, then rebuild.");
            }

            HashSet<string> expected = formattedOutputs.Select(output => Path.GetFullPath(output.Path)).ToHashSet(
                OperatingSystem.IsWindows() ? StringComparer.OrdinalIgnoreCase : StringComparer.Ordinal);
            if (Directory.Exists(options.OutputRoot))
            {
                foreach (string existing in Directory.EnumerateFiles(options.OutputRoot, "*", SearchOption.AllDirectories))
                {
                    if (!expected.Contains(Path.GetFullPath(existing)))
                    {
                        throw new MetadataException($"stale generated file: {existing}", "RMC4000",
                            "Delete the generated file of a header that is no longer an input (a standalone run does not delete outputs; the build's publish step does), then check again.");
                    }
                }
            }
        }

        if (options.IdentityInventory is not null)
        {
            static object Declaration(string kind, string name, string id, SourceSite site)
            {
                if (site.Line < 1 || !File.Exists(site.File))
                {
                    throw new MetadataException($"{name}: identity inventory source location is unknown", "RMC2100",
                        "Declare the type and property directly in a known source file.").At(site);
                }

                return new { kind, name, id, file = CanonicalPath.Resolve(site.File), line = site.Line };
            }

            var declarations = schema.Ordered.SelectMany(type =>
                new[] { Declaration("type", type.Name, type.Id, type.Site) }
                    .Concat(type.OwnFields.Select(field => Declaration("field", field.Owner + "::" + field.Name, field.Id, field.Site))))
                .ToArray();
            string json = JsonSerializer.Serialize(new { schema = 1, declarations }, new JsonSerializerOptions { WriteIndented = true, NewLine = "\n" }) + "\n";
            AtomicFile.WriteIfChanged(options.IdentityInventory, new UTF8Encoding(false, true).GetBytes(json));
            // inventory는 임시 결과다. 두 파일 간 게시 경쟁도 다시 검사하되 전체 파일 트랜잭션은
            // 빌드 호출자가 담당한다. 실패한 실행의 inventory를 성공 결과로 사용하지 않는다.
            VerifyIdentitySnapshot(options.IdentityManifest, identitySnapshot);
        }

        if (!options.Check)
        {
            foreach ((string path, byte[] bytes, bool _) in formattedOutputs)
            {
                AtomicFile.WriteIfChanged(path, bytes);
            }

            AtomicFile.WriteIfChanged(options.Descriptor, descriptor);
        }

        Console.WriteLine($"Rocks metadata: module {options.Module}: {schema.Ordered.Count} types; {(options.Check ? "checked" : "generated")}");
    }

    // import descriptor를 읽어 정규 이름 → 타입 모델과, public 타입이 있는 모듈 목록을 만든다. 같은 이름이
    // 두 descriptor에 있으면 RMC2004다.
    private static (Dictionary<string, ClassModel> Types, List<string> PublicModules) LoadImports(IEnumerable<string> descriptors, string self)
    {
        Dictionary<string, ClassModel> types = new(StringComparer.Ordinal);
        HashSet<string> modules = new(StringComparer.OrdinalIgnoreCase);
        List<string> publicModules = [];
        foreach (string path in descriptors)
        {
            (string module, List<ClassModel> models) = ModuleDescriptor.Read(path);
            // 자기 descriptor를 import하면 집계 헤더가 자기 RegisterModuleTypes를 다시 불러 무한 재귀가 된다.
            if (string.Equals(module, self, StringComparison.OrdinalIgnoreCase))
            {
                throw new MetadataException($"module {self} imports its own descriptor: {path}", "RMC2004", "Import only dependency modules' descriptors.");
            }

            if (!modules.Add(module))
            {
                throw new MetadataException($"module {module} imported twice", "RMC2004", "Pass each dependency descriptor once.");
            }

            foreach (ClassModel model in models)
            {
                if (!types.TryAdd(model.Name, model))
                {
                    throw new MetadataException($"reflected type {model.Name} is declared by two imported modules ({types[model.Name].Module}, {module})", "RMC2004",
                        "Keep each reflected type in exactly one module.");
                }
            }

            if (models.Any(model => !model.Private))
            {
                publicModules.Add(module);
            }
        }

        return (types, publicModules);
    }

    private static byte[] ReadIdentitySnapshot(string path)
    {
        return BoundedFile.Read(path, BoundedFile.MaximumIdentityBytes);
    }

    private static void VerifyIdentitySnapshot(string? path, byte[]? original)
    {
        if (path is null || original is null)
        {
            return;
        }

        if (!original.AsSpan().SequenceEqual(ReadIdentitySnapshot(path)))
        {
            throw new MetadataException($"identity manifest changed during metadata generation: {path}", "RMC2100",
                "Stop concurrent manifest edits and rebuild; no generated output was published.");
        }
    }

    private static async Task ValidateCompilerArgumentsAsync(IReadOnlyList<string> arguments, bool clangCl,
        string root, IReadOnlyDictionary<string, string?> environment)
    {
        // 허용된 프로필의 의미를 추측해 우회 인자 목록을 계속 추가하지 않는다. 프로필 확장은
        // native compile과 AST의 동등성·회귀 테스트를 함께 검토한 뒤 이 계약에 명시한다.
        var windows = new HashSet<string>(StringComparer.Ordinal)
        {
            "/nologo", "/utf-8", "/W4", "/WX", "/GR-", "/EHs-c-", "/Zc:__cplusplus",
            "/clang:-std=c++23", "/clang:-ffp-contract=off", "/MD", "/MDd", "/O2", "/Od", "/Z7",
            "/fsanitize=address"
        };
        var unix = new HashSet<string>(StringComparer.Ordinal)
        {
            "-std=c++23", "-Wall", "-Wextra", "-Wpedantic", "-Werror", "-fno-rtti",
            "-fno-exceptions", "-ffp-contract=off", "-O2", "-O0", "-g", "-fsanitize=address",
            "-fno-omit-frame-pointer", "-fPIC"
        };
        string? sdk = null;
        var groups = new Dictionary<string, string>(StringComparer.Ordinal);
        void Remember(string group, string argument)
        {
            if (!groups.TryAdd(group, argument))
            {
                throw new MetadataException($"conflicting or duplicate compiler options: {groups[group]} and {argument}",
                    "RMC1000", "Specify each native profile setting and macro once; remove the conflicting option.");
            }
        }

        for (int index = 0; index < arguments.Count; ++index)
        {
            string argument = arguments[index];
            if ((clangCl ? windows : unix).Contains(argument))
            {
                string group = argument switch
                {
                    "/MD" or "/MDd" => "windows-runtime",
                    "/O2" or "/Od" => "windows-optimization",
                    "-O2" or "-O0" => "mac-optimization",
                    _ => "flag:" + argument
                };
                Remember(group, argument);
                continue;
            }
            // Android 교차 빌드 대상(NDK clang)은 검토한 ABI와 API level만 받는다.
            if (!clangCl && argument.StartsWith("--target=", StringComparison.Ordinal))
            {
                if (!System.Text.RegularExpressions.Regex.IsMatch(argument,
                        "^--target=(aarch64|x86_64)-linux-android(2[6-9]|3[0-9])$"))
                {
                    throw InvalidArgument(argument);
                }

                Remember("target", argument);
                continue;
            }

            if (!clangCl && argument == "-isysroot")
            {
                Remember("mac-sysroot", argument);
                if (++index >= arguments.Count)
                {
                    throw InvalidArgument(argument);
                }

                string path = arguments[index];
                if (!Path.IsPathFullyQualified(path) || !Directory.Exists(path) || path.IndexOfAny(['\0', '\r', '\n']) >= 0)
                {
                    throw new MetadataException("invalid macOS SDK sysroot: " + path, "RMC1000",
                        "Pass the existing absolute SDK directory selected by xcrun --show-sdk-path.");
                }

                sdk = CanonicalPath.Resolve(path);
                continue;
            }

            if (!clangCl && argument.StartsWith("-mmacosx-version-min=", StringComparison.Ordinal))
            {
                string version = argument["-mmacosx-version-min=".Length..];
                if (Version.TryParse(version, out var minimum) && minimum.Major >= 13 &&
                    version.All(value => char.IsAsciiDigit(value) || value == '.'))
                {
                    Remember("mac-deployment", argument);
                    continue;
                }

                throw InvalidArgument(argument);
            }

            int prefix = argument.StartsWith("-D", StringComparison.Ordinal) ||
                (clangCl && argument.StartsWith("/D", StringComparison.Ordinal)) ? 2 : 0;
            if (prefix != 0)
            {
                string define = argument[prefix..];
                int equals = define.IndexOf('=');
                string name = equals < 0 ? define : define[..equals];
                if (name.Length != 0 && (char.IsAsciiLetter(name[0]) || name[0] == '_') &&
                    name.All(value => char.IsAsciiLetterOrDigit(value) || value == '_') &&
                    name is not ("_CRT_SECURE_NO_WARNINGS" or "_SCL_SECURE_NO_WARNINGS") &&
                    define.IndexOfAny(['\0', '\r', '\n']) < 0)
                {
                    Remember("define:" + name, argument);
                    continue;
                }
            }

            throw InvalidArgument(argument);
        }

        if (groups.ContainsKey("flag:/fsanitize=address") &&
            groups.TryGetValue("windows-runtime", out string? runtime) && runtime == "/MDd")
        {
            throw new MetadataException("conflicting compiler options: /fsanitize=address and /MDd", "RMC1000",
                "Use the address-sanitizer native profile with /MD instead of /MDd.");
        }

        if (sdk is not null)
        {
            if (!OperatingSystem.IsMacOS())
            {
                throw InvalidArgument("-isysroot");
            }

            string xcrun = NativeEnvironment.FindExecutable("xcrun", environment);
            ProcessResult selected = await ProcessRunner.RunAsync(new(xcrun, ["--show-sdk-path"], root, environment,
                Timeout: TimeSpan.FromSeconds(30)));
            if (selected.ExitCode != 0 || !Directory.Exists(selected.StdOut.Trim()) ||
                !CanonicalPath.Equals(sdk, selected.StdOut.Trim()))
            {
                throw new MetadataException("SDK sysroot does not match the selected macOS SDK: " + sdk,
                    "RMC1000", "Pass the SDK selected by xcrun --show-sdk-path or change the selected Xcode SDK.");
            }
        }
    }

    private static MetadataException InvalidArgument(string argument) => new(
        "unsupported compiler option override: " + argument, "RMC1000",
        "Use the documented native profile, explicit -D or /D definitions, and --include-dir; unsupported flags require a reviewed profile extension.");

    private static void ValidateIncludeBinding(string input, string include, IEnumerable<string> directories)
    {
        string? target = directories.Select(directory => Path.GetFullPath(Path.Combine(directory, include)))
            .FirstOrDefault(File.Exists);
        string canonicalInput = CanonicalPath.Resolve(input);
        string? canonicalTarget = target is null ? null : CanonicalPath.Resolve(target);
        if (canonicalTarget is null || !string.Equals(canonicalInput, canonicalTarget,
                OperatingSystem.IsWindows() ? StringComparison.OrdinalIgnoreCase : StringComparison.Ordinal))
        {
            throw new MetadataException($"--include resolves to a different header than --input; input={input}; resolved input={canonicalInput}; " +
                $"include target={target ?? "<not found>"}; resolved target={canonicalTarget ?? "<not found>"}", "RMC1000",
                "Pass the same author header through the target's explicit include search order; remove shadowing include directories.");
        }
    }

    private static string SourceNotes(SchemaModel schema, string diagnostic)
    {
        // 이름의 일부 문자열은 별도 타입과 혼동된다. 완전한 qualified 이름만 확정 참조로
        // 표시하고, 짧은 이름만 나온 경우에는 원인이라고 단정하지 않고 후보로 구분한다.
        bool Token(string value) => Regex.IsMatch(diagnostic,
            @"(?<![\p{L}\p{Nd}_:])(?:::)?" + Regex.Escape(value) + @"(?![\p{L}\p{Nd}_])",
            RegexOptions.CultureInvariant, TimeSpan.FromSeconds(1));
        StringBuilder notes = new();
        foreach (ClassModel type in schema.Ordered)
        {
            bool qualified = type.Name.Contains("::", StringComparison.Ordinal) && Token(type.Name);
            string shortName = type.Name.Split("::")[^1];
            if (!qualified && !Token(shortName))
            {
                continue;
            }

            string description = qualified ? "diagnostic references CLASS" : "candidate CLASS";
            notes.AppendLine($"{type.Site.File}:{type.Site.Line}: note: {description} {type.Name}");
            foreach (FieldModel field in type.Fields)
            {
                if (Token(field.Name))
                {
                    notes.AppendLine($"{field.Site.File}:{field.Site.Line}: note: candidate PROPERTY {field.Owner}::{field.Name}");
                }
            }
        }

        return notes.ToString();
    }

    private static string? FindRoot()
    {
        for (DirectoryInfo? directory = new(AppContext.BaseDirectory); directory is not null; directory = directory.Parent)
        {
            if (Directory.Exists(Path.Combine(directory.FullName, "Core", "Source", "Runtime")) &&
                Directory.Exists(Path.Combine(directory.FullName, "Programs")))
            {
                return directory.FullName;
            }
        }

        return null; // SDK 배포에서는 명시 include 경로와 함께 저장소 밖에서도 실행할 수 있다.
    }
}
