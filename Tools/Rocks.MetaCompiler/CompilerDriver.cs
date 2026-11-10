using System.Text;
using System.Text.Json;
using System.Text.RegularExpressions;
using Rocks.Tooling.Common;

namespace Rocks.MetaCompiler;

internal static class CompilerDriver
{
    public static async Task RunAsync(CompilerOptions options)
    {
        string? repository = FindRoot();
        string root = repository ?? Directory.GetCurrentDirectory();
        string input = Path.GetFullPath(options.Input);
        if (!File.Exists(input)) throw new MetadataException($"input header not found: {input}", "RMC1000", "Pass an existing input header.");
        if (input.IndexOfAny(['\r', '\n', '"']) >= 0) throw new MetadataException("unsupported input path spelling", "RMC1000");
        Dictionary<string, string?> environment = await NativeEnvironment.CreateAsync(options.Compiler);
        string compiler = NativeEnvironment.FindExecutable(options.Compiler ?? (OperatingSystem.IsWindows() ? "clang-cl" : "clang++"), environment);
        bool clangCl = Path.GetFileNameWithoutExtension(compiler).Contains("clang-cl", StringComparison.OrdinalIgnoreCase);
        List<string> arguments = clangCl
            ? ["/nologo", "/TP", "/utf-8", "/clang:-std=c++23", "/W4", "/WX", "/GR-", "/EHs-c-", "/Zc:__cplusplus", "/clang:-fsyntax-only", "/clang:-Xclang", "/clang:-ast-dump=json"]
            : ["-x", "c++", "-std=c++23", "-Wall", "-Wextra", "-Wpedantic", "-Werror", "-fno-exceptions", "-fno-rtti", "-fsyntax-only", "-Xclang", "-ast-dump=json"];
        // 실제 빌드가 include 문맥을 제공하면 그 순서를 그대로 사용한다. 저장소 전체 헤더를 몰래
        // 추가하여 선언되지 않은 모듈 의존성을 통과시키지 않는다. 자동 탐색은 standalone fallback뿐이다.
        List<string> includes;
        if (options.IncludeDirectories.Count != 0) includes = options.IncludeDirectories.Select(Path.GetFullPath).ToList();
        else {
            IEnumerable<string> engineModules = repository is null ? [] : Directory.EnumerateDirectories(Path.Combine(repository, "Engine"));
            includes = engineModules.Select(path => Path.Combine(path, "Public")).Where(Directory.Exists)
                .Order(StringComparer.Ordinal).Append(root).Append(Path.GetDirectoryName(input)!).ToList();
        }
        ValidateCompilerArguments(options.ClangArguments);
        string outputDirectory = Path.GetDirectoryName(Path.GetFullPath(options.Output))!;
        ValidateIncludeBinding(input, options.Include, outputDirectory, includes);
        foreach (string path in includes)
        {
            if (clangCl) arguments.Add("/I" + path);
            else { arguments.Add("-I"); arguments.Add(path); }
        }
        arguments.AddRange(options.ClangArguments);
        arguments.Add("-");
        ProcessResult compiled = await ProcessRunner.RunAsync(new(compiler, arguments, root, environment,
            $"#include \"{input.Replace('\\', '/')}\"\n", TimeSpan.FromMinutes(2)));
        if (compiled.ExitCode != 0 || compiled.StdErr.Contains("warning:", StringComparison.OrdinalIgnoreCase))
            throw new MetadataException("Clang rejected reflection input:\n" + compiled.StdErr, "RMC3000",
                "Fix the Clang diagnostic at the reported source location; do not suppress or downgrade warnings.");
        SchemaModel schema;
        try { schema = new ClangAstParser(input).Parse(compiled.StdOut); }
        catch (Exception error) when (error is JsonException or KeyNotFoundException or InvalidOperationException or FormatException)
        {
            throw new MetadataException("unsupported Clang AST shape: " + error.Message, "RMC2001",
                "Use the supported Clang/toolchain and metadata subset; add a reviewed parser case before accepting a new AST shape.");
        }
        string generated = CppEmitter.Emit(schema, options.Include);
        // C++ 생성/소멸·noexcept·private friend·typed accessor의 실제 제약을 출력 게시 전에 검사한다.
        // AST의 defaultCtor 요약을 보고 생성 가능성을 추측하지 않는다.
        List<string> validationArguments = [.. arguments];
        validationArguments.Remove(clangCl ? "/clang:-Xclang" : "-Xclang");
        validationArguments.Remove(clangCl ? "/clang:-ast-dump=json" : "-ast-dump=json");
        // 생성 헤더의 quoted include는 헤더가 놓일 디렉터리부터 검색된다. 같은 위치의 임시
        // translation unit으로 검증하여 stdin/cwd와 실제 소비자의 검색 순서가 달라지지 않게 한다.
        Directory.CreateDirectory(outputDirectory);
        string temporarySource = Path.Combine(outputDirectory, ".rocks-meta-validation-" + Guid.NewGuid().ToString("N") + ".cpp");
        ProcessResult validated;
        try
        {
            File.WriteAllText(temporarySource, generated.Replace("#pragma once\n", "", StringComparison.Ordinal), new UTF8Encoding(false, true));
            validationArguments[^1] = temporarySource;
            validated = await ProcessRunner.RunAsync(new(compiler, validationArguments, root, environment,
                Timeout: TimeSpan.FromMinutes(2)));
        }
        finally
        {
            if (File.Exists(temporarySource)) File.Delete(temporarySource);
        }
        if (validated.ExitCode != 0 || validated.StdErr.Contains("warning:", StringComparison.OrdinalIgnoreCase))
            throw new MetadataException("generated metadata contract rejected by Clang:\n" + validated.StdErr + SourceNotes(schema, validated.StdErr), "RMC3000",
                "Fix constructor/destructor noexcept, CLASS/PROPERTY access and GENERATED_BODY friendship; every generated typed operation must compile before output is accepted.");
        string formatter = NativeEnvironment.FindExecutable(Environment.GetEnvironmentVariable("ROCKS_CLANG_FORMAT") ?? "clang-format", environment);
        ProcessResult version = await ProcessRunner.RunAsync(new(formatter, ["--version"], root, environment, Timeout: TimeSpan.FromSeconds(20)));
        if (version.ExitCode != 0 || !version.StdOut.Contains("version 21.1.6", StringComparison.Ordinal))
            throw new MetadataException("Rocks requires clang-format 21.1.6", "RMC3100", "Set ROCKS_CLANG_FORMAT to the pinned formatter executable.");
        ProcessResult formatted = await ProcessRunner.RunAsync(new(formatter,
            ["--style=file:" + Path.Combine(AppContext.BaseDirectory, ".clang-format"), "--assume-filename=Metadata.h"], root, environment, generated, TimeSpan.FromSeconds(30)));
        if (formatted.ExitCode != 0 || formatted.StdErr.Contains("warning:", StringComparison.OrdinalIgnoreCase))
            throw new MetadataException("clang-format failed: " + formatted.StdErr, "RMC3100", "Fix the formatter/style error without suppressing diagnostics.");
        byte[] bytes = new UTF8Encoding(false, true).GetBytes(formatted.StdOut.Replace("\r\n", "\n", StringComparison.Ordinal));
        if (options.Check)
        {
            if (!File.Exists(options.Output) || !File.ReadAllBytes(options.Output).AsSpan().SequenceEqual(bytes))
                throw new MetadataException($"generated output is stale: {options.Output}", "RMC4000", "Regenerate with the same command without --check, then rebuild.");
        }
        else AtomicFile.WriteIfChanged(options.Output, bytes);
        Console.WriteLine($"Rocks metadata: {schema.Ordered.Count} types; {(options.Check ? "checked" : "generated")}");
    }

    private static void ValidateCompilerArguments(IEnumerable<string> arguments)
    {
        foreach (string argument in arguments)
        {
            string native = argument.StartsWith("/clang:", StringComparison.Ordinal) ? argument[7..] : argument;
            bool reduced = native is "-Xclang" or "-Xpreprocessor" || native == "-w" || native.StartsWith("-Wno-", StringComparison.Ordinal) ||
                native.StartsWith('@') || native.Equals("/WX-", StringComparison.OrdinalIgnoreCase) ||
                native is "/W0" or "/W1" or "/W2" or "/W3" or "/w" ||
                native.StartsWith("/wd", StringComparison.OrdinalIgnoreCase) ||
                native.StartsWith("/external:W", StringComparison.OrdinalIgnoreCase) ||
                native.Contains("_CRT_SECURE_NO_WARNINGS", StringComparison.Ordinal) ||
                native.Contains("_SCL_SECURE_NO_WARNINGS", StringComparison.Ordinal);
            // 검색 순서와 forced include는 --include-dir에서만 선언한다. 임의 compiler 인자로
            // 별도 검색 경로를 추가하면 원본 검증과 실제 generated header가 다른 파일을 볼 수 있다.
            bool searchOverride = native.StartsWith("-I", StringComparison.Ordinal) ||
                native.StartsWith("/I", StringComparison.OrdinalIgnoreCase) ||
                native.StartsWith("/FI", StringComparison.OrdinalIgnoreCase) ||
                native.StartsWith("/external:I", StringComparison.OrdinalIgnoreCase) ||
                native.StartsWith("-iquote", StringComparison.Ordinal) ||
                native.StartsWith("-isystem", StringComparison.Ordinal) ||
                native.StartsWith("-idirafter", StringComparison.Ordinal) ||
                native.StartsWith("-include", StringComparison.Ordinal) ||
                native.StartsWith("-imacros", StringComparison.Ordinal) ||
                native.StartsWith("-F", StringComparison.Ordinal) ||
                native.StartsWith("-iframework", StringComparison.Ordinal) ||
                native.StartsWith("-ivfsoverlay", StringComparison.Ordinal) ||
                native.StartsWith("-fmodule-map-file", StringComparison.Ordinal) ||
                native.StartsWith("-Wp,", StringComparison.Ordinal) ||
                native.StartsWith("--config", StringComparison.Ordinal);
            if (reduced || searchOverride) throw new MetadataException("unsupported compiler diagnostics/include/response-file override: " + argument,
                "RMC1000", "Remove warning suppression, forced includes and hidden search paths; supply explicit --include-dir entries in target order.");
        }
    }

    private static void ValidateIncludeBinding(string input, string include, string outputDirectory, IEnumerable<string> directories)
    {
        string? target = directories.Prepend(outputDirectory).Select(directory => Path.GetFullPath(Path.Combine(directory, include)))
            .FirstOrDefault(File.Exists);
        string canonicalInput = CanonicalPath.Resolve(input);
        string? canonicalTarget = target is null ? null : CanonicalPath.Resolve(target);
        if (canonicalTarget is null || !string.Equals(canonicalInput, canonicalTarget,
                OperatingSystem.IsWindows() ? StringComparison.OrdinalIgnoreCase : StringComparison.Ordinal))
            throw new MetadataException($"--include resolves to a different header than --input; input={input}; resolved input={canonicalInput}; " +
                $"include target={target ?? "<not found>"}; resolved target={canonicalTarget ?? "<not found>"}", "RMC1000",
                "Pass the same author header through the target's explicit include search order; remove shadowing include directories.");
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
            if (!qualified && !Token(shortName)) continue;
            string description = qualified ? "diagnostic references CLASS" : "candidate CLASS";
            notes.AppendLine($"{type.Site.File}:{type.Site.Line}: note: {description} {type.Name}");
            foreach (FieldModel field in type.Fields)
                if (Token(field.Name))
                    notes.AppendLine($"{field.Site.File}:{field.Site.Line}: note: candidate PROPERTY {field.Owner}::{field.Name}");
        }
        return notes.ToString();
    }
    private static string? FindRoot()
    {
        for (DirectoryInfo? directory = new(AppContext.BaseDirectory); directory is not null; directory = directory.Parent)
            if (Directory.Exists(Path.Combine(directory.FullName, "Engine")) && Directory.Exists(Path.Combine(directory.FullName, "Tools")))
                return directory.FullName;
        return null; // SDK 배포에서는 명시 include 경로와 함께 저장소 밖에서도 실행할 수 있다.
    }
}
