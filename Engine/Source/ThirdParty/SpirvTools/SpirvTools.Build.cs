using Rocks.Build;
using Rocks.Build.Rules;

// Header module of the SpirvTools library (no sources: Docs/Design/BuildTargets section 3.5 BS3a). Consumers list it as a private dependency.
public sealed class SpirvTools : ThirdPartyModuleRules
{
    protected override string ModuleName => "SpirvTools";
}
