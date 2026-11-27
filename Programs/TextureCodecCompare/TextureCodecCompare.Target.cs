using Rocks.Build.Rules;

// Developer tool program: TextureTools reads editor data, so the target compiles with editor data (Docs/Design/BuildTargets section
// 3.1, BD19).
public sealed class TextureCodecCompareTarget : TargetRules
{
    public override string Name => "TextureCodecCompare";

    public override TargetType Type => TargetType.Program;

    public override bool WithEditorData => true;

    public override IReadOnlyList<string> Modules => ["TextureCodecCompare"];
}
