using System.Globalization;
using System.Text;
using System.Text.Json;
using System.Text.RegularExpressions;
using Rocks.Tooling.Common;

namespace Rocks.MetaCompiler;

// 셰이더 파라미터 구조체 생성 모드다(Docs/Design/ShaderSystem 16.2절 SSD49–SSD53, 13절 P7). 리플렉션 메타 단계와 따로 돈다:
// `Rocks.MetaCompiler shader-parameters --module M --input <헤더> --include <철자> ... --output-root <디렉터리>`.
// 모듈 헤더의 ROCKS_SHADER_PARAMETERS 구조체마다 Clang이 정한 C++ 오프셋을 읽어 7.2절 5항 규칙으로 검사하고, 생성 C++ 표
// (<출력>/Include/<철자의 .h를 .shaderparams.h로>)와 생성 HLSL(<출력>/Shaders/<C++ 이름>.rsh)을 쓴다. 오류는 RMC6xxx다.
internal static class ShaderParameterGenerator
{
    // 표지 annotation 이름이다(Meta/Annotations.h ROCKS_SHADER_PARAMETERS).
    private const string MarkerAnnotation = "rocks.shaderparams.marker";

    // ShaderCore 한도다(ShaderCore/ShaderParameterLayout.h와 같다).
    private const int MaxMembers = 256;
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

        public int Size { get; set; }

        public int Alignment { get; set; }

        public List<Member> Members { get; } = [];
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
        List<string> astArguments = [.. baseArguments, .. clangCl ? new[] { "/clang:-Xclang", "/clang:-ast-dump=json" } : ["-Xclang", "-ast-dump=json"], "-"];
        ProcessResult ast = await ProcessRunner.RunAsync(new(compiler, astArguments, root, environment, CompilerDriver.ValidateGeneratedCode(unit), TimeSpan.FromMinutes(2)));
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

        await ProbeAsync(owned, compiler, baseArguments, unit, root, environment, clangCl);
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
                default:
                    throw new MetadataException("unknown shader-parameters option: " + argument, "RMC1000",
                        "Use --module, --input/--include, --private, --output-root, --compiler, --virtual-root, --definitions-header, --include-dir and --clang-arg=.");
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
        ProcessResult probed = await ProcessRunner.RunAsync(new(compiler, probeArguments, root, environment,
            CompilerDriver.ValidateGeneratedCode(unit + "#include <Base/Compiler.h>\n" + lines), TimeSpan.FromMinutes(2)));
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

    // 멤버 표를 만들고 7.2절 5항 규칙을 C++ 오프셋으로 검사한다(16.2절 SSD50).
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

        int limit = declaration.RootConstants ? MaxRootConstantBytes : MaxConstantBufferBytes;
        if (size > limit)
        {
            throw new MetadataException($"{where}: shader parameter struct is {size} bytes, above the {(declaration.RootConstants ? "root constants" : "constant buffer")} limit of {limit}",
                "RMC6006", declaration.RootConstants ? "Move large values to a constant buffer struct (without RootConstants)." : "Split the struct.");
        }

        declaration.Size = (int)size;
        declaration.Alignment = (int)alignment;
        int end = 0;
        int padding = 0;
        int lastSlot = -1;
        Base lastBase = Base.Float;
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
            if (scalar is null && nested is null)
            {
                throw new MetadataException($"{member}: type '{facts.Type}' is not a shader parameter type", "RMC6007",
                    "Use float, int32, uint32, the ShaderCore aligned types (ShaderFloat2-ShaderUint4, ShaderFloat4x4) or a shader parameter struct " +
                    "of this module; Core Matrix4/Vector3, bool, double, pointers and other types are rejected (ShaderSystem 7.2 item 5).");
            }

            int offset = (int)facts.Offset;
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

        if (declaration.Members.Count == 0 || declaration.Members.Count > MaxMembers)
        {
            throw new MetadataException($"{where}: shader parameter structs have 1 to 256 members including padding ({declaration.Members.Count})", "RMC6006",
                "Split the struct.");
        }

        if (AlignUp(end, SlotBytes) != declaration.Size)
        {
            throw new MetadataException($"{where}: C++ size {declaration.Size} does not match the shader size {AlignUp(end, SlotBytes)}", "RMC6006",
                "Remove trailing alignment padding beyond the next 16-byte boundary.");
        }
    }

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
        else
        {
            text.Append(CultureInfo.InvariantCulture, $"cbuffer {declaration.Name} {{\n");
            AppendMembers(text, declaration);
            text.Append("};\n");
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
            foreach (string other in header.SelectMany(declaration => declaration.Members).Where(member => member.Struct is not null &&
                         member.Struct.Input != input).Select(member => member.Struct!.Input!.Include).Distinct().Order(StringComparer.Ordinal))
            {
                text.Append(CultureInfo.InvariantCulture, $"#include <{other[..^2]}{GeneratedHeaderSuffix}>\n");
            }

            text.Append("#include <Base/Compiler.h>\n#include <ShaderCore/ShaderParameterLayout.h>\n\nnamespace rocks::shader {\n\n");
            foreach (StructDecl declaration in OrderNestedFirst([.. header]))
            {
                string type = CanonicalTypeProbe.Spell(declaration.Qualified);
                string hlsl = HlslOf(declaration);
                text.Append(CultureInfo.InvariantCulture, $"template <>\nstruct ShaderParameterStruct<{type}> {{\n");
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
                text.Append(CultureInfo.InvariantCulture, $"    static constexpr StringView Source = StringView(\n        {CppString(hlsl)});\n");
                text.Append(CultureInfo.InvariantCulture,
                    $"    static constexpr ShaderParameterLayout Layout{{\"{declaration.Name}\", {(declaration.RootConstants ? "true" : "false")}, {declaration.Size}, Members,\n" +
                    $"                                                \"{declaration.Qualified}\", \"{VirtualPath(options, declaration)}\", Source}};\n");
                text.Append("};\n\n");
                text.Append(CultureInfo.InvariantCulture,
                    $"static_assert(sizeof({type}) == {declaration.Size} && alignof({type}) <= 16, \"{declaration.Qualified} layout changed; rebuild to regenerate\");\n");
                foreach (Member member in declaration.Members.Where(member => !member.Padding))
                {
                    text.Append(CultureInfo.InvariantCulture,
                        $"static_assert(ROCKS_OFFSET_OF({type}, {member.Name}) == {member.Offset}, \"{declaration.Qualified}::{member.Name} offset changed\");\n");
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
