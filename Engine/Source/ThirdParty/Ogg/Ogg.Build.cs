using Rocks.Build.Rules;

// libogg 1.3.6 (Xiph.Org, BSD-3-Clause): Ogg container for the Vorbis and Opus source decoders of the audio importer
// (Docs/Design/AudioSystem.ko.md section 3.28). The ThirdParty.json in the same folder is the single source of sources, licenses
// and file hashes.
public sealed class Ogg : ThirdPartyModuleRules
{
    protected override string ModuleName => "Ogg";
}
