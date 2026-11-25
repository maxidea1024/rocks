using Rocks.Build.Rules;

// Asset cooker (Docs/Design/AssetSystem section 10 and 10.2): target profiles and cook settings, cook target sets from the asset index,
// cook keys (build key Build SCC, cook key Hard SCC), cook processors, cooking from editor packages and publishing the cooked manifest.
// Developer module: editor and tools only (games read cooked data). AssetDatabase: index and editor packages. AssetRuntime: package writer
// and cooked manifest. AssetNugget: key writer, SCC keys and the nugget cache. ReflectionText: settings codec.
public sealed class AssetCooker : EngineModuleRules
{
    protected override string Group => "Developer";

    protected override string ModuleName => "AssetCooker";

    // TargetProfiles: the target profile struct, layering and checks (section 10.3.1 A4f1 — it also brings the Texture enums).
    protected override string[] PublicDependencies => ["AssetDatabase", "AssetRuntime", "AssetNugget", "ObjectSerialization", "ReflectionText", "TargetProfiles"];
}
