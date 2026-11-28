using Rocks.Build;
using Rocks.Build.Rules;

// Material cooking (Docs/Design/MaterialSystem section 12.8, stage MS3): gathering the translation inputs of a material from the
// open editor packages (functions, other materials, color curves, curve atlases, texture facts, parameter collection layouts),
// the reference sync that fills Material.referencedTextures and parameterCollections, the translation nugget and the material
// cook processor. Developer module: the cooker and the editor link it, shipping runtime targets do not.
public sealed class MaterialCook : EngineModuleRules
{
    protected override string Group => "Developer";

    protected override string ModuleName => "MaterialCook";

    // Unity build: this module passes the unity collision policy checks (Docs/Design/UnityBuildPch.ko.md section 9.3).
    protected override UnityMode Unity => UnityMode.On;

    // Material for the asset classes and the graph reference collection, MaterialCompiler for the translator input types and the
    // source provider interface, AssetCooker for the cook processor interface and the dependency opener, ShaderCore for the
    // library bundle id of a shader set (MaterialShaderBundleId).
    protected override string[] PublicDependencies => ["Core", "Material", "MaterialSchema", "MaterialCompiler", "AssetCooker", "AssetRuntime", "AssetNugget", "ShaderCore"];

    // Texture, CurveAssets and CurveAtlas for reading the referenced assets, ObjectRuntime for Registry access.
    protected override string[] PrivateDependencies => ["Texture", "CurveAssets", "CurveAtlas", "Curves", "ObjectRuntime", "NodeGraph"];
}
