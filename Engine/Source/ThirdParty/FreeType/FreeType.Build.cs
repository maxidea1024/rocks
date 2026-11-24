using Rocks.Build;
using Rocks.Build.Rules;

// FreeType 2.15.0 (FreeType License, chosen from FTL OR GPL-2.0-or-later): glyph loading, hinting and rasterization for the font
// backend (Docs/Design/Font/08-ThirdParty.ko.md section 8.2). The ThirdParty.json in the same folder is the single source of
// sources, licenses, file hashes and traceable modifications.
public sealed class FreeType : ThirdPartyModuleRules
{
    protected override string ModuleName => "FreeType";

    // The library sources include zlib.h, brotli/decode.h and png.h; the public FreeType headers include none of them, so the
    // three libraries are private dependencies.
    public override NativeModuleInput Configure(BuildContext context)
    {
        NativeModuleInput input = base.Configure(context);
        return input with
        {
            PrivateDependencies = ["Zlib", "Brotli", "LibPng"],
        };
    }
}
