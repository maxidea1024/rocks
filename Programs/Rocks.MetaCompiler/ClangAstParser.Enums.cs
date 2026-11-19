using System.Globalization;
using System.Numerics;
using System.Text.Json;

namespace Rocks.MetaCompiler;

// ClangAstParser 중 ROCKS_ENUM 정의의 모델 생성(VisitEnum)과 underlying 명시 판정(HasExplicitUnderlying)이다
// (Docs/Design/ReflectionFoundations.ko.md 3.4절).
internal sealed partial class ClangAstParser
{
    // ROCKS_ENUM 정의 하나를 모델로 만든다(Docs/Design/ReflectionFoundations.ko.md 3.4절). underlying은 원문에
    // 명시한 정수 8종(uint8·uint16·uint32·uint64·int8·int16·int32·int64 — Docs/Design/ReflectionExtensions.ko.md 2절)만,
    // 열거자는 하나 이상이고 값 별칭이 없어야 하며 Flags enum은 음수 값을, uint64 enum은 int64 최댓값보다 큰 값을 둘
    // 수 없다(RMC2401). 접두 유무를 합친 도구 이름은 대소문자 무시로 유일하다(RMC2203).
    private void VisitEnum(JsonElement node, string[] scope, bool anonymous, bool templated, PrefixBinding prefix)
    {
        string name = Text(node, "name");
        string qualified = string.Join("::", scope.Append(name));
        UpdateSite(node);
        SourceSite enumSite = currentSite;
        currentSite = prefix.Site;
        if (anonymous || templated || name.Length == 0)
        {
            throw new MetadataException($"{qualified}: anonymous/template/invalid reflected enum is unsupported");
        }

        if (prefix.Arguments.Length != 1 || prefix.Arguments[0] is not string enumSpecifiers)
        {
            throw new MetadataException($"{qualified}: invalid ENUM annotation arguments");
        }

        // 의존 모듈의 리플렉션 enum은 그 모듈 descriptor의 같은 이름·같은 헤더 선언이어야 한다(클래스와 같다).
        string? physical = locations.TryGetValue(Text(node, "id"), out var declared) ? declared.File : null;
        ModuleInput? owner = OwningInput(physical);
        if (owner is null)
        {
            if (!imported.TryGetValue(qualified, out ClassModel? external) || !external.IsEnum || physical is null ||
                resolveHeader(external.Header) is not string externalFile || !CanonicalPath.Equals(externalFile, physical))
            {
                throw new MetadataException($"{qualified}: reflected declaration is neither in this module's Metadata headers nor in an imported descriptor (same name and header)",
                    "RMC2003", "Add the header to the owning module's Metadata list and depend on that module; do not redeclare reflected types.");
            }

            return;
        }

        EnumSpec spec = Specifiers.Enum(enumSpecifiers, qualified);
        string id = Identity("type", qualified);
        currentSite = enumSite;
        if (Text(node, "scopedEnumTag") is not ("class" or "struct"))
        {
            throw new MetadataException($"{qualified}: ROCKS_ENUM requires a scoped enum (enum class)", "RMC2401",
                "Declare the reflected enum as 'enum class Name : rocks::uint8 { ... }'.");
        }

        string underlyingSpelling = "";
        if (node.TryGetProperty("fixedUnderlyingType", out JsonElement fixedType))
        {
            string desugared = Text(fixedType, "desugaredQualType");
            underlyingSpelling = desugared.Length != 0 ? desugared : Text(fixedType, "qualType");
        }

        if (!HasExplicitUnderlying(node))
        {
            throw new MetadataException($"{qualified}: ROCKS_ENUM requires an explicitly written integer underlying type", "RMC2401",
                "Write the underlying type directly after the enum name, e.g. 'enum class Name : rocks::uint8'.");
        }

        // underlying은 UE와 같은 정수 8종이다(Docs/Design/ReflectionExtensions.ko.md 2절 — Clang desugar 철자). 평범한 char·bool·
        // long(폭이 플랫폼마다 다르다)은 받지 않는다.
        string? underlying = underlyingSpelling switch
        {
            "unsigned char" => "UInt8",
            "unsigned short" => "UInt16",
            "unsigned int" => "UInt32",
            "unsigned long long" => "UInt64",
            "signed char" => "Int8",
            "short" => "Int16",
            "int" => "Int32",
            "long long" => "Int64",
            _ => null
        };
        if (underlying is null)
        {
            throw new MetadataException($"{qualified}: unsupported ROCKS_ENUM underlying type '{underlyingSpelling}'", "RMC2401",
                "Use rocks::uint8, uint16, uint32, uint64, int8, int16, int32 or int64; plain char, bool and long are unsupported.");
        }

        (long minimum, long maximum) = EnumRules.Range(underlying)!.Value;
        List<EnumeratorModel> enumerators = [];
        BigInteger next = BigInteger.Zero;
        foreach (JsonElement constant in Children(node))
        {
            if (Text(constant, "kind") != "EnumConstantDecl")
            {
                continue;
            }

            UpdateSite(constant);
            string constantName = Text(constant, "name");
            string symbol = qualified + "::" + constantName;
            // 런타임 스키마는 열거자 이름을 ASCII 식별자로 받는다(Clang은 비ASCII·$ 이름도 받는다).
            if (!Specifiers.IsAsciiIdentifier(constantName))
            {
                throw new MetadataException($"{symbol}: reflected enumerator names must be ASCII identifiers", "RMC2401",
                    "Rename the enumerator with [A-Za-z_][A-Za-z0-9_]*; use DisplayName for localized text.");
            }

            if (enumerators.Count == EnumRules.MaxEnumerators)
            {
                throw new MetadataException($"{qualified}: a reflected enum has at most {EnumRules.MaxEnumerators} enumerators", "RMC2401",
                    "Split the enum or keep it unreflected.");
            }

            // 초기값이 없는 열거자는 이전 값 + 1이다(첫 열거자는 0). underlying이 고정된 enum의 초기값은 Clang이 평가한
            // 값을 가진 ConstantExpr이다. 다른 모양이면 값을 추측하지 않고 거부한다(생성 코드의 static_assert가 한 번 더
            // 대조한다).
            BigInteger value = next;
            JsonElement[] initializers = Children(constant).Where(child => !Text(child, "kind").EndsWith("Attr", StringComparison.Ordinal)).ToArray();
            if (initializers.Length != 0)
            {
                if (initializers.Length != 1 || Text(initializers[0], "kind") != "ConstantExpr" ||
                    !BigInteger.TryParse(Text(initializers[0], "value"), NumberStyles.Integer, CultureInfo.InvariantCulture, out value))
                {
                    throw new MetadataException($"{symbol}: unsupported enumerator initializer AST shape", "RMC2001",
                        "Use the supported Clang/toolchain; add a reviewed parser case before accepting a new AST shape.");
                }
            }

            // UInt64의 최댓값은 int64 최댓값이다(EnumeratorSchema::value가 int64 — Flags uint64 enum은 비트 0–62).
            if (underlying == "UInt64" && value > maximum)
            {
                throw new MetadataException($"{symbol}: value {value} exceeds the int64 maximum {maximum} that UInt64 enumerators can store", "RMC2401",
                    "Keep uint64 enumerator values at most 0x7fffffffffffffff (Flags enums use bits 0-62); stored enum values are int64.");
            }

            if (value < minimum || value > maximum || (spec.Flags && value < 0))
            {
                throw new MetadataException($"{symbol}: value {value} is outside the {underlying} range{(spec.Flags ? " or negative in a Flags enum" : "")}", "RMC2401",
                    "Keep enumerator values within the underlying type; Flags enums use non-negative bit values.");
            }

            object?[]? annotation = Annotation(constant, "rocks.enumerator");
            if (annotation is not null && (annotation.Length != 1 || annotation[0] is not string))
            {
                throw new MetadataException($"{symbol}: invalid ENUMERATOR annotation arguments");
            }

            EnumeratorSpec enumeratorSpec = Specifiers.Enumerator(annotation is null ? "" : (string)annotation[0]!, symbol, constantName);
            EnumeratorModel model = new(constantName, enumeratorSpec.ToolName, enumeratorSpec.DisplayName, (long)value, enumeratorSpec.Hidden);
            EnumeratorModel? alias = enumerators.FirstOrDefault(other => other.Value == model.Value);
            if (alias is not null)
            {
                throw new MetadataException($"{symbol}: value {value} aliases {qualified}::{alias.Name}", "RMC2401",
                    "Give every reflected enumerator a distinct value; aliases make stored values ambiguous for tools.");
            }

            EnumeratorModel? collision = enumerators.FirstOrDefault(other => EnumRules.ToolNamesCollide(other.EffectiveToolName, model.EffectiveToolName, spec.ToolPrefix));
            if (collision is not null)
            {
                throw new MetadataException($"{symbol}: tool name '{model.EffectiveToolName}' collides with {qualified}::{collision.Name} (case-insensitive, with or without ToolPrefix)", "RMC2203",
                    "Give the enumerator a distinct Name=\"...\" specifier.");
            }

            enumerators.Add(model);
            next = value + 1;
        }

        if (enumerators.Count == 0)
        {
            currentSite = enumSite;
            throw new MetadataException($"{qualified}: ROCKS_ENUM requires an enum definition with at least one enumerator", "RMC2401",
                "Define the enumerators directly after ROCKS_ENUM; forward declarations and empty enums are unsupported.");
        }

        currentSite = enumSite;
        classes.Add(qualified, new ClassModel
        {
            Name = qualified,
            Id = id,
            Site = enumSite,
            Version = 1,
            Base = "",
            Abstract = false,
            Header = owner.Include,
            Private = owner.Private,
            IsEnum = true,
            Underlying = underlying,
            FlagsEnum = spec.Flags,
            ToolPrefix = spec.ToolPrefix,
            Enumerators = enumerators,
            BindsSpelling = spec.Binds,
            BindsSite = spec.Binds.Length != 0 ? prefix.Site : null
        });
    }

    // Clang은 underlying을 생략한 enum class에도 int를 fixedUnderlyingType으로 준다. 원문에서 이름 바로 뒤(공백·주석
    // 다음)에 ':'가 있는지로 명시 여부를 본다. 이름이 그 위치에 그대로 쓰여 있지 않거나(매크로·UCN) 판단이 모호하면
    // (주석 안 줄 이음) 명시하지 않은 것으로 본다 — 거부 쪽으로만 틀린다.
    private bool HasExplicitUnderlying(JsonElement node)
    {
        if (!node.TryGetProperty("loc", out JsonElement nameLocation) || nameLocation.TryGetProperty("expansionLoc", out _) ||
            !locations.TryGetValue(Text(node, "id"), out var location) || location.File is null || location.Offset < 0 ||
            !File.Exists(location.File))
        {
            return false;
        }

        if (!sourceBytes.TryGetValue(location.File, out byte[]? bytes))
        {
            bytes = File.ReadAllBytes(location.File);
            sourceBytes.Add(location.File, bytes);
        }

        byte[] name = System.Text.Encoding.UTF8.GetBytes(Text(node, "name"));
        if (location.Offset + name.Length > bytes.Length || !bytes.AsSpan(location.Offset, name.Length).SequenceEqual(name))
        {
            return false;
        }

        int cursor = location.Offset + name.Length;
        while (cursor < bytes.Length)
        {
            byte value = bytes[cursor];
            if (value is (byte)' ' or (byte)'\t' or (byte)'\r' or (byte)'\n')
            {
                ++cursor;
            }
            else if (value == (byte)'/' && cursor + 1 < bytes.Length && bytes[cursor + 1] == (byte)'/')
            {
                while (cursor < bytes.Length && bytes[cursor] != (byte)'\n')
                {
                    // 줄 이음(\ + 줄바꿈)은 다음 줄까지 주석으로 만든다.
                    if (bytes[cursor] == (byte)'\\' && (cursor + 1 < bytes.Length && (bytes[cursor + 1] is (byte)'\n' or (byte)'\r')))
                    {
                        return false;
                    }

                    ++cursor;
                }
            }
            else if (value == (byte)'/' && cursor + 1 < bytes.Length && bytes[cursor + 1] == (byte)'*')
            {
                int end = bytes.AsSpan(cursor + 2).IndexOf("*/"u8);
                if (end < 0)
                {
                    return false;
                }

                cursor += end + 4;
            }
            else
            {
                return value == (byte)':' && (cursor + 1 >= bytes.Length || bytes[cursor + 1] != (byte)':');
            }
        }

        return false;
    }
}
