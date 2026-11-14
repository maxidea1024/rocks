using Rocks.Tooling.Common;

namespace Rocks.MetaCompiler;

// 모듈 하나의 리플렉션 헤더를 한 번에 생성한다(Docs/Design/ReflectionFoundations.ko.md 1.3절).
internal sealed record ModuleInput(string Path, string Include, bool Private);

internal sealed class CompilerOptions
{
    public required string Module { get; init; }

    public required IReadOnlyList<ModuleInput> Inputs { get; init; }

    public required string OutputRoot { get; init; }

    public required string Descriptor { get; init; }

    public string? Compiler { get; init; }

    public string? IdentityManifest { get; init; }

    public string? IdentityInventory { get; init; }

    public bool Check { get; init; }

    public List<string> Imports { get; } = [];

    public List<string> IncludeDirectories { get; } = [];

    public List<string> ClangArguments { get; } = [];

    // 생성 헤더의 철자: 원본 철자의 확장자 ".h"를 ".generated.h"로 바꾼다.
    public static string GeneratedSpelling(string include) => include[..^2] + ".generated.h";

    // 모듈 집계 헤더의 철자다(public 생성 루트 기준). private 타입은 private 집계 헤더가 등록한다.
    public static string AggregateSpelling(string module, bool privateTypes) =>
        privateTypes ? $"{module}/{module}.private.module.generated.h" : $"{module}/{module}.module.generated.h";

    public string OutputPath(ModuleInput input) =>
        Path.Combine(OutputRoot, input.Private ? "Private" : "Public", GeneratedSpelling(input.Include));

    public string AggregatePath(bool privateTypes) =>
        Path.Combine(OutputRoot, privateTypes ? "Private" : "Public", AggregateSpelling(Module, privateTypes));

    public static CompilerOptions Parse(string[] arguments)
    {
        Dictionary<string, string> single = new(StringComparer.Ordinal);
        List<string> inputs = [];
        List<string> spellings = [];
        HashSet<string> privates = new(StringComparer.Ordinal);
        List<string> imports = [];
        List<string> includes = [];
        List<string> clang = [];
        bool check = false;
        for (int index = 0; index < arguments.Length; ++index)
        {
            string argument = arguments[index];
            if (argument == "--check")
            {
                if (check)
                {
                    throw new MetadataException("duplicate option --check", "RMC1000", "Specify --check only once.");
                }

                check = true;
                continue;
            }

            int equals = argument.IndexOf('=');
            string key = equals < 0 ? argument : argument[..equals];
            if (key is not ("--module" or "--input" or "--include" or "--private" or "--output-root" or "--descriptor" or "--import" or
                "--compiler" or "--include-dir" or "--clang-arg" or "--identity-manifest" or "--identity-inventory"))
            {
                throw new MetadataException($"unknown option {key}", "RMC1000", "Use --module, paired --input/--include, --output-root, --descriptor and documented optional arguments.");
            }

            if (equals < 0 && index + 1 == arguments.Length)
            {
                throw new MetadataException($"missing value for {key}", "RMC1000", "Supply an explicit option value.");
            }

            string value = equals < 0 ? arguments[++index] : argument[(equals + 1)..];
            switch (key)
            {
                case "--input":
                    inputs.Add(value);
                    break;
                case "--include":
                    spellings.Add(value);
                    break;
                case "--private":
                    if (!privates.Add(value))
                    {
                        throw new MetadataException("duplicate --private " + value, "RMC1000", "Mark each private header once.");
                    }

                    break;
                case "--import":
                    imports.Add(value);
                    break;
                case "--include-dir":
                    includes.Add(value);
                    break;
                case "--clang-arg":
                    clang.Add(value);
                    break;
                default:
                    if (!single.TryAdd(key, value))
                    {
                        throw new MetadataException($"duplicate option {key}", "RMC1000", "Specify each scalar option once.");
                    }

                    break;
            }
        }

        foreach (string key in new[] { "--module", "--output-root", "--descriptor" })
        {
            if (!single.TryGetValue(key, out string? value) || string.IsNullOrEmpty(value))
            {
                throw new MetadataException($"required option missing: {key}", "RMC1000", "Supply --module, --output-root and --descriptor.");
            }
        }

        string module = single["--module"];
        if (!char.IsAsciiLetter(module[0]) || !module.All(c => char.IsAsciiLetterOrDigit(c) || c == '_'))
        {
            throw new MetadataException("invalid --module name " + module, "RMC1000", "Use the native module name (ASCII identifier).");
        }

        if (inputs.Count == 0 || inputs.Count != spellings.Count)
        {
            throw new MetadataException("--input and --include must be given in pairs (at least one)", "RMC1000",
                "Pass one --include spelling after each --input header.");
        }

        List<ModuleInput> pairs = [];
        HashSet<string> seenSpellings = new(StringComparer.OrdinalIgnoreCase);
        for (int index = 0; index < inputs.Count; ++index)
        {
            string include = spellings[index];
            if (Path.IsPathRooted(include) || include.IndexOfAny(['\0', '\r', '\n', '"', '\\', ':']) >= 0 ||
                include.Split('/').Any(part => part.Length == 0 || part is "." or "..") || !include.EndsWith(".h", StringComparison.Ordinal))
            {
                throw new MetadataException("unsafe or ambiguous --include path " + include, "RMC1000",
                    "Use a relative forward-slash .h spelling without parent traversal, empty segments or quotes.");
            }

            if (include.EndsWith(".module.h", StringComparison.OrdinalIgnoreCase) || include.EndsWith(".generated.h", StringComparison.OrdinalIgnoreCase))
            {
                throw new MetadataException("reflected header name ending in .module.h or .generated.h is reserved: " + include, "RMC2005",
                    "Rename the header; .generated.h names generated headers and .module.generated.h the module aggregate header.");
            }

            if (!seenSpellings.Add(include))
            {
                throw new MetadataException("duplicate or case-only different --include spelling " + include, "RMC2005",
                    "Give each reflected header a distinct spelling (case-insensitively).");
            }

            pairs.Add(new(Path.GetFullPath(inputs[index]), include, privates.Contains(include)));
        }

        foreach (string marked in privates)
        {
            if (!spellings.Contains(marked, StringComparer.Ordinal))
            {
                throw new MetadataException("--private names an unknown --include " + marked, "RMC1000", "Mark only headers passed with --include.");
            }
        }

        // 끝 구분자를 뗀다. 검증 디렉터리는 출력 루트의 부모 아래에 만들므로(CompilerDriver) 끝 구분자가 있으면 출력 루트 안에
        // 생기고, 파일시스템 루트면 부모가 없다.
        string outputRoot = Path.TrimEndingDirectorySeparator(Path.GetFullPath(single["--output-root"]));
        if (Path.GetDirectoryName(outputRoot) is null)
        {
            throw new MetadataException("output root must not be a filesystem root: " + outputRoot, "RMC1000",
                "Use a dedicated directory such as <out>/Generated/<Module>; validation staging is created next to it.");
        }

        string descriptor = Path.GetFullPath(single["--descriptor"]);
        if (!descriptor.EndsWith(".rocksmeta.json", StringComparison.Ordinal))
        {
            throw new MetadataException("descriptor must use the .rocksmeta.json suffix", "RMC1000", "Pass <Module>.rocksmeta.json.");
        }

        // symlink 별칭으로 출력 루트가 입력을 담는 경우도 막도록 정규 경로로 비교한다.
        string canonicalRoot = CanonicalPath.Resolve(outputRoot);
        foreach (ModuleInput input in pairs)
        {
            if (CanonicalPath.Equals(input.Path, descriptor) || DirectoryPath.IsStrictlyUnder(CanonicalPath.Resolve(input.Path), canonicalRoot))
            {
                throw new MetadataException("input header must be outside the output root and descriptor", "RMC1000",
                    "Never overwrite an author header with generated output.");
            }
        }

        string? manifest = single.GetValueOrDefault("--identity-manifest");
        string? inventory = single.GetValueOrDefault("--identity-inventory");
        // 출력 루트의 파일은 모두 생성 헤더다(--check가 잉여 파일로 본다). descriptor·inventory는 그 밖에 둔다.
        foreach ((string what, string? path) in new[] { ("descriptor", (string?)descriptor), ("identity inventory", inventory) })
        {
            if (path is not null && DirectoryPath.IsAtOrUnder(CanonicalPath.Resolve(Path.GetFullPath(path)), canonicalRoot))
            {
                throw new MetadataException($"{what} must be outside the output root: {path}", "RMC1000",
                    "Keep only generated headers under --output-root; place the descriptor and inventory next to it.");
            }
        }

        if (manifest is not null && string.IsNullOrWhiteSpace(manifest))
        {
            throw new MetadataException("empty --identity-manifest", "RMC1000");
        }

        if (manifest is not null && !manifest.EndsWith(".rocksids.json", StringComparison.Ordinal))
        {
            throw new MetadataException("identity manifest must use the .rocksids.json suffix", "RMC1000",
                "Use the module's source-controlled identity manifest.");
        }

        if (inventory is not null && (string.IsNullOrWhiteSpace(inventory) || manifest is null))
        {
            throw new MetadataException("identity inventory requires a path and an identity manifest", "RMC1000");
        }

        if (inventory is not null && (Directory.Exists(inventory) || CanonicalPath.Equals(manifest!, inventory) ||
            CanonicalPath.Equals(descriptor, inventory) || pairs.Any(input => CanonicalPath.Equals(input.Path, inventory))))
        {
            throw new MetadataException("identity inventory must be a separate scratch file", "RMC1000",
                "Use an inventory path distinct from sources, descriptor and the source-controlled manifest.");
        }

        foreach (string import in imports)
        {
            if (!File.Exists(import) || !import.EndsWith(".rocksmeta.json", StringComparison.Ordinal))
            {
                throw new MetadataException("import descriptor does not exist: " + import, "RMC1000", "Pass dependency <Module>.rocksmeta.json files.");
            }
        }

        foreach (string includeDirectory in includes)
        {
            if (string.IsNullOrWhiteSpace(includeDirectory) || !Directory.Exists(includeDirectory))
            {
                throw new MetadataException("include directory does not exist: " + includeDirectory, "RMC1000", "Pass each real include directory in the native target's search order.");
            }
        }

        if (clang.Any(string.IsNullOrWhiteSpace))
        {
            throw new MetadataException("empty --clang-arg", "RMC1000", "Remove empty compiler arguments.");
        }

        CompilerOptions options = new()
        {
            Module = module,
            Inputs = pairs,
            OutputRoot = outputRoot,
            Descriptor = descriptor,
            Compiler = single.GetValueOrDefault("--compiler"),
            IdentityManifest = manifest,
            IdentityInventory = inventory,
            Check = check
        };
        options.Imports.AddRange(imports.Select(Path.GetFullPath));
        options.IncludeDirectories.AddRange(includes);
        options.ClangArguments.AddRange(clang);
        return options;
    }
}
