namespace Rocks.MetaCompiler;

internal sealed class CompilerOptions
{
    public required string Input { get; init; }
    public required string Include { get; init; }
    public required string Output { get; init; }
    public string? Compiler { get; init; }
    public string? IdentityManifest { get; init; }
    public string? IdentityInventory { get; init; }
    public bool Check { get; init; }
    public List<string> IncludeDirectories { get; } = [];
    public List<string> ClangArguments { get; } = [];

    public static CompilerOptions Parse(string[] arguments)
    {
        Dictionary<string, string> single = new(StringComparer.Ordinal);
        List<string> includes = [];
        List<string> clang = [];
        bool check = false;
        for (int index = 0; index < arguments.Length; ++index)
        {
            string argument = arguments[index];
            if (argument == "--check") { if (check) throw new MetadataException("duplicate option --check", "RMC1000", "Specify --check only once."); check = true; continue; }
            int equals = argument.IndexOf('=');
            string key = equals < 0 ? argument : argument[..equals];
            if (key is not ("--input" or "--include" or "--output" or "--compiler" or "--include-dir" or "--clang-arg" or "--identity-manifest" or "--identity-inventory"))
                throw new MetadataException($"unknown option {key}", "RMC1000", "Use --input, --include, --output and documented optional arguments.");
            if (equals < 0 && index + 1 == arguments.Length)
                throw new MetadataException($"missing value for {key}", "RMC1000", "Supply an explicit option value.");
            string value = equals < 0 ? arguments[++index] : argument[(equals + 1)..];
            if (key == "--include-dir") includes.Add(value);
            else if (key == "--clang-arg") clang.Add(value);
            else if (!single.TryAdd(key, value)) throw new MetadataException($"duplicate option {key}", "RMC1000", "Specify each scalar option once.");
        }
        foreach (string key in new[] { "--input", "--include", "--output" })
            if (!single.TryGetValue(key, out string? value) || string.IsNullOrEmpty(value))
                throw new MetadataException($"required option missing: {key}", "RMC1000", "Supply all three input/include/output options.");
        string input = Path.GetFullPath(single["--input"]);
        string output = Path.GetFullPath(single["--output"]);
        if (CanonicalPath.Equals(input, output))

            throw new MetadataException("input and output must be different files", "RMC1000", "Use a separate generated header path; never overwrite an author header.");
        if (Directory.Exists(output)) throw new MetadataException("output path is a directory", "RMC1000", "Specify a generated file path.");
        string? manifest = single.GetValueOrDefault("--identity-manifest");
        string? inventory = single.GetValueOrDefault("--identity-inventory");
        if (manifest is not null && string.IsNullOrWhiteSpace(manifest))
            throw new MetadataException("empty --identity-manifest", "RMC1000");
        if (manifest is not null && !manifest.EndsWith(".rocksids.json", StringComparison.Ordinal))
            throw new MetadataException("identity manifest must use the .rocksids.json suffix", "RMC1000",
                "Use the source-controlled identity manifest selected by the build target.");
        if (inventory is not null && (string.IsNullOrWhiteSpace(inventory) || manifest is null))
            throw new MetadataException("identity inventory requires a path and an identity manifest", "RMC1000");
        if (inventory is not null && (Directory.Exists(inventory) || CanonicalPath.Equals(input, inventory) ||
            CanonicalPath.Equals(output, inventory) || CanonicalPath.Equals(manifest!, inventory)))
            throw new MetadataException("identity inventory must be a separate scratch file", "RMC1000",
                "Use an inventory path distinct from source, generated output and the source-controlled manifest.");
        string include = single["--include"];
        if (Path.IsPathRooted(include) || include.IndexOfAny(['\0', '\r', '\n', '"', '\\', ':']) >= 0 ||
            include.Split('/').Any(part => part.Length == 0 || part is "." or ".."))
            throw new MetadataException("unsafe or ambiguous --include path", "RMC1000", "Use a relative forward-slash header spelling without parent traversal, empty segments or quotes.");
        foreach (string includeDirectory in includes)
            if (string.IsNullOrWhiteSpace(includeDirectory) || !Directory.Exists(includeDirectory))
                throw new MetadataException("include directory does not exist: " + includeDirectory, "RMC1000", "Pass each real include directory in the native target's search order.");
        if (clang.Any(string.IsNullOrWhiteSpace)) throw new MetadataException("empty --clang-arg", "RMC1000", "Remove empty compiler arguments.");
        CompilerOptions options = new() { Input = single["--input"], Include = single["--include"], Output = single["--output"],
            Compiler = single.GetValueOrDefault("--compiler"), IdentityManifest = manifest,
            IdentityInventory = inventory, Check = check };
        options.IncludeDirectories.AddRange(includes);
        options.ClangArguments.AddRange(clang);
        return options;
    }
}
