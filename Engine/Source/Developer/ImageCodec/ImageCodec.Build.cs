using Rocks.Build;
using Rocks.Build.Rules;

// Texture source image codec (Docs/Design/AssetSystem sections 12.2 and 12.9-12.11, stages A5e-A5g). Decoders: BMP, TGA, PNG, PSD, DDS
// and Radiance HDR (own code), TIFF (libtiff), OpenEXR (OpenEXRCore) and JPEG (libjpeg-turbo). Every decoder produces the same result
// (TextureSourceFormat pixels, top row first) and the same diagnostics (InvalidSource, UnsupportedSourceFormat, LossySource and the
// NonSrgbSource, IgnoredSourceChannels, IgnoredSourceMips and IgnoredSourceImages warnings). Developer module: the editor, cooker and
// tool programs link it, shipping runtime targets do not (no source format code at run time - section 12.1). Texture supplies
// TextureSourceFormat, AssetRuntime the diagnostic codes, and the private Compression dependency inflates PNG zlib streams. Encoders
// for texture export (PNG and the like) join this module later (UE ImageWrapper also covers both directions).
public sealed class ImageCodec : EngineModuleRules
{
    protected override string Group => "Developer";

    protected override string ModuleName => "ImageCodec";

    // Unity build: this module passes the unity collision policy checks (Docs/Design/UnityBuildPch.ko.md section 9.3).
    protected override UnityMode Unity => UnityMode.On;

    protected override string[] PublicDependencies => ["Core", "Texture", "AssetRuntime"];

    // OpenEXR (OpenEXRCore), libtiff and libjpeg-turbo decode EXR, TIFF and JPEG sources (sections 12.10 and 12.11); as private
    // dependencies their headers and consumer defines stay private to this module (BuildTargets BS3a). LibTiff comes before LibJpegTurbo and Zlib so that a left-to-right static linker resolves its
    // references.
    protected override string[] PrivateDependencies => ["Compression", "OpenExr", "LibTiff", "LibJpegTurbo", "Zlib"];
}
