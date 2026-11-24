using Rocks.Build;
using Rocks.Build.Rules;

// Texture source image codec (Docs/Design/AssetSystem sections 12.2, 12.9 and 15, stages A5e-A5g). Decoders: BMP, TGA, PNG and PSD now,
// DDS, TIFF, Radiance HDR and OpenEXR (A5f) and JPEG through libjpeg-turbo (A5g) later. Every decoder produces the same result
// (TextureSourceFormat pixels, top row first) and the same diagnostics (InvalidSource, UnsupportedSourceFormat, LossySource and the
// NonSrgbSource and IgnoredSourceChannels warnings). Developer module: the editor, cooker and tool programs link it, shipping runtime
// targets do not (no source format code at run time — section 12.1). Texture supplies TextureSourceFormat, AssetRuntime the
// diagnostic codes, the private Compression dependency inflates PNG zlib streams, and OpenExr and LibTiff decode EXR and TIFF. Encoders for texture export (PNG and the like)
// join this module later (UE ImageWrapper also covers both directions).
public sealed class ImageCodec : EngineModuleRules
{
    protected override string Group => "Developer";

    protected override string ModuleName => "ImageCodec";

    // Unity build: this module passes the unity collision policy checks (Docs/Design/UnityBuildPch.ko.md section 9.3).
    protected override UnityMode Unity => UnityMode.On;

    protected override string[] PublicDependencies => ["Core", "Texture", "AssetRuntime"];

    // OpenEXR (OpenEXRCore) and libtiff decode EXR and TIFF sources (section 12.10); their headers stay private to this module.
    // LibTiff comes before Zlib so that a left-to-right static linker resolves its zlib references.
    protected override string[] PrivateDependencies => ["Compression", "OpenExr", "LibTiff", "Zlib"];

    protected override IReadOnlyList<string> PrivateIncludeDirectories(BuildContext context) =>
        ThirdPartyModuleRules.ConsumerIncludeDirectories(context, "OpenExr", "LibTiff");

    protected override IReadOnlyList<string> PrivateDefines(BuildContext context) =>
        ThirdPartyModuleRules.ConsumerDefines(context, "OpenExr", "LibTiff");
}
