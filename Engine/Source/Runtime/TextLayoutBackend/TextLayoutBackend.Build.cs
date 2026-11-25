using Rocks.Build;
using Rocks.Build.Descriptors;
using Rocks.Build.Rules;

// Font library backend of the text layout (Docs/Design/Font/03-Modules.ko.md, 06-TextLayoutCore.ko.md section 6.8): FreeType faces,
// HarfBuzz shaping, ICU boundaries and msdfgen distance fields behind the TextLayout TextBackend interface. ClientOnly: server targets do
// not link the font libraries (BuildTargets BD25 — a Runtime module may not depend on it, so hosts create the backend and inject it).
public sealed class TextLayoutBackend : EngineModuleRules
{
    protected override string ModuleName => "TextLayoutBackend";

    protected override ModuleType ModuleType => ModuleType.ClientOnly;

    // Unity build: this module passes the unity collision policy checks (Docs/Design/UnityBuildPch.ko.md section 9.3).
    protected override UnityMode Unity => UnityMode.On;

    protected override string[] PublicDependencies => ["Core", "TextLayout"];

    // The font libraries stay private: no library type appears in the public headers (Docs/Design/Font FR5). HarfBuzz and Msdfgen come
    // before FreeType and Icu so that a left-to-right static linker resolves their references.
    protected override string[] PrivateDependencies => ["HarfBuzz", "Msdfgen", "FreeType", "LibPng", "Icu"];
}
