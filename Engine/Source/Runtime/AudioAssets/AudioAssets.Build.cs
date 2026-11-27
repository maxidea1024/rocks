using Rocks.Build;
using Rocks.Build.Rules;

// Sound asset classes of the audio system (Docs/Design/AudioSystem sections 3.9 and 3.36.1): SoundSource, SoundClip and AttenuationSettings,
// the immutable playback snapshot types (SoundPcm, SoundSourceRef) and the import option structs. Split out of EngineAudio so asset command
// sessions register the sound types without World or Engine (the same reason Texture is its own module). Runtime module: servers link it.
// Generated headers are <AudioAssets/*.generated.h>, module registration is rocks::generated::AudioAssets::RegisterModuleTypes (called by
// RegisterAudioAssetsTypes), and the identity spec is AudioAssets.rocksids.json.
public sealed class AudioAssets : EngineModuleRules
{
    protected override string ModuleName => "AudioAssets";

    // Unity build: this module passes the unity collision policy checks (Docs/Design/UnityBuildPch.ko.md section 9.3).
    protected override UnityMode Unity => UnityMode.On;

    // AssetRuntime: the Asset base class and bulk data references. Audio: mixer limits, distance attenuation settings.
    // CoreTypeBindings: reflected Vec3 (shape extents). Curves: TangentCurve in the attenuation snapshot. CurveAssets:
    // FloatCurveSource fields of AttenuationSettings (section 3.42.1).
    protected override string[] PublicDependencies => ["AssetRuntime", "Audio", "CoreTypeBindings", "Curves", "CurveAssets"];
}
