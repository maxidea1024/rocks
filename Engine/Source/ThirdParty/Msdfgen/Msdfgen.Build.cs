using Rocks.Build;
using Rocks.Build.Rules;

// msdfgen 1.13.1 (MIT): signed distance field generation for the font backend (Docs/Design/Font/08-ThirdParty.ko.md section 8.2).
// The ThirdParty.json in the same folder is the single source of sources, definitions, licenses, file hashes and traceable
// modifications.
public sealed class Msdfgen : ThirdPartyModuleRules
{
    protected override string ModuleName => "Msdfgen";

    // ext/import-font.h declares functions over FreeType types, so FreeType is a public dependency.
    public override NativeModuleInput Configure(BuildContext context)
    {
        NativeModuleInput input = base.Configure(context);
        return input with
        {
            PublicDependencies = ["FreeType"],
        };
    }
}
