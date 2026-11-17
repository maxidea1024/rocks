using Rocks.Build.Rules;

// libsodium 1.0.22-stable pinned commit (ISC). The Crypto module's only external crypto library (Docs/Design/Crypto.ko.md section 5.1).
// The ThirdParty.json and Rocks.patch in the same folder are the single source of sources, definitions, licenses, file hashes and traceable modifications.
public sealed class Sodium : ThirdPartyModuleRules
{
    protected override string ModuleName => "Sodium";
}
