using Rocks.Build;
using Rocks.Build.Rules;

// Header module of the VulkanHeaders library (no sources: Docs/Design/BuildTargets section 3.5 BS3a). Consumers list it as a private dependency.
public sealed class VulkanHeaders : ThirdPartyModuleRules
{
    protected override string ModuleName => "VulkanHeaders";
}
