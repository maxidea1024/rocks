namespace Rocks.MetaCompiler;

internal sealed record SourceSite(string File, int Line);

internal sealed class MetadataException(string message, string code = "RMC2000", string? hint = null) : Exception(message)
{
    public string Code { get; } = code;
    public string Hint { get; } = hint ?? "Correct the named declaration using the supported CLASS/PROPERTY syntax; unsupported constructs must be replaced or explicitly implemented.";
    public SourceSite? Site { get; private set; }
    public MetadataException At(SourceSite site) { Site ??= site; return this; }
}

internal sealed record PropertySpec(bool Transient, bool ReadOnly, string Category, string DisplayName);

internal sealed class FieldModel
{
    public required string Id { get; init; }
    public required SourceSite Site { get; init; }
    public required string Name { get; init; }
    public required string Owner { get; init; }
    public required string NativeSpelling { get; init; }
    public required PropertySpec Spec { get; init; }
    public string Kind { get; set; } = "Opaque";
    public string? Reference { get; set; }
    public uint Flags => (Spec.Transient ? 1u : 0u) | (Spec.ReadOnly ? 2u : 0u);
}

internal sealed class ClassModel
{
    public required string Name { get; init; }
    public required string Id { get; init; }
    public required SourceSite Site { get; init; }
    public required uint Version { get; init; }
    public required string Base { get; set; }
    public required bool Abstract { get; init; }
    public List<FieldModel> OwnFields { get; } = [];
    public List<FieldModel> Fields { get; set; } = [];
}

internal sealed record SchemaModel(IReadOnlyDictionary<string, ClassModel> Classes, IReadOnlyList<ClassModel> Ordered);
