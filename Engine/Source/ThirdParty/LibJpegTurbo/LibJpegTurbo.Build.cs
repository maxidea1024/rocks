using Rocks.Build;
using Rocks.Build.Rules;

// libjpeg-turbo 3.2.1 (IJG, BSD-3-Clause, zlib licenses): JPEG decoder for the texture importer and the JPEG codec of libtiff
// (Docs/Design/AssetSystem section 12.11). The libjpeg API library is built without SIMD (no NASM); jmemrocks.c binds
// allocation to the caller and rocksjpeg.c keeps libjpeg's longjmp inside C. The ThirdParty.json in the same folder is the single
// source of sources, licenses, file hashes and traceable modifications.
public sealed class LibJpegTurbo : ThirdPartyModuleRules
{
    protected override string ModuleName => "LibJpegTurbo";
}
