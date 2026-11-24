using Rocks.Build;
using Rocks.Build.Rules;

// HarfBuzz 15.2.0 (Old MIT license): text shaping for the font backend (Docs/Design/Font/08-ThirdParty.ko.md section 8.2). The
// ThirdParty.json in the same folder is the single source of sources, definitions, licenses, file hashes and traceable
// modifications.
public sealed class HarfBuzz : ThirdPartyModuleRules
{
    protected override string ModuleName => "HarfBuzz";

    // hb-ft.h includes the FreeType headers and hb-icu.h includes unicode/uscript.h, so both libraries are public dependencies.
    public override NativeModuleInput Configure(BuildContext context)
    {
        NativeModuleInput input = base.Configure(context);
        return input with
        {
            PublicDependencies = ["FreeType", "Icu"],
        };
    }
}
