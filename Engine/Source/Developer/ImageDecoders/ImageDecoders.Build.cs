using Rocks.Build;
using Rocks.Build.Rules;

// Texture source image decoders (Docs/Design/AssetSystem sections 12.2, 12.9 and 15, stages A5e-A5g): BMP, TGA, PNG and PSD now,
// DDS, TIFF, Radiance HDR and OpenEXR (A5f) and JPEG through libjpeg-turbo (A5g) later. Every decoder produces the same result
// (TextureSourceFormat pixels, top row first) and the same diagnostics (InvalidSource, UnsupportedSourceFormat, LossySource and the
// NonSrgbSource and IgnoredSourceChannels warnings). Developer module: the editor, cooker and tool programs link it, shipping runtime
// targets do not (no source format code at run time — section 12.1). Texture supplies TextureSourceFormat, AssetRuntime the
// diagnostic codes, and the private Compression dependency inflates PNG zlib streams.
public sealed class ImageDecoders : EngineModuleRules
{
    protected override string Group => "Developer";

    protected override string ModuleName => "ImageDecoders";

    // Unity build: this module passes the unity collision policy checks (Docs/Design/UnityBuildPch.ko.md section 9.3).
    protected override UnityMode Unity => UnityMode.On;

    protected override string[] PublicDependencies => ["Core", "Texture", "AssetRuntime"];

    protected override string[] PrivateDependencies => ["Compression"];
}
