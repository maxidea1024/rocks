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

    // 이 모듈 안에서 해석한 참조 대상의 정규 이름이다(rocks::Object 포함). import 필드는 null이다.
    public string? Reference { get; set; }

    // 참조 대상 타입 ID(32자리 16진, Object는 null)다. import 필드는 descriptor에서 바로 온다.
    public string? ReferenceId { get; set; }

    public bool IsReference => Kind == "ObjectReference";

    // FieldFlags 비트: Transient 1, ReadOnly 2, EditorOnly 4, Required 8.
    public uint Flags => (Spec.Transient ? 1u : 0u) | (Spec.ReadOnly ? 2u : 0u) | (Spec.EditorOnly ? 4u : 0u) | (Spec.Required ? 8u : 0u);

    // 도구 이름(지정하지 않았으면 필드 이름)이다. 상속 포함 대소문자 무시로 유일해야 한다(RMC2203).
    public string EffectiveToolName => Spec.ToolName.Length != 0 ? Spec.ToolName : Name;
}

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
}

// Classes는 이 모듈 타입만, Imported는 import한 타입(정규 이름 → 모델)이다. Ordered는 이 모듈 타입의 생성 순서다.
internal sealed record SchemaModel(IReadOnlyDictionary<string, ClassModel> Classes, IReadOnlyList<ClassModel> Ordered,
    IReadOnlyDictionary<string, ClassModel> Imported)
{
    public ClassModel Find(string name) =>
        Classes.TryGetValue(name, out ClassModel? own) ? own : Imported[name];
}
