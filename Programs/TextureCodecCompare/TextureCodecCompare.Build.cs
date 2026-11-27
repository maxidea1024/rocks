using Rocks.Build;
using Rocks.Build.Rules;

// Quality comparison of the Rocks BC6H/BC7 encoder with the UE 5.8 ISPC Texture Compressor DLL (Docs/Design/AssetSystem section
// 12.17.6 item 10, stage TC3c). It encodes the block test images with both, decodes both with the Rocks reference decoders and
// checks the PSNR criteria. --speed measures encoder and build speed (section 12.17.9 item 6, stage TC5b). IspcTextureCompressor provides the ispc_texcomp.h types the UE DLL functions take. The test image
// generator header lives with the TextureTools tests.
public sealed class TextureCodecCompareProgram : ModuleRules
{
    public override NativeModuleInput Configure(BuildContext context) => new()
    {
        Name = "TextureCodecCompare",
        ModuleDirectory = "Programs/TextureCodecCompare",
        Kind = NativeModuleKind.Executable,
        Sources = context.Sources("Programs/TextureCodecCompare/Private"),
        PrivateIncludeDirectories = [context.PathInRoot("Engine/Source/Developer/TextureTools/Tests")],
        PublicDependencies = ["Launch", "Core", "Task", "Texture", "TextureTools", "IspcTextureCompressor"]
    };
}
