using Rocks.Build;
using Rocks.Build.Rules;

// libtiff 4.7.2 (libtiff license): TIFF source decoder for the texture importer (Docs/Design/AssetSystem section 12.10), the same
// library UE 5.8 uses. The lossless codecs are built (CCITT, LZW, NeXT, PackBits, ThunderScan and ZIP through the Rocks zlib);
// tif_rocks.c replaces the platform files with a per-thread allocator binding and no file I/O. The ThirdParty.json in the same
// folder is the single source of sources, licenses, file hashes and traceable modifications.
public sealed class LibTiff : ThirdPartyModuleRules
{
    protected override string ModuleName => "LibTiff";

    // tif_zip.c includes zlib.h, so the library compiles against the Zlib headers and defines (Z_PREFIX, Z_SOLO). Consumers take
    // LibTiff before Zlib.
    public override NativeModuleInput Configure(BuildContext context)
    {
        NativeModuleInput input = base.Configure(context);
        return input with
        {
            PrivateIncludeDirectories = [.. input.PrivateIncludeDirectories, .. ConsumerIncludeDirectories(context, "Zlib")],
            PrivateDefines = [.. input.PrivateDefines, .. ConsumerDefines(context, "Zlib")],
        };
    }
}
