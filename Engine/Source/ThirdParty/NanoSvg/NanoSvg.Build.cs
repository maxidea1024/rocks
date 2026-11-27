using Rocks.Build.Rules;

// nanosvg (zlib license): SVG parsing and rasterization for UI vector images and icons, the same library UE 5.8 uses
// (Docs/Design/UiToolkit/17-VectorImages.ko.md section 17.2). The ThirdParty.json in the same folder is the single source of
// sources, licenses and file hashes.
public sealed class NanoSvg : ThirdPartyModuleRules
{
    protected override string ModuleName => "NanoSvg";
}
