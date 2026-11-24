using Rocks.Build.Rules;

// ICU4C 79.1 common library (Unicode-3.0): text boundaries and bidi for the font backend (Docs/Design/Font/08-ThirdParty.ko.md
// sections 8.2-8.3). The data subset is the separate data library IcuData. The ThirdParty.json in the same folder is the single
// source of sources, definitions, licenses and file hashes.
public sealed class Icu : ThirdPartyModuleRules
{
    protected override string ModuleName => "Icu";
}
