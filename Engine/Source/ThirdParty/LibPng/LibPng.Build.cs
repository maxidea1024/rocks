using Rocks.Build;
using Rocks.Build.Rules;

// libpng 1.6.59 (PNG Reference Library License version 2): PNG decoder for FreeType colour bitmap glyphs in the font backend
// (Docs/Design/Font/08-ThirdParty.ko.md section 8.1). The ThirdParty.json in the same folder is the single source of sources,
// licenses, file hashes and traceable modifications.
public sealed class LibPng : ThirdPartyModuleRules
{
    protected override string ModuleName => "LibPng";

    // png.h does not include zlib.h, but pngstruct.h (library internal) does, so zlib is a private dependency.
    public override NativeModuleInput Configure(BuildContext context)
    {
        NativeModuleInput input = base.Configure(context);
        return input with
        {
            PrivateDependencies = ["Zlib"],
        };
    }
}
