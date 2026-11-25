using Rocks.Build.Rules;

// libFLAC 1.5.0 (xiph/flac, BSD-3-Clause): FLAC source decoder for the audio importer (Docs/Design/AudioSystem section 3.27).
// Only the stream decoder subset is imported; the encoder, Ogg FLAC and SIMD paths are not built. Rocks.patch replaces two
// deprecated Microsoft CRT calls. The ThirdParty.json in the same folder is the single source of sources, licenses, file hashes
// and traceable modifications.
public sealed class Flac : ThirdPartyModuleRules
{
    protected override string ModuleName => "Flac";
}
