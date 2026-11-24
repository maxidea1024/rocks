using Rocks.Build.Rules;

// Jolt Physics 5.7.0 (Jorrit Rouwe, MIT): rigid body physics middleware for the Physics module (Docs/Design/PhysicsJoltAdapter.ko.md
// section 10, JL1). Cross-platform deterministic, single precision build with asserts in every configuration. The ThirdParty.json in
// the same folder is the single source of sources, definitions, licenses and file hashes.
public sealed class Jolt : ThirdPartyModuleRules
{
    protected override string ModuleName => "Jolt";
}
