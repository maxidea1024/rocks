using Rocks.Build;
using Rocks.Build.Rules;

// libtiff 4.7.2 (libtiff license): TIFF source decoder for the texture importer (Docs/Design/AssetSystem section 12.10), the same
// library UE 5.8 uses. The lossless codecs are built (CCITT, LZW, NeXT, PackBits, ThunderScan and ZIP through the Rocks zlib) and
// JPEG and old JPEG through the Rocks libjpeg-turbo (section 12.11);
// tif_rocks.c replaces the platform files with a per-thread allocator binding and no file I/O. The ThirdParty.json in the same
// folder is the single source of sources, licenses, file hashes and traceable modifications.
public sealed class LibTiff : ThirdPartyModuleRules
{
    protected override string ModuleName => "LibTiff";

    // tif_zip.c includes zlib.h and tif_jpeg.c and tif_ojpeg.c include jpeglib.h (library internal; tiffio.h includes neither), so
    // Zlib and LibJpegTurbo are private dependencies (their headers and consumer defines such as Z_PREFIX and Z_SOLO reach the library).
    public override NativeModuleInput Configure(BuildContext context)
    {
        NativeModuleInput input = base.Configure(context);
        return input with
        {
            PrivateDependencies = ["Zlib", "LibJpegTurbo"],
        };
    }
}
