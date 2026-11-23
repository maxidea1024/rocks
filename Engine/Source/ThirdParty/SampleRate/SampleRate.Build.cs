using Rocks.Build.Rules;

// libsamplerate 0.2.2 (libsndfile project, BSD-2-Clause): band-limited resampler for audio cooking, the same library and
// converter (SRC_SINC_BEST_QUALITY) as UE's cook-time ResampleWaveData (Docs/Design/AudioSystem.ko.md section 3.31). The
// ThirdParty.json in the same folder is the single source of sources, licenses and file hashes.
public sealed class SampleRate : ThirdPartyModuleRules
{
    protected override string ModuleName => "SampleRate";
}
