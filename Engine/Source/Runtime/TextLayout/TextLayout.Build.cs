using Rocks.Build;
using Rocks.Build.Rules;

// Library-neutral font core of the text layout (Docs/Design/Font/06-TextLayoutCore.ko.md): font face data, face and metric types and
// the text backend interface. The FreeType, HarfBuzz, ICU and msdfgen implementation is the ClientOnly module TextLayoutBackend; this
// module does not depend on it, so server targets link TextLayout without the font libraries (Docs/Design/Font/03-Modules.ko.md).
public sealed class TextLayout : EngineModuleRules
{
    protected override string ModuleName => "TextLayout";

    // Unity build: this module passes the unity collision policy checks (Docs/Design/UnityBuildPch.ko.md section 9.3).
    protected override UnityMode Unity => UnityMode.On;

    protected override string[] PublicDependencies => ["Core"];
}
