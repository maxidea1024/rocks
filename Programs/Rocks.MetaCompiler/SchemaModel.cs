namespace Rocks.MetaCompiler;

internal sealed record SourceSite(string File, int Line);

internal sealed class MetadataException(string message, string code = "RMC2000", string? hint = null) : Exception(message)
{
    public string Code { get; } = code;

    public string Hint { get; } = hint ?? "Correct the named declaration using the supported CLASS/PROPERTY syntax; unsupported constructs must be replaced or explicitly implemented.";

    public SourceSite? Site { get; private set; }

    public MetadataException At(SourceSite site)
    {
        Site ??= site;
        return this;
    }
}

internal sealed class FieldModel
{
    public required string Id { get; init; }

    public required SourceSite Site { get; init; }

    public required string Name { get; init; }

    public required string Owner { get; init; }

    public required string NativeSpelling { get; init; }

    public required PropertySpec Spec { get; init; }

    public string Kind { get; set; } = "Opaque";

    // 이 모듈 안에서 해석한 참조 대상의 정규 이름이다(rocks::Object 포함). import 필드는 null이고, import 필드의
    // Enum 대상만 SchemaValidator가 import enum에서 채운다(상속한 enum 필드를 생성할 때 쓴다).
    public string? Reference { get; set; }

    // 참조 대상 타입 ID(32자리 16진, Object는 null)다. import 필드는 descriptor에서 바로 온다.
    public string? ReferenceId { get; set; }

    public bool IsReference => Kind == "ObjectReference";

    // ValueType.type(대상 타입 ID)을 쓰는 종류다: 강한 참조·소프트 참조의 기대 타입과 Enum의 enum ID.
    public bool HasTargetType => KindHasTargetType(Kind);

    // Required를 쓸 수 있는 종류다: 강한 참조와 소프트 참조.
    public bool AcceptsRequired => KindAcceptsRequired(Kind);

    // descriptor 읽기·쓰기와 생성이 같은 조건을 쓰도록 종류 이름으로도 판정한다.
    public static bool KindHasTargetType(string kind) => kind is "ObjectReference" or "SoftObjectReference" or "Enum";

    public static bool KindAcceptsRequired(string kind) => kind is "ObjectReference" or "SoftObjectReference";

    // FieldFlags 비트: Transient 1, ReadOnly 2, EditorOnly 4, Required 8.
    public uint Flags => (Spec.Transient ? 1u : 0u) | (Spec.ReadOnly ? 2u : 0u) | (Spec.EditorOnly ? 4u : 0u) | (Spec.Required ? 8u : 0u);

    // 도구 이름(지정하지 않았으면 필드 이름)이다. 상속 포함 대소문자 무시로 유일해야 한다(RMC2203).
    public string EffectiveToolName => Spec.ToolName.Length != 0 ? Spec.ToolName : Name;
}

// enum 규칙을 한 곳에 둔다. 파서·descriptor·생성기가 같이 쓰며 런타임 TypeRegistry::RegisterEnum과 같은 규칙이다.
internal static class EnumRules
{
    public const int MaxEnumerators = 4096;

    // underlying(ValueKind 이름)의 값 범위다. 지원하지 않는 underlying이면 null이다.
    public static (long Minimum, long Maximum)? Range(string underlying) => underlying switch
    {
        "UInt8" => (0L, 0xffL),
        "UInt16" => (0L, 0xffffL),
        "UInt32" => (0L, 0xffffffffL),
        "Int32" => (int.MinValue, int.MaxValue),
        _ => null
    };

    // underlying의 rocks 정수 타입 이름이다(생성 static_assert용).
    public static string NativeType(string underlying) => underlying switch
    {
        "UInt8" => "uint8",
        "UInt16" => "uint16",
        "UInt32" => "uint32",
        _ => "int32"
    };

    // 접두 유무를 합친 두 도구 이름 집합 {a, prefix+a}와 {b, prefix+b}가 ASCII 대소문자 무시로 겹치는지다.
    public static bool ToolNamesCollide(string left, string right, string prefix) =>
        string.Equals(left, right, StringComparison.OrdinalIgnoreCase) ||
        (prefix.Length != 0 && (string.Equals(prefix + left, right, StringComparison.OrdinalIgnoreCase) ||
                                string.Equals(left, prefix + right, StringComparison.OrdinalIgnoreCase)));
}

// enum 열거자 하나다. ToolName은 Name 지정자(C++ 이름과 같거나 없으면 "")다.
internal sealed record EnumeratorModel(string Name, string ToolName, string DisplayName, long Value, bool Hidden)
{
    public string EffectiveToolName => ToolName.Length != 0 ? ToolName : Name;
}

// 리플렉션 타입(ROCKS_CLASS 클래스와 ROCKS_ENUM enum) 모델이다. enum은 IsEnum이고 Base는 "", 필드는 없으며
// Underlying·FlagsEnum·ToolPrefix·Enumerators를 쓴다. enum도 클래스와 같은 이름·ID 공간과 descriptor·inventory
// 경로를 쓴다.
internal sealed class ClassModel
{
    public required string Name { get; init; }

    public required string Id { get; init; }

    public required SourceSite Site { get; init; }

    public required uint Version { get; init; }

    public required string Base { get; set; }

    public required bool Abstract { get; init; }

    // TypeFlags::WorldOnly. 선언이 직접 쓰거나 base에서 상속된다(SchemaValidator가 base를 보고 채운다).
    public bool WorldOnly { get; set; }

    // TypeFlags 비트: Abstract 1, WorldOnly 2.
    public uint Flags => (Abstract ? 1u : 0u) | (WorldOnly ? 2u : 0u);

    // ROCKS_NATIVE 멤버 이름이다(스키마에 넣지 않고 생성 코드의 static_assert만 받는다).
    public List<string> NativeMembers { get; } = [];

    // RocksRequiresWorldOnlyOwner 형질을 담는 NATIVE 멤버다. 상속까지 본 WorldOnly가 아니면 RMC2206이다.
    public List<(string Symbol, string Path, SourceSite Site)> WorldOnlyNatives { get; } = [];

    // 선언이 있는 입력 헤더의 include 철자다(import 타입은 descriptor의 header).
    public string Header { get; set; } = "";

    public bool Private { get; set; }

    // 다른 모듈 descriptor에서 온 타입이다. 생성·inventory 대상이 아니다.
    public bool Imported { get; init; }

    public string Module { get; init; } = "";

    public List<FieldModel> OwnFields { get; } = [];

    public List<FieldModel> Fields { get; set; } = [];

    public bool IsEnum { get; init; }

    // enum underlying의 ValueKind 이름(UInt8·UInt16·UInt32·Int32)이다.
    public string Underlying { get; init; } = "";

    // EnumFlags::Flags(비트 플래그 enum)다.
    public bool FlagsEnum { get; init; }

    public string ToolPrefix { get; init; } = "";

    public List<EnumeratorModel> Enumerators { get; init; } = [];
}

// Classes는 이 모듈 타입(클래스·enum)만, Imported는 import한 타입(정규 이름 → 모델)이다. Ordered는 이 모듈 타입의
// 생성·등록 순서다(enum 먼저, 클래스는 base 먼저).
internal sealed record SchemaModel(IReadOnlyDictionary<string, ClassModel> Classes, IReadOnlyList<ClassModel> Ordered,
    IReadOnlyDictionary<string, ClassModel> Imported)
{
    public ClassModel Find(string name) =>
        Classes.TryGetValue(name, out ClassModel? own) ? own : Imported[name];
}
