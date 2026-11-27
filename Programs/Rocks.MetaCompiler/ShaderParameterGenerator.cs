using System.Globalization;
using System.Text;
using System.Text.Json;
using System.Text.RegularExpressions;
using Rocks.Tooling.Common;

namespace Rocks.MetaCompiler;

// 셰이더 파라미터 구조체 생성 모드다(Docs/Design/ShaderSystem 16.2절 SSD49–SSD53, 13절 P7). 리플렉션 메타 단계와 따로 돈다:
// `Rocks.MetaCompiler shader-parameters --module M --input <헤더> --include <철자> ... --output-root <디렉터리>`.
// 모듈 헤더의 ROCKS_SHADER_PARAMETERS 구조체마다 Clang이 정한 C++ 오프셋을 읽어 7.2절 5항 규칙으로 검사하고, 생성 C++ 표
// (<출력>/Include/<철자의 .h를 .shaderparams.h로>)와 생성 HLSL(<출력>/Shaders/<C++ 이름>.rsh)을 쓴다. 자원 멤버(RenderBase·RenderGraph 틀 +
// ShaderCore 서술자, 16.2절 SSD59–SSD62)는 상수 부분 뒤에 와서 표의 자원 목록과 HLSL 전역 선언이 된다. 오류는 RMC6xxx다.
internal static class ShaderParameterGenerator
{
    // 표지 annotation 이름이다(Meta/Annotations.h ROCKS_SHADER_PARAMETERS).
    private const string MarkerAnnotation = "rocks.shaderparams.marker";

    // ShaderCore 한도다(ShaderCore/ShaderParameterLayout.h와 같다).
    private const int MaxMembers = 256;
    private const int MaxResources = 256;
    private const int MaxNameBytes = 64;
    private const int MaxConstantBufferBytes = 65536;
    private const int MaxRootConstantBytes = 128;
    private const int SlotBytes = 16;
    private const int MaxAlignment = 16;

    // 생성 padding 멤버 이름의 접두다. 사용자 멤버 이름은 이 접두로 시작할 수 없다.
    private const string PaddingPrefix = "rocksPadding";

    // 생성 C++ 헤더 철자의 접미다.
    public const string GeneratedHeaderSuffix = ".shaderparams.h";

    // 생성 C++ 헤더·HLSL이 놓이는 출력 루트 아래 디렉터리 이름이다.
    public const string IncludeDirectoryName = "Include";
    public const string ShadersDirectoryName = "Shaders";

    private static readonly Regex Identifier = new("^[A-Za-z_][A-Za-z0-9_]*$", RegexOptions.CultureInvariant);

    // 생성 HLSL이 쓸 수 없는 예약 전역 이름이다(공유 샘플러와 그 짝, PS 진단 버퍼 — 7.2절 9·12항).
    private static readonly HashSet<string> ReservedNames =
        new(["SharedWrap", "SharedClamp", "SharedWrapSampler", "SharedClampSampler", "RocksDebugBuffer"], StringComparer.Ordinal);

    // 자원 멤버 틀의 범주다(16.2절 SSD59).
    private enum Category { TextureSrv, BufferSrv, TextureUav, BufferUav, Access, RenderTargets }

    // 자원 틀(C++ 정규 철자, 템플릿 이름) → ShaderParameterResourceKind 열거자와 범주다.
    private static readonly Dictionary<string, (string Kind, Category Category)> Holders = new(StringComparer.Ordinal)
    {
        ["rocks::render::DeviceTextureParameter"] = ("DeviceTexture", Category.TextureSrv),
        ["rocks::render::DeviceBufferParameter"] = ("DeviceBuffer", Category.BufferSrv),
        ["rocks::render::GraphTextureParameter"] = ("GraphTexture", Category.TextureSrv),
        ["rocks::render::GraphTextureSrvParameter"] = ("GraphTextureSrv", Category.TextureSrv),
        ["rocks::render::GraphTextureUavParameter"] = ("GraphTextureUav", Category.TextureUav),
        ["rocks::render::GraphBufferSrvParameter"] = ("GraphBufferSrv", Category.BufferSrv),
        ["rocks::render::GraphBufferUavParameter"] = ("GraphBufferUav", Category.BufferUav),
        ["rocks::render::GraphTextureAccessParameter"] = ("GraphTextureAccess", Category.Access),
        ["rocks::render::GraphBufferAccessParameter"] = ("GraphBufferAccess", Category.Access),
        ["rocks::render::GraphRenderTargetParameter"] = ("RenderTargets", Category.RenderTargets),
    };

    // HLSL 자원 서술자다(ShaderCore/ShaderResourceTypes.h): 범주, ShaderResourceShape 열거자, HLSL 타입, image 형식 인자, 짝 샘플러 인자.
    private sealed record Descriptor(Category Category, string Shape, string Hlsl, bool Format, bool Sampler);

    private static readonly Dictionary<string, Descriptor> Descriptors = new(StringComparer.Ordinal)
    {
        ["rocks::shader::ShaderTexture1D"] = new(Category.TextureSrv, "Texture1D", "Texture1D", false, true),
        ["rocks::shader::ShaderTexture1DArray"] = new(Category.TextureSrv, "Texture1DArray", "Texture1DArray", false, true),
        ["rocks::shader::ShaderTexture2D"] = new(Category.TextureSrv, "Texture2D", "Texture2D", false, true),
        ["rocks::shader::ShaderTexture2DArray"] = new(Category.TextureSrv, "Texture2DArray", "Texture2DArray", false, true),
        ["rocks::shader::ShaderTexture3D"] = new(Category.TextureSrv, "Texture3D", "Texture3D", false, true),
        ["rocks::shader::ShaderTextureCube"] = new(Category.TextureSrv, "TextureCube", "TextureCube", false, true),
        ["rocks::shader::ShaderTextureCubeArray"] = new(Category.TextureSrv, "TextureCubeArray", "TextureCubeArray", false, true),
        ["rocks::shader::ShaderTexture2DMS"] = new(Category.TextureSrv, "Texture2DMultisample", "Texture2DMS", false, false),
        ["rocks::shader::ShaderTexture2DMSArray"] = new(Category.TextureSrv, "Texture2DMultisampleArray", "Texture2DMSArray", false, false),
        ["rocks::shader::ShaderBuffer"] = new(Category.BufferSrv, "TypedBuffer", "Buffer", false, false),
        ["rocks::shader::ShaderStructuredBuffer"] = new(Category.BufferSrv, "StructuredBuffer", "StructuredBuffer", false, false),
        ["rocks::shader::ShaderByteAddressBuffer"] = new(Category.BufferSrv, "RawBuffer", "ByteAddressBuffer", false, false),
        ["rocks::shader::ShaderRWTexture2D"] = new(Category.TextureUav, "Texture2D", "RWTexture2D", true, false),
        ["rocks::shader::ShaderRWTexture2DArray"] = new(Category.TextureUav, "Texture2DArray", "RWTexture2DArray", true, false),
        ["rocks::shader::ShaderRWTexture3D"] = new(Category.TextureUav, "Texture3D", "RWTexture3D", true, false),
        ["rocks::shader::ShaderRWBuffer"] = new(Category.BufferUav, "TypedBuffer", "RWBuffer", true, false),
        ["rocks::shader::ShaderRWStructuredBuffer"] = new(Category.BufferUav, "StructuredBuffer", "RWStructuredBuffer", false, false),
        ["rocks::shader::ShaderRWByteAddressBuffer"] = new(Category.BufferUav, "RawBuffer", "RWByteAddressBuffer", false, false),
    };

    // SRV 텍스처 서술자의 기본 원소다(ShaderCore/ShaderResourceTypes.h).
    private const string DefaultTextureElement = "rocks::shader::ShaderFloat4";

    // 비교 짝 샘플러 태그다(ShaderFilterSampler가 기본).
    private const string ComparisonSamplerTag = "rocks::shader::ShaderComparisonSampler";

    // UAV image 형식 태그 → ROCKS_IMAGE_FORMAT 철자·기본 타입·채널 수다(7.2절 1항).
    private static readonly Dictionary<string, (string Spelling, Base Base, int Channels)> ImageFormats = new(StringComparer.Ordinal)
    {
        ["rocks::shader::ShaderImageRgba8"] = ("rgba8", Base.Float, 4),
        ["rocks::shader::ShaderImageRgba8Snorm"] = ("rgba8snorm", Base.Float, 4),
        ["rocks::shader::ShaderImageRgba16f"] = ("rgba16f", Base.Float, 4),
        ["rocks::shader::ShaderImageRgba32f"] = ("rgba32f", Base.Float, 4),
        ["rocks::shader::ShaderImageR11g11b10f"] = ("r11g11b10f", Base.Float, 3),
        ["rocks::shader::ShaderImageR16f"] = ("r16f", Base.Float, 1),
        ["rocks::shader::ShaderImageRg16f"] = ("rg16f", Base.Float, 2),
        ["rocks::shader::ShaderImageR32f"] = ("r32f", Base.Float, 1),
        ["rocks::shader::ShaderImageRg32f"] = ("rg32f", Base.Float, 2),
        ["rocks::shader::ShaderImageR32ui"] = ("r32ui", Base.Uint, 1),
        ["rocks::shader::ShaderImageR32i"] = ("r32i", Base.Int, 1),
        ["rocks::shader::ShaderImageRgba32ui"] = ("rgba32ui", Base.Uint, 4),
    };

    // 표지 뒤 구조체 이름(어휘)이다. 헤더마다 어느 구조체가 그 헤더에 있는지 정한다(AST 위치는 파일이 바뀔 때만 실려 있다).
    private static readonly Regex DeclaredStruct = new(
        @"^[ \t]*ROCKS_SHADER_PARAMETERS[ \t]*\((?:[^()]|\([^()]*\))*\)\s*(?:struct|class)\s+(?:alignas\s*\(\s*\d+\s*\)\s*)?(?<name>[A-Za-z_][A-Za-z0-9_]*)",
        RegexOptions.CultureInvariant | RegexOptions.Multiline, TimeSpan.FromSeconds(10));

    // 멤버의 기본 타입이다(GLES 에뮬레이션 배열 h·i·u — 7.2절 4항).
    private enum Base { Float, Int, Uint, Struct }

    private sealed record ScalarType(string Cpp, string Hlsl, string Enum, int Bytes, int Alignment, Base Base);

    // C++ 정규 철자 → 셰이더 타입이다(ShaderParameterType 순서). 3성분 C++ 크기는 16이고 HLSL 크기는 12다.
    private static readonly ScalarType[] Types =
    [
        new("float", "float", "Float", 4, 4, Base.Float),
        new("rocks::shader::ShaderFloat2", "float2", "Float2", 8, 8, Base.Float),
        new("rocks::shader::ShaderFloat3", "float3", "Float3", 12, 16, Base.Float),
        new("rocks::shader::ShaderFloat4", "float4", "Float4", 16, 16, Base.Float),
        new("int", "int", "Int", 4, 4, Base.Int),
        new("rocks::shader::ShaderInt2", "int2", "Int2", 8, 8, Base.Int),
        new("rocks::shader::ShaderInt3", "int3", "Int3", 12, 16, Base.Int),
        new("rocks::shader::ShaderInt4", "int4", "Int4", 16, 16, Base.Int),
        new("unsigned int", "uint", "Uint", 4, 4, Base.Uint),
        new("rocks::shader::ShaderUint2", "uint2", "Uint2", 8, 8, Base.Uint),
        new("rocks::shader::ShaderUint3", "uint3", "Uint3", 12, 16, Base.Uint),
        new("rocks::shader::ShaderUint4", "uint4", "Uint4", 16, 16, Base.Uint),
        new("rocks::shader::ShaderFloat4x4", "float4x4", "Float4x4", 64, 16, Base.Float),
    ];

    private sealed class Options
    {
        public string Module = "";
        public readonly List<ModuleInput> Inputs = [];
        public string OutputRoot = "";
        public string? Compiler;
        public string VirtualRoot = "/Engine";
        public string? DefinitionsHeader;
        public readonly List<string> IncludeDirectories = [];
        public readonly List<string> ClangArguments = [];
        // 의존 기록·단계 보고 출력 경로다(Docs/Design/BuildReuse 2.4·2.8절 — 리플렉션 모드와 같은 형식).
        public string? DependencyRecord;
        public string? PhaseReport;
    }

    // 선언 하나다.
    private sealed class StructDecl
    {
        public required string Qualified { get; init; }

        public required string Simple { get; init; }

        public required string Specifiers { get; init; }

        public required List<string> Fields { get; init; }

        public ModuleInput? Input { get; set; }

        public string Name { get; set; } = "";

        public bool RootConstants { get; set; }

        // 표의 totalBytes(상수 부분 byte)다.
        public int Size { get; set; }

        // C++ sizeof다(자원 멤버가 있으면 Size보다 크다).
        public int CppSize { get; set; }

        public int Alignment { get; set; }

        public List<Member> Members { get; } = [];

        // 자원 멤버(포함 구조체는 펼친 것, 오프셋 순)다.
        public List<ResourceEntry> Resources { get; } = [];

        // 포함한 자원 구조체 멤버(이름, 오프셋, 구조체)다.
        public List<(string Name, int Offset, StructDecl Struct)> Includes { get; } = [];
    }

    // 자원 멤버 하나다(16.2절 SSD62 — ShaderParameterResource 한 줄). Kind·Shape·Component·Sampler는 C++ 열거자 이름이다.
    private sealed class ResourceEntry
    {
        public required string Name { get; init; }

        public required string Kind { get; init; }

        public required int Offset { get; init; }

        public string Shape { get; init; } = "None";

        public string Component { get; init; } = "None";

        public int Stride { get; init; }

        public string Sampler { get; init; } = "None";

        // HLSL 선언의 타입 부분("Texture2D<float4>", "ROCKS_IMAGE_FORMAT(r32f) RWTexture2D<float>")이다. 셰이더에 묶지 않는 멤버는 null이다.
        public string? Declaration { get; init; }

        // 바깥 C++ 멤버 이름이다(static_assert). 포함 구조체에서 펼친 자원은 null이다.
        public string? Member { get; init; }

        public string SamplerName => Sampler == "None" ? "" : Name + "Sampler";
    }

    // 표 멤버 하나다(padding 포함).
    private sealed class Member
    {
        public required string Name { get; init; }

        public required int Offset { get; init; }

        public ScalarType? Scalar { get; init; }

        public StructDecl? Struct { get; init; }

        public int ArrayCount { get; init; }

        public bool Padding { get; init; }

        public Base Base => Struct is null ? Scalar!.Base : Base.Struct;

        public int HlslBytes => Struct is not null ? Struct.Size : ArrayCount == 0 ? Scalar!.Bytes : Scalar!.Bytes * ArrayCount;
    }

    public static async Task<int> RunAsync(string[] arguments)
    {
        Options options = Parse(arguments);
        MetaRunRecorder recorder = MetaRunRecorder.Begin();
        try
        {
            int exit = await GenerateAsync(options, recorder);
            if (options.DependencyRecord is not null)
            {
                recorder.WriteDependencyRecord(options.DependencyRecord);
            }

            return exit;
        }
        finally
        {
            if (options.PhaseReport is not null)
            {
                recorder.WritePhaseReport(options.PhaseReport);
            }
        }
    }

    private static async Task<int> GenerateAsync(Options options, MetaRunRecorder recorder)
    {
        recorder.Mark("environment");
        // 자기 출력은 입력이 아니다(Docs/Design/BuildReuse 2.4절 제외 규칙).
        recorder.Exclude(options.OutputRoot);
        string root = CompilerDriver.FindRoot() ?? Directory.GetCurrentDirectory();
        Dictionary<string, string?> environment = await NativeEnvironment.CreateAsync(options.Compiler);
        string compiler = NativeEnvironment.FindExecutable(options.Compiler ?? (OperatingSystem.IsWindows() ? "clang-cl" : "clang++"), environment);
        bool clangCl = Path.GetFileNameWithoutExtension(compiler).Contains("clang-cl", StringComparison.OrdinalIgnoreCase);
        await CompilerDriver.ValidateCompilerArgumentsAsync(options.ClangArguments, clangCl, root, environment);
        foreach (ModuleInput input in options.Inputs)
        {
            CompilerDriver.ValidateIncludeBinding(input.Path, input.Include, options.IncludeDirectories.Select(Path.GetFullPath));
        }

        List<string> baseArguments = clangCl
            ? ["/nologo", "/TP", "/utf-8", "/clang:-std=c++23", "/W4", "/WX", "/GR-", "/EHs-c-", "/Zc:__cplusplus", "/clang:-fsyntax-only"]
            : ["-x", "c++", "-std=c++23", "-Wall", "-Wextra", "-Wpedantic", "-Werror", "-fno-exceptions", "-fno-rtti", "-fsyntax-only"];
        foreach (string directory in options.IncludeDirectories.Select(Path.GetFullPath))
        {
            if (clangCl)
            {
                baseArguments.Add("/I" + directory);
            }
            else
            {
                baseArguments.AddRange(["-I", directory]);
            }
        }

        baseArguments.AddRange(options.ClangArguments);
        baseArguments.AddRange(CompilerDriver.TargetMacroArguments(clangCl, editor: true));
        if (options.DefinitionsHeader is not null)
        {
            baseArguments.AddRange(clangCl ? ["/FI" + options.DefinitionsHeader] : ["-include", options.DefinitionsHeader]);
        }

        string unit = string.Concat(options.Inputs.Select(input => $"#include \"{input.Path.Replace('\\', '/')}\"\n"));
        recorder.Mark("ast");
        List<string> astArguments = [.. baseArguments, .. clangCl ? new[] { "/clang:-Xclang", "/clang:-ast-dump=json" } : ["-Xclang", "-ast-dump=json"], "-"];
        ProcessResult ast = await ClangInvocation.RunAsync(new(compiler, astArguments, root, environment, CompilerDriver.ValidateGeneratedCode(unit), TimeSpan.FromMinutes(2)), clangCl);
        if (ast.ExitCode != 0 || ast.StdErr.Contains("warning:", StringComparison.OrdinalIgnoreCase))
        {
            throw new MetadataException("Clang rejected shader parameter input:\n" + ast.StdErr, "RMC6000",
                "Fix the Clang diagnostic at the reported source location; do not suppress warnings.");
        }

        List<StructDecl> declared = FindDeclarations(ast.StdOut);
        BindInputs(declared, options);
        List<StructDecl> owned = [.. declared.Where(declaration => declaration.Input is not null)];
        if (owned.Count == 0)
        {
            throw new MetadataException($"module {options.Module}: no ROCKS_SHADER_PARAMETERS struct was found in the inputs", "RMC6001",
                "Pass only headers that declare shader parameter structs.");
        }

        recorder.Mark("probes");
        await ProbeAsync(owned, compiler, baseArguments, unit, root, environment, clangCl);
        recorder.Mark("write");
        foreach (StructDecl declaration in OrderNestedFirst(owned))
        {
            Layout(declaration, owned);
        }

        CheckModuleNames(owned, options.Module);
        Write(owned, options);
        Console.WriteLine($"Rocks shader parameters: module {options.Module}: {owned.Count} structs; generated");
        return 0;
    }

    private static Options Parse(string[] arguments)
    {
        var options = new Options();
        var privateIncludes = new HashSet<string>(StringComparer.Ordinal);
        string? pendingInput = null;
        for (int index = 0; index < arguments.Length; ++index)
        {
            string argument = arguments[index];
            if (argument.StartsWith("--clang-arg=", StringComparison.Ordinal))
            {
                options.ClangArguments.Add(argument["--clang-arg=".Length..]);
                continue;
            }

            if (index + 1 >= arguments.Length)
            {
                throw new MetadataException("missing value for " + argument, "RMC1000", "Pass every option with its value.");
            }

            string value = arguments[++index];
            switch (argument)
            {
                case "--module":
                    options.Module = value;
                    break;
                case "--input":
                    pendingInput = Path.GetFullPath(value);
                    break;
                case "--include":
                    if (pendingInput is null)
                    {
                        throw new MetadataException("--include must follow --input", "RMC1000", "Pass --input <header> --include <spelling> pairs.");
                    }

                    options.Inputs.Add(new(pendingInput, value.Replace('\\', '/'), false));
                    pendingInput = null;
                    break;
                case "--private":
                    privateIncludes.Add(value.Replace('\\', '/'));
                    break;
                case "--output-root":
                    options.OutputRoot = Path.GetFullPath(value);
                    break;
                case "--compiler":
                    options.Compiler = value;
                    break;
                case "--virtual-root":
                    options.VirtualRoot = value;
                    break;
                case "--definitions-header":
                    options.DefinitionsHeader = Path.GetFullPath(value);
                    break;
                case "--include-dir":
                    options.IncludeDirectories.Add(value);
                    break;
                case "--dependency-record":
                    options.DependencyRecord = Path.GetFullPath(value);
                    break;
                case "--phase-report":
                    options.PhaseReport = Path.GetFullPath(value);
                    break;
                default:
                    throw new MetadataException("unknown shader-parameters option: " + argument, "RMC1000",
                        "Use --module, --input/--include, --private, --output-root, --compiler, --virtual-root, --definitions-header, --include-dir, " +
                        "--dependency-record, --phase-report and --clang-arg=.");
            }
        }

        if (options.Module.Length == 0 || options.Inputs.Count == 0 || options.OutputRoot.Length == 0 || pendingInput is not null)
        {
            throw new MetadataException("shader-parameters needs --module, --output-root and --input/--include pairs", "RMC1000",
                "Pass the module name, the output root and every input header with its include spelling.");
        }

        if (!Identifier.IsMatch(options.Module))
        {
            throw new MetadataException("module name must be an identifier: " + options.Module, "RMC1000", "Pass the module name from its rules.");
        }

        if (options.VirtualRoot is not ("/Engine" or "/Project") && !Regex.IsMatch(options.VirtualRoot, "^/Plugin/[A-Za-z0-9_]+$", RegexOptions.CultureInvariant))
        {
            throw new MetadataException("virtual root must be /Engine, /Project or /Plugin/<name>: " + options.VirtualRoot, "RMC1000",
                "Pass the shader virtual root of the module owner.");
        }

        for (int index = 0; index < options.Inputs.Count; ++index)
        {
            ModuleInput input = options.Inputs[index];
            if (!File.Exists(input.Path) || !input.Include.EndsWith(".h", StringComparison.Ordinal))
            {
                throw new MetadataException($"input header not found or not a .h spelling: {input.Path}", "RMC1000", "Pass existing .h headers.");
            }

            options.Inputs[index] = input with { Private = privateIncludes.Contains(input.Include) };
        }

        return options;
    }

    private static string Text(JsonElement node, string property) =>
        node.ValueKind == JsonValueKind.Object && node.TryGetProperty(property, out JsonElement value) && value.ValueKind == JsonValueKind.String
            ? value.GetString() ?? "" : "";

    private static IEnumerable<JsonElement> Children(JsonElement node) =>
        node.ValueKind == JsonValueKind.Object && node.TryGetProperty("inner", out JsonElement inner) && inner.ValueKind == JsonValueKind.Array
            ? inner.EnumerateArray() : [];

    private static bool Flag(JsonElement node, string property) =>
        node.TryGetProperty(property, out JsonElement value) && value.ValueKind == JsonValueKind.True;

    // 표지 enum의 지정자 문자열이다. 표지가 아니면 null이다.
    private static string? MarkerSpecifiers(JsonElement node)
    {
        if (Text(node, "kind") != "EnumDecl" || Text(node, "name").Length != 0)
        {
            return null;
        }

        foreach (JsonElement constant in Children(node))
        {
            foreach (JsonElement attribute in Children(constant))
            {
                if (Text(attribute, "kind") != "AnnotateAttr")
                {
                    continue;
                }

                string?[] values = [.. Children(attribute).Select(Literal)];
                if (values.Length >= 2 && values[0] == MarkerAnnotation)
                {
                    return values[1] ?? "";
                }
            }
        }

        return null;
    }

    private static string? Literal(JsonElement node)
    {
        string kind = Text(node, "kind");
        if (kind == "StringLiteral")
        {
            return ClangLiteral.Decode(Text(node, "value"));
        }

        JsonElement[] children = [.. Children(node)];
        return kind is "ConstantExpr" or "ImplicitCastExpr" or "ParenExpr" && children.Length == 1 ? Literal(children[0]) : null;
    }

    private static List<StructDecl> FindDeclarations(string json)
    {
        var found = new List<StructDecl>();
        using JsonDocument document = JsonDocument.Parse(json, new JsonDocumentOptions { MaxDepth = 4096 });
        Walk(document.RootElement, "", found);
        return found;
    }

    private static void Walk(JsonElement node, string scope, List<StructDecl> found)
    {
        JsonElement[] children = [.. Children(node)];
        for (int index = 0; index < children.Length; ++index)
        {
            JsonElement child = children[index];
            string kind = Text(child, "kind");
            if (kind == "NamespaceDecl")
            {
                string name = Text(child, "name");
                Walk(child, name.Length == 0 ? scope + "(anonymous)::" : scope + name + "::", found);
                continue;
            }

            if (kind == "LinkageSpecDecl")
            {
                Walk(child, scope, found);
                continue;
            }

            if (MarkerSpecifiers(child) is not string specifiers)
            {
                continue;
            }

            JsonElement? target = index + 1 < children.Length ? children[index + 1] : null;
            if (target is not JsonElement record || Text(record, "kind") != "CXXRecordDecl" || !Flag(record, "completeDefinition") ||
                Text(record, "name").Length == 0)
            {
                string what = target is JsonElement next && Text(next, "kind") == "ClassTemplateDecl" ? "a template" : "not a complete named struct";
                throw new MetadataException($"ROCKS_SHADER_PARAMETERS in scope '{scope}' is followed by {what}", "RMC6002",
                    "Put ROCKS_SHADER_PARAMETERS(...) directly before a non-template struct definition at namespace scope.");
            }

            string qualified = scope + Text(record, "name");
            if (Text(node, "kind") == "CXXRecordDecl" || scope.Contains("(anonymous)", StringComparison.Ordinal))
            {
                throw new MetadataException($"{qualified}: shader parameter structs must be at named namespace scope", "RMC6002",
                    "Move the struct out of the class or anonymous namespace.");
            }

            if (record.TryGetProperty("bases", out JsonElement bases) && bases.ValueKind == JsonValueKind.Array && bases.GetArrayLength() != 0)
            {
                throw new MetadataException($"{qualified}: shader parameter structs cannot have bases", "RMC6002",
                    "Declare every member directly in the struct.");
            }

            var fields = new List<string>();
            foreach (JsonElement member in Children(record))
            {
                string memberKind = Text(member, "kind");
                // 정적 데이터 멤버(기본값 상수 등)와 구조체 속성(alignas 등)은 배치에 들지 않는다.
                if (Flag(member, "isImplicit") || memberKind is "AccessSpecDecl" or "VarDecl" || memberKind.EndsWith("Attr", StringComparison.Ordinal))
                {
                    continue;
                }

                if (memberKind == "FieldDecl")
                {
                    string field = Text(member, "name");
                    if (field.Length == 0 || Flag(member, "isBitfield"))
                    {
                        throw new MetadataException($"{qualified}: anonymous and bitfield members are not shader parameters", "RMC6003",
                            "Use named members of the shader aligned types.");
                    }

                    fields.Add(field);
                    continue;
                }

                if (memberKind is "CXXRecordDecl" && Text(member, "name").Length == 0)
                {
                    throw new MetadataException($"{qualified}: anonymous unions and structs are not shader parameters", "RMC6003",
                        "Use named members of the shader aligned types.");
                }

                throw new MetadataException($"{qualified}: shader parameter structs hold data members only (found {memberKind})", "RMC6003",
                    "Remove functions and nested types; shader parameter structs are plain data (static constants are allowed).");
            }

            found.Add(new StructDecl { Qualified = qualified, Simple = Text(record, "name"), Specifiers = specifiers, Fields = fields });
            ++index;
        }
    }

    // 헤더마다 어휘로 찾은 구조체 이름에 AST 선언을 잇는다(이 모듈 입력에 있는 것만 생성한다).
    private static void BindInputs(List<StructDecl> declared, Options options)
    {
        foreach (ModuleInput input in options.Inputs)
        {
            string text = CppSourceText.WithoutCommentsAndLiterals(File.ReadAllText(input.Path));
            foreach (Match match in DeclaredStruct.Matches(text))
            {
                string name = match.Groups["name"].Value;
                StructDecl[] candidates = [.. declared.Where(declaration => declaration.Simple == name)];
                if (candidates.Length != 1)
                {
                    throw new MetadataException($"{input.Include}: shader parameter struct name '{name}' must be unique in the module's headers",
                        "RMC6004", "Give every shader parameter struct a distinct name (the HLSL struct name is the C++ name).");
                }

                if (candidates[0].Input is not null)
                {
                    throw new MetadataException($"{input.Include}: shader parameter struct '{name}' is declared twice", "RMC6004",
                        "Declare each shader parameter struct once.");
                }

                candidates[0].Input = input;
            }
        }
    }

    // 지정자: Name = "…", RootConstants.
    private static void ApplySpecifiers(StructDecl declaration)
    {
        declaration.Name = declaration.Simple;
        foreach (string raw in SplitSpecifiers(declaration.Specifiers))
        {
            string item = raw.Trim();
            if (item.Length == 0)
            {
                continue;
            }

            if (item == "RootConstants")
            {
                declaration.RootConstants = true;
                continue;
            }

            Match name = Regex.Match(item, "^Name\\s*=\\s*\"(?<value>[^\"]*)\"$", RegexOptions.CultureInvariant);
            if (name.Success)
            {
                declaration.Name = name.Groups["value"].Value;
                continue;
            }

            throw new MetadataException($"{declaration.Qualified}: unknown ROCKS_SHADER_PARAMETERS specifier '{item}'", "RMC6005",
                "Use Name = \"...\" and RootConstants only.");
        }

        if (!Identifier.IsMatch(declaration.Name) || Encoding.UTF8.GetByteCount(declaration.Name) > MaxNameBytes)
        {
            throw new MetadataException($"{declaration.Qualified}: shader parameter name '{declaration.Name}' must be an identifier of at most 64 bytes",
                "RMC6005", "Use an HLSL identifier for Name.");
        }

        if (declaration.RootConstants && declaration.Name == declaration.Simple)
        {
            throw new MetadataException($"{declaration.Qualified}: root constants need a Name different from the struct name (HLSL declares both)",
                "RMC6005", "Give the root constants block its shader name, e.g. ROCKS_SHADER_PARAMETERS(Name = \"Draw\", RootConstants).");
        }
    }

    private static IEnumerable<string> SplitSpecifiers(string text)
    {
        var current = new StringBuilder();
        bool quoted = false;
        foreach (char character in text)
        {
            if (character == '"')
            {
                quoted = !quoted;
            }

            if (character == ',' && !quoted)
            {
                yield return current.ToString();
                current.Clear();
                continue;
            }

            current.Append(character);
        }

        yield return current.ToString();
    }

    // 사실(C++ 정규 형식·오프셋·크기)을 정규 형식 탐침 한 번으로 읽는다(Docs/Design/ClangVersionSupport.ko.md 4.2절의 형판).
    private sealed record FieldFacts(string Type, string Element, long Extent, long Offset);

    private static readonly Dictionary<StructDecl, (long Size, long Alignment, List<FieldFacts> Fields)> Facts = new(ReferenceEqualityComparer.Instance);

    private static async Task ProbeAsync(List<StructDecl> owned, string compiler, List<string> baseArguments, string unit, string root,
        Dictionary<string, string?> environment, bool clangCl)
    {
        var lines = new StringBuilder(CanonicalTypeProbe.Templates);
        int next = 0;
        var questions = new List<(StructDecl Owner, int Field, int Type, int Element, int Extent, int Offset)>();
        var sizes = new List<(StructDecl Owner, int Size, int Alignment)>();
        foreach (StructDecl declaration in owned)
        {
            string spelled = CanonicalTypeProbe.Spell(declaration.Qualified);
            int size = next++;
            int alignment = next++;
            lines.AppendLine(CanonicalTypeProbe.FactLine(size, $"static_cast<long long>(sizeof({spelled}))"));
            lines.AppendLine(CanonicalTypeProbe.FactLine(alignment, $"static_cast<long long>(alignof({spelled}))"));
            sizes.Add((declaration, size, alignment));
            for (int field = 0; field < declaration.Fields.Count; ++field)
            {
                string type = $"decltype({spelled}::{declaration.Fields[field]})";
                int typeQuestion = next++;
                int element = next++;
                int extent = next++;
                int offset = next++;
                lines.AppendLine(CanonicalTypeProbe.TypeLine(typeQuestion, type));
                lines.AppendLine(CanonicalTypeProbe.TypeLine(element, $"::rocks_meta_canon::RocksStaticShape<{type}>::rocksStaticElement"));
                lines.AppendLine(CanonicalTypeProbe.FactLine(extent, $"::rocks_meta_canon::RocksStaticShape<{type}>::rocksStaticExtent"));
                lines.AppendLine(CanonicalTypeProbe.FactLine(offset, $"static_cast<long long>(ROCKS_OFFSET_OF({spelled}, {declaration.Fields[field]}))"));
                questions.Add((declaration, field, typeQuestion, element, extent, offset));
            }
        }

        List<string> probeArguments = [.. baseArguments,
            .. clangCl ? new[] { "/w", "/clang:-Xclang", "/clang:-ast-dump=json", "/clang:-Xclang", "/clang:-ast-dump-filter=" + CanonicalTypeProbe.Filter }
                       : ["-w", "-Xclang", "-ast-dump=json", "-Xclang", "-ast-dump-filter=" + CanonicalTypeProbe.Filter], "-"];
        ProcessResult probed = await ClangInvocation.RunAsync(new(compiler, probeArguments, root, environment,
            CompilerDriver.ValidateGeneratedCode(unit + "#include <Base/Compiler.h>\n" + lines), TimeSpan.FromMinutes(2)), clangCl);
        Dictionary<int, string>? answers = probed.ExitCode == 0 ? CanonicalTypeProbe.Read(probed.StdOut) : null;
        if (answers is null)
        {
            throw new MetadataException("Clang rejected the shader parameter layout probe:\n" + probed.StdErr, "RMC6000",
                "Every member must be a complete type whose offset Clang can compute.");
        }

        string Answer(int index) => answers.TryGetValue(index, out string? value) ? value
            : throw new MetadataException("shader parameter layout probe has no answer " + index, "RMC6000", "Use the supported Clang version.");
        long Number(int index) => long.Parse(Answer(index), NumberStyles.Integer, CultureInfo.InvariantCulture);
        foreach ((StructDecl owner, int size, int alignment) in sizes)
        {
            Facts[owner] = (Number(size), Number(alignment), []);
        }

        foreach ((StructDecl owner, int _, int type, int element, int extent, int offset) in questions)
        {
            Facts[owner].Fields.Add(new(Answer(type), Answer(element), Number(extent), Number(offset)));
        }
    }

    // 중첩 구조체가 먼저 오도록 정렬한다(같은 모듈 안 — 2단 중첩은 Layout이 거부한다).
    private static IEnumerable<StructDecl> OrderNestedFirst(List<StructDecl> owned)
    {
        var done = new HashSet<StructDecl>(ReferenceEqualityComparer.Instance);
        var order = new List<StructDecl>();
        void Visit(StructDecl declaration)
        {
            if (!done.Add(declaration))
            {
                return;
            }

            foreach (FieldFacts field in Facts[declaration].Fields)
            {
                string type = field.Extent == 0 ? field.Type : field.Element;
                if (owned.FirstOrDefault(other => other.Qualified == type) is StructDecl nested && !ReferenceEquals(nested, declaration))
                {
                    Visit(nested);
                }
            }

            order.Add(declaration);
        }

        foreach (StructDecl declaration in owned)
        {
            Visit(declaration);
        }

        return order;
    }

    private static int AlignUp(int value, int alignment) => (value + alignment - 1) / alignment * alignment;

    // 멤버 표를 만들고 7.2절 5항 규칙을 C++ 오프셋으로 검사한다(16.2절 SSD50). 자원 멤버(SSD59–SSD61)는 상수 부분 뒤에 오며 자원 목록으로 모은다.
    private static void Layout(StructDecl declaration, List<StructDecl> owned)
    {
        ApplySpecifiers(declaration);
        (long size, long alignment, List<FieldFacts> fields) = Facts[declaration];
        string where = declaration.Qualified;
        if (alignment > MaxAlignment)
        {
            throw new MetadataException($"{where}: shader parameter struct alignment {alignment} is above 16", "RMC6006",
                "Remove the larger alignas; shader constant buffers align to 16 bytes.");
        }

        if (size == 0 || size % SlotBytes != 0)
        {
            throw new MetadataException($"{where}: shader parameter struct size {size} is not a multiple of 16 bytes", "RMC6006",
                "Declare the struct alignas(16) or add members so that its size is a multiple of 16 bytes.");
        }

        declaration.CppSize = (int)size;
        declaration.Alignment = (int)alignment;
        int end = 0;
        int padding = 0;
        int lastSlot = -1;
        Base lastBase = Base.Float;
        bool resourcePart = false;
        for (int index = 0; index < fields.Count; ++index)
        {
            FieldFacts facts = fields[index];
            string name = declaration.Fields[index];
            string member = where + "::" + name;
            if (!Identifier.IsMatch(name) || Encoding.UTF8.GetByteCount(name) > MaxNameBytes || name.StartsWith(PaddingPrefix, StringComparison.Ordinal))
            {
                throw new MetadataException($"{member}: member names must be identifiers of at most 64 bytes and must not start with '{PaddingPrefix}'",
                    "RMC6003", "Rename the member.");
            }

            bool array = facts.Extent != 0;
            if (array && facts.Element.Contains('[', StringComparison.Ordinal))
            {
                throw new MetadataException($"{member}: multi-dimensional arrays are not shader parameters", "RMC6007",
                    "Use a one-dimensional array of 16-byte elements.");
            }

            string spelling = array ? facts.Element : facts.Type;
            ScalarType? scalar = Types.FirstOrDefault(type => type.Cpp == spelling);
            StructDecl? nested = scalar is null ? owned.FirstOrDefault(other => other.Qualified == spelling) : null;
            int offset = (int)facts.Offset;
            ResourceEntry? resource = scalar is null && nested is null ? ResourceOf(member, name, spelling, offset) : null;
            if (scalar is null && nested is null && resource is null)
            {
                throw new MetadataException($"{member}: type '{facts.Type}' is not a shader parameter type", "RMC6007",
                    "Use float, int32, uint32, the ShaderCore aligned types (ShaderFloat2-ShaderUint4, ShaderFloat4x4), a shader parameter struct " +
                    "of this module or a resource member (RenderBase Device*Parameter, RenderGraph Graph*Parameter); Core Matrix4/Vector3, bool, " +
                    "double, pointers and other types are rejected (ShaderSystem 7.2 item 5, 16.2 SSD59).");
            }

            bool include = nested is not null && nested.Resources.Count != 0;
            if (resource is not null || include)
            {
                if (array)
                {
                    throw new MetadataException($"{member}: resource arrays are not shader parameters", "RMC6007",
                        "Declare one member per resource (resource arrays wait for the material system).");
                }

                if (declaration.RootConstants)
                {
                    throw new MetadataException($"{member}: root constants take scalar, vector and matrix members only", "RMC6009",
                        "Move resource members to a constant buffer or resource struct (without RootConstants).");
                }

                if (offset % SlotBytes != 0)
                {
                    throw new MetadataException($"{member}: a resource member must start on a 16-byte boundary (offset {offset})", "RMC6008",
                        "Use the RenderBase and RenderGraph resource member types, which are alignas(16).");
                }

                resourcePart = true;
                if (resource is not null)
                {
                    declaration.Resources.Add(resource);
                    continue;
                }

                if (nested!.Members.Count != 0 || nested.Includes.Count != 0 ||
                    nested.Resources.Any(entry => !IsShaderBound(entry.Kind)))
                {
                    throw new MetadataException($"{member}: an included resource struct holds shader resources only and nests one level", "RMC6007",
                        "Keep constants, access-only and render target members in the pass struct; include only resource structs of " +
                        "texture, buffer and sampler members.");
                }

                declaration.Includes.Add((name, offset, nested));
                foreach (ResourceEntry inner in nested.Resources)
                {
                    declaration.Resources.Add(new ResourceEntry
                    {
                        Name = name + "_" + inner.Name,
                        Kind = inner.Kind,
                        Offset = offset + inner.Offset,
                        Shape = inner.Shape,
                        Component = inner.Component,
                        Stride = inner.Stride,
                        Sampler = inner.Sampler,
                        Declaration = inner.Declaration,
                    });
                }

                continue;
            }

            if (resourcePart)
            {
                throw new MetadataException($"{member}: constant members come before resource members", "RMC6009",
                    "Move the constant member above the first resource member (the constant part is the front of the struct, ShaderSystem 16.2 SSD60).");
            }

            if (nested is not null)
            {
                if (ReferenceEquals(nested, declaration) || nested.Members.Count == 0)
                {
                    throw new MetadataException($"{member}: a shader parameter struct cannot contain itself", "RMC6007", "Remove the cycle.");
                }

                if (array)
                {
                    throw new MetadataException($"{member}: arrays of structs are not allowed in shader parameters", "RMC6007",
                        "Use separate members or arrays of float4.");
                }

                if (declaration.RootConstants)
                {
                    throw new MetadataException($"{member}: root constants take scalar, vector and matrix members only", "RMC6007",
                        "Flatten the nested struct into the root constants struct.");
                }

                if (nested.RootConstants || nested.Members.Any(other => other.Struct is not null))
                {
                    throw new MetadataException($"{member}: nested shader parameter structs nest one level and cannot be root constants", "RMC6007",
                        "Nest only plain shader parameter structs, one level deep.");
                }

                if (offset % SlotBytes != 0)
                {
                    throw new MetadataException($"{member}: a nested struct must start on a 16-byte boundary (offset {offset})", "RMC6008",
                        "Declare the nested struct alignas(16).");
                }
            }
            else if (array && scalar!.Bytes % SlotBytes != 0)
            {
                throw new MetadataException($"{member}: arrays need 16-byte elements ({scalar.Hlsl} is {scalar.Bytes} bytes)", "RMC6007",
                    "Pack scalars and small vectors into ShaderFloat4 arrays (ShaderSystem 7.2 item 5).");
            }

            int hlslAlignment = nested is not null || array ? SlotBytes : scalar!.Alignment;
            int natural = AlignUp(end, hlslAlignment);
            if (offset < natural)
            {
                throw new MetadataException($"{member}: C++ offset {offset} is before the shader packing offset {natural}", "RMC6008",
                    "Reorder the members or add explicit members so that C++ and HLSL agree.");
            }

            // C++ 오프셋 앞의 빈 자리는 모두 명시 padding으로 채운다(16.2절 SSD50). 규칙 오프셋과 같아도 채운다: D3D cbuffer는
            // 2성분을 4 byte 경계에도 두므로(16 byte 경계만 넘지 않으면 된다) 규칙과 C++이 8에 둔 자리를 앞에 당길 수 있다.
            for (int gap = end; gap < offset; gap += 4)
            {
                int slot = gap / SlotBytes;
                Base padBase = slot == lastSlot ? lastBase : nested is not null ? Base.Float : scalar!.Base;
                ScalarType padType = Types.First(type => type.Bytes == 4 && type.Base == padBase);
                declaration.Members.Add(new Member { Name = PaddingPrefix + padding++.ToString(CultureInfo.InvariantCulture), Offset = gap, Scalar = padType, Padding = true });
                lastSlot = slot;
                lastBase = padBase;
            }

            var added = new Member { Name = name, Offset = offset, Scalar = scalar, Struct = nested, ArrayCount = (int)facts.Extent };
            if (nested is null)
            {
                int firstSlot = offset / SlotBytes;
                if (firstSlot == lastSlot && scalar!.Base != lastBase)
                {
                    throw new MetadataException($"{member}: a 16-byte slot must not mix float, int and uint members", "RMC6008",
                        "Move the member to the next 16-byte slot with padding members or reorder (ShaderSystem 7.2 item 5).");
                }

                lastSlot = (offset + added.HlslBytes - 1) / SlotBytes;
                lastBase = scalar!.Base;
            }
            else
            {
                lastSlot = -1;
            }

            declaration.Members.Add(added);
            end = offset + added.HlslBytes;
        }

        if ((declaration.Members.Count == 0 && declaration.Resources.Count == 0) || declaration.Members.Count > MaxMembers)
        {
            throw new MetadataException($"{where}: shader parameter structs have 1 to 256 members including padding ({declaration.Members.Count})", "RMC6006",
                "Split the struct.");
        }

        if (declaration.Resources.Count > MaxResources)
        {
            throw new MetadataException($"{where}: shader parameter structs have at most 256 resource members ({declaration.Resources.Count})", "RMC6006",
                "Split the struct.");
        }

        // 표의 totalBytes는 상수 부분 byte(cbuffer 크기)다. 자원 멤버가 없으면 C++ 크기와 같다(SSD60).
        int constantBytes = AlignUp(end, SlotBytes);
        if (declaration.Resources.Count == 0 && constantBytes != declaration.CppSize)
        {
            throw new MetadataException($"{where}: C++ size {declaration.CppSize} does not match the shader size {constantBytes}", "RMC6006",
                "Remove trailing alignment padding beyond the next 16-byte boundary.");
        }

        if (declaration.Resources.Count != 0 && declaration.Resources[0].Offset < constantBytes)
        {
            throw new MetadataException($"{where}: the first resource member is inside the constant part ({constantBytes} bytes)", "RMC6008",
                "Use the RenderBase and RenderGraph resource member types, which are alignas(16).");
        }

        int limit = declaration.RootConstants ? MaxRootConstantBytes : MaxConstantBufferBytes;
        if (constantBytes > limit)
        {
            throw new MetadataException($"{where}: shader parameter struct is {constantBytes} bytes, above the {(declaration.RootConstants ? "root constants" : "constant buffer")} limit of {limit}",
                "RMC6006", declaration.RootConstants ? "Move large values to a constant buffer struct (without RootConstants)." : "Split the struct.");
        }

        declaration.Size = constantBytes;
        if (declaration.Resources.Count(entry => entry.Kind == "RenderTargets") > 1)
        {
            throw new MetadataException($"{where}: a shader parameter struct has at most one render target member", "RMC6007",
                "Declare all color and depth targets in one GraphRenderTargetParameter.");
        }

        CheckGlobalNames(declaration);
    }

    // 셰이더 전역 이름(cbuffer 이름·상수 멤버·자원·짝 샘플러)이 겹치지 않고 예약 이름이 아닌지 본다(16.2절 SSD61).
    private static void CheckGlobalNames(StructDecl declaration)
    {
        if (declaration.Resources.Count == 0)
        {
            return;
        }

        var names = new HashSet<string>(StringComparer.Ordinal);
        if (declaration.Members.Count != 0)
        {
            names.Add(declaration.Name);
            foreach (Member member in declaration.Members)
            {
                names.Add(member.Name);
            }
        }

        foreach (ResourceEntry resource in declaration.Resources.Where(entry => IsShaderBound(entry.Kind)))
        {
            foreach (string name in resource.SamplerName.Length == 0 ? new[] { resource.Name } : [resource.Name, resource.SamplerName])
            {
                if (Encoding.UTF8.GetByteCount(name) > MaxNameBytes || ReservedNames.Contains(name) || !names.Add(name))
                {
                    throw new MetadataException($"{declaration.Qualified}: shader resource name '{name}' repeats another shader name, is reserved or is longer than 64 bytes",
                        "RMC6010", "Rename the member; generated names are <member>, <member>Sampler and <include>_<member> " +
                        "(SharedWrap, SharedClamp, SharedWrapSampler, SharedClampSampler and RocksDebugBuffer are reserved).");
                }
            }
        }
    }

    // 자원 멤버 타입(틀<서술자>)을 푼다(16.2절 SSD59). 자원 틀이 아니면 null이다.
    private static ResourceEntry? ResourceOf(string member, string name, string spelling, int offset)
    {
        (string holder, List<string> arguments) = SplitTemplate(spelling);
        if (!Holders.TryGetValue(holder, out (string Kind, Category Category) info))
        {
            return null;
        }

        if (info.Category is Category.Access or Category.RenderTargets)
        {
            return new ResourceEntry { Name = name, Kind = info.Kind, Offset = offset, Member = name };
        }

        (string descriptorName, List<string> parameters) = arguments.Count == 1 ? SplitTemplate(arguments[0]) : ("", []);
        if (!Descriptors.TryGetValue(descriptorName, out Descriptor? descriptor) || descriptor.Category != info.Category)
        {
            throw new MetadataException($"{member}: '{spelling}' needs a {CategoryText(info.Category)} descriptor of ShaderCore/ShaderResourceTypes.h",
                "RMC6011", "Use the ShaderTexture*, ShaderBuffer*, ShaderStructuredBuffer*, ShaderByteAddressBuffer and ShaderRW* descriptors that fit " +
                "the resource member type (ShaderSystem 16.2 SSD59).");
        }

        if (descriptor.Shape == "RawBuffer")
        {
            return new ResourceEntry { Name = name, Kind = info.Kind, Offset = offset, Shape = "RawBuffer", Declaration = descriptor.Hlsl, Member = name };
        }

        // Clang 정규 철자는 기본 템플릿 인자를 생략한다(ShaderTexture2D<> — 원소 ShaderFloat4, 짝 ShaderFilterSampler).
        if (parameters.Count == 0 && descriptor.Category == Category.TextureSrv)
        {
            parameters.Add(DefaultTextureElement);
        }

        ScalarType? element = parameters.Count == 0 ? null : Types.FirstOrDefault(type => type.Cpp == parameters[0]);
        bool structured = descriptor.Shape == "StructuredBuffer";
        if (element is null || (element.Enum == "Float4x4" && !structured))
        {
            throw new MetadataException($"{member}: '{(parameters.Count == 0 ? "" : parameters[0])}' is not a shader resource element type", "RMC6011",
                "Use float, int32, uint32 or a ShaderCore aligned vector (structured buffers also take ShaderFloat4x4).");
        }

        if (structured && element.Bytes == 12)
        {
            throw new MetadataException($"{member}: structured buffer elements cannot be 3-component vectors (C++ 16 bytes, HLSL stride 12)", "RMC6011",
                "Use a 4-component element.");
        }

        string format = "";
        if (descriptor.Format)
        {
            string tag = parameters.Count > 1 ? parameters[1] : "";
            if (!ImageFormats.TryGetValue(tag, out (string Spelling, Base Base, int Channels) image) || image.Base != element.Base ||
                image.Channels != Channels(element))
            {
                throw new MetadataException($"{member}: image format '{tag}' does not fit the element {element.Hlsl}", "RMC6011",
                    "Use a ShaderImage* format tag whose base type and channel count match the element (r32ui with uint32, rgba16f with ShaderFloat4).");
            }

            format = $"ROCKS_IMAGE_FORMAT({image.Spelling}) ";
        }

        string sampler = "None";
        if (descriptor.Sampler && element.Base == Base.Float)
        {
            sampler = parameters.Count > 1 && parameters[1] == ComparisonSamplerTag ? "Comparison" : "Filter";
        }
        else if (descriptor.Sampler && parameters.Count > 1 && parameters[1] == ComparisonSamplerTag)
        {
            throw new MetadataException($"{member}: comparison samplers pair with float textures only", "RMC6011",
                "Use a float element for depth comparison textures.");
        }

        return new ResourceEntry
        {
            Name = name,
            Kind = info.Kind,
            Offset = offset,
            Shape = descriptor.Shape,
            Component = structured ? "None" : element.Base.ToString(),
            Stride = structured ? element.Bytes : 0,
            Sampler = sampler,
            Declaration = $"{format}{descriptor.Hlsl}<{element.Hlsl}>",
            Member = name,
        };
    }

    private static int Channels(ScalarType element) =>
        char.IsDigit(element.Hlsl[^1]) ? element.Hlsl[^1] - '0' : 1;

    private static string CategoryText(Category category) => category switch
    {
        Category.TextureSrv => "shader resource texture",
        Category.BufferSrv => "shader resource buffer",
        Category.TextureUav => "read-write texture",
        Category.BufferUav => "read-write buffer",
        _ => "resource",
    };

    // "이름<인자, 인자<…>>"를 이름과 최상위 인자로 나눈다. 템플릿이 아니면 인자가 없다.
    private static (string Name, List<string> Arguments) SplitTemplate(string spelling)
    {
        string text = spelling.Trim();
        int open = text.IndexOf('<', StringComparison.Ordinal);
        if (open < 0 || !text.EndsWith('>'))
        {
            return (text, []);
        }

        var arguments = new List<string>();
        var current = new StringBuilder();
        int depth = 0;
        foreach (char character in text[(open + 1)..^1])
        {
            depth += character == '<' ? 1 : character == '>' ? -1 : 0;
            if (character == ',' && depth == 0)
            {
                arguments.Add(current.ToString().Trim());
                current.Clear();
                continue;
            }

            current.Append(character);
        }

        if (current.Length != 0)
        {
            arguments.Add(current.ToString().Trim());
        }

        return (text[..open].Trim(), arguments);
    }

    private static bool IsShaderBound(string kind) => kind is not ("GraphTextureAccess" or "GraphBufferAccess" or "RenderTargets");

    private static void CheckModuleNames(List<StructDecl> owned, string module)
    {
        foreach (IGrouping<string, StructDecl> group in owned.GroupBy(declaration => declaration.Name, StringComparer.Ordinal))
        {
            if (group.Count() > 1)
            {
                throw new MetadataException($"module {module}: shader parameter name '{group.Key}' is used by {string.Join(", ", group.Select(d => d.Qualified))}",
                    "RMC6004", "Give every shader parameter struct of a module a distinct Name.");
            }
        }
    }

    private static string VirtualPath(Options options, StructDecl declaration) =>
        $"{options.VirtualRoot}/Generated/{options.Module}/{declaration.Simple}.rsh";

    private static string HlslType(Member member) => member.Struct is not null ? member.Struct.Simple : member.Scalar!.Hlsl;

    private static void AppendMembers(StringBuilder text, StructDecl declaration)
    {
        foreach (Member member in declaration.Members)
        {
            string array = member.ArrayCount == 0 ? "" : $"[{member.ArrayCount.ToString(CultureInfo.InvariantCulture)}]";
            text.Append(CultureInfo.InvariantCulture, $"    {HlslType(member)} {member.Name}{array};\n");
        }
    }

    // 자원 멤버의 HLSL 전역 선언이다(16.2절 SSD61).
    private static void AppendResources(StringBuilder text, StructDecl declaration)
    {
        foreach (ResourceEntry resource in declaration.Resources.Where(entry => entry.Declaration is not null))
        {
            text.Append(CultureInfo.InvariantCulture, $"{resource.Declaration} {resource.Name};\n");
            if (resource.Sampler != "None")
            {
                string state = resource.Sampler == "Comparison" ? "SamplerComparisonState" : "SamplerState";
                text.Append(CultureInfo.InvariantCulture, $"{state} {resource.SamplerName};\n");
            }
        }
    }

    private static void AppendStruct(StringBuilder text, StructDecl declaration)
    {
        text.Append(CultureInfo.InvariantCulture, $"#ifndef ROCKS_SHADER_STRUCT_{declaration.Simple}\n");
        text.Append(CultureInfo.InvariantCulture, $"#define ROCKS_SHADER_STRUCT_{declaration.Simple}\n");
        text.Append(CultureInfo.InvariantCulture, $"struct {declaration.Simple} {{\n");
        AppendMembers(text, declaration);
        text.Append("};\n#endif\n\n");
    }

    // 생성 HLSL 원문이다(16.2절 SSD52).
    private static string HlslOf(StructDecl declaration)
    {
        var text = new StringBuilder();
        text.Append(CultureInfo.InvariantCulture,
            $"// Generated by Rocks.MetaCompiler from {declaration.Input!.Include} ({declaration.Qualified}). Do not edit.\n#pragma once\n\n");
        foreach (StructDecl nested in declaration.Members.Where(member => member.Struct is not null).Select(member => member.Struct!).Distinct())
        {
            AppendStruct(text, nested);
        }

        if (declaration.RootConstants)
        {
            // 루트 상수 매크로는 구조체 타입을 받는다. cbuffer는 멤버를 바로 담으므로 구조체를 내지 않는다(cbuffer 이름과 겹치지 않게).
            AppendStruct(text, declaration);
            text.Append(CultureInfo.InvariantCulture, $"ROCKS_ROOT_CONSTANTS({declaration.Simple}, {declaration.Name});\n");
        }
        else if (declaration.Members.Count != 0)
        {
            text.Append(CultureInfo.InvariantCulture, $"cbuffer {declaration.Name} {{\n");
            AppendMembers(text, declaration);
            text.Append("};\n");
        }

        if (declaration.Resources.Any(entry => entry.Declaration is not null))
        {
            if (declaration.Members.Count != 0)
            {
                text.Append('\n');
            }

            AppendResources(text, declaration);
        }

        return text.ToString();
    }

    private static string CppString(string text) => "\"" + text.Replace("\\", "\\\\", StringComparison.Ordinal)
        .Replace("\"", "\\\"", StringComparison.Ordinal).Replace("\n", "\\n\"\n        \"", StringComparison.Ordinal) + "\"";

    private static void Write(List<StructDecl> owned, Options options)
    {
        var utf8 = new UTF8Encoding(false, true);
        var produced = new HashSet<string>(OperatingSystem.IsWindows() ? StringComparer.OrdinalIgnoreCase : StringComparer.Ordinal);
        foreach (IGrouping<ModuleInput, StructDecl> header in owned.GroupBy(declaration => declaration.Input!))
        {
            ModuleInput input = header.Key;
            var text = new StringBuilder();
            text.Append(CultureInfo.InvariantCulture,
                $"// Generated by Rocks.MetaCompiler (shader parameters, Docs/Design/ShaderSystem section 16.2) from {input.Include}. Do not edit.\n");
            text.Append("#pragma once\n\n");
            text.Append(CultureInfo.InvariantCulture, $"#include <{input.Include}>\n");
            IEnumerable<StructDecl> linked = header.SelectMany(declaration => declaration.Members.Where(member => member.Struct is not null)
                .Select(member => member.Struct!).Concat(declaration.Includes.Select(entry => entry.Struct)));
            foreach (string other in linked.Where(nested => nested.Input != input).Select(nested => nested.Input!.Include).Distinct()
                         .Order(StringComparer.Ordinal))
            {
                text.Append(CultureInfo.InvariantCulture, $"#include <{other[..^2]}{GeneratedHeaderSuffix}>\n");
            }

            text.Append("#include <Base/Compiler.h>\n#include <ShaderCore/ShaderParameterLayout.h>\n\nnamespace rocks::shader {\n\n");
            foreach (StructDecl declaration in OrderNestedFirst([.. header]))
            {
                string type = CanonicalTypeProbe.Spell(declaration.Qualified);
                string hlsl = HlslOf(declaration);
                text.Append(CultureInfo.InvariantCulture, $"template <>\nstruct ShaderParameterStruct<{type}> {{\n");
                if (declaration.Members.Count == 0)
                {
                    text.Append("    static constexpr Span<const ShaderParameterMember> Members{};\n");
                }
                else
                {
                    text.Append("    static constexpr ShaderParameterMember Members[] = {\n");
                    foreach (Member member in declaration.Members)
                    {
                        string kind = member.Struct is not null ? "Struct" : member.Scalar!.Enum;
                        string nested = member.Struct is not null
                            ? $", &ShaderParameterStruct<{CanonicalTypeProbe.Spell(member.Struct.Qualified)}>::Layout" : "";
                        text.Append(CultureInfo.InvariantCulture,
                            $"        {{\"{member.Name}\", ShaderParameterType::{kind}, {member.Offset}, {member.ArrayCount}{nested}}},\n");
                    }

                    text.Append("    };\n");
                }

                if (declaration.Resources.Count == 0)
                {
                    text.Append("    static constexpr Span<const ShaderParameterResource> Resources{};\n");
                }
                else
                {
                    text.Append("    static constexpr ShaderParameterResource Resources[] = {\n");
                    foreach (ResourceEntry resource in declaration.Resources)
                    {
                        text.Append(CultureInfo.InvariantCulture,
                            $"        {{\"{resource.Name}\", \"{resource.SamplerName}\", ShaderParameterResourceKind::{resource.Kind}, {resource.Offset}, " +
                            $"ShaderResourceShape::{resource.Shape}, ShaderComponentType::{resource.Component}, {resource.Stride}, ShaderParameterSampler::{resource.Sampler}}},\n");
                    }

                    text.Append("    };\n");
                }

                text.Append(CultureInfo.InvariantCulture, $"    static constexpr StringView Source = StringView(\n        {CppString(hlsl)});\n");
                text.Append(CultureInfo.InvariantCulture,
                    $"    static constexpr ShaderParameterLayout Layout{{\"{declaration.Name}\", {(declaration.RootConstants ? "true" : "false")}, {declaration.Size}, Members,\n" +
                    $"                                                Resources, \"{declaration.Qualified}\", \"{VirtualPath(options, declaration)}\", Source}};\n");
                text.Append("};\n\n");
                text.Append(CultureInfo.InvariantCulture,
                    $"static_assert(sizeof({type}) == {declaration.CppSize} && alignof({type}) <= 16, \"{declaration.Qualified} layout changed; rebuild to regenerate\");\n");
                foreach (Member member in declaration.Members.Where(member => !member.Padding))
                {
                    text.Append(CultureInfo.InvariantCulture,
                        $"static_assert(ROCKS_OFFSET_OF({type}, {member.Name}) == {member.Offset}, \"{declaration.Qualified}::{member.Name} offset changed\");\n");
                }

                IEnumerable<(string Name, int Offset)> placed = declaration.Resources.Where(entry => entry.Member is not null)
                    .Select(entry => (entry.Member!, entry.Offset)).Concat(declaration.Includes.Select(entry => (entry.Name, entry.Offset)));
                foreach ((string name, int offset) in placed.OrderBy(entry => entry.Offset))
                {
                    text.Append(CultureInfo.InvariantCulture,
                        $"static_assert(ROCKS_OFFSET_OF({type}, {name}) == {offset}, \"{declaration.Qualified}::{name} offset changed\");\n");
                }

                text.Append('\n');
                string shader = Path.Combine(options.OutputRoot, ShadersDirectoryName, declaration.Simple + ".rsh");
                WriteIfChanged(shader, utf8.GetBytes(hlsl));
                produced.Add(Path.GetFullPath(shader));
            }

            text.Append("} // namespace rocks::shader\n");
            string generated = CompilerDriver.ValidateGeneratedCode(text.ToString());
            string path = Path.Combine(options.OutputRoot, IncludeDirectoryName, input.Include[..^2] + GeneratedHeaderSuffix);
            WriteIfChanged(path, utf8.GetBytes(generated));
            produced.Add(Path.GetFullPath(path));
        }

        // 이번 실행이 내지 않은 낡은 생성물(지운 구조체·헤더)을 지운다. 출력 루트는 이 모드 전용이다.
        foreach (string directory in new[] { IncludeDirectoryName, ShadersDirectoryName }.Select(name => Path.Combine(options.OutputRoot, name)))
        {
            if (!Directory.Exists(directory))
            {
                continue;
            }

            foreach (string existing in Directory.EnumerateFiles(directory, "*", SearchOption.AllDirectories).Order(StringComparer.Ordinal))
            {
                if (!produced.Contains(Path.GetFullPath(existing)))
                {
                    File.Delete(existing);
                }
            }
        }
    }

    private static void WriteIfChanged(string path, byte[] bytes)
    {
        if (File.Exists(path) && File.ReadAllBytes(path).AsSpan().SequenceEqual(bytes))
        {
            return;
        }

        Directory.CreateDirectory(Path.GetDirectoryName(path)!);
        string temporary = path + ".tmp-" + Guid.NewGuid().ToString("N");
        File.WriteAllBytes(temporary, bytes);
        File.Move(temporary, path, true);
    }
}
