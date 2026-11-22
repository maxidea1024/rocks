using Rocks.Build;
using Rocks.Build.Rules;

// Real-time audio mixer, DSP and device backend interface (Docs/Design/AudioSystem.ko.md). AU1 DSP1 (section 3.20) adds the
// buffer-level DSP: channel layouts, planar buffers, gain ramps, one-pole filters, the linear resampler, the azimuth panner and
// distance attenuation. Engine layer module that depends on Core only (no Object, World or RHI dependency), so DedicatedServer
// builds may link it.
public sealed class Audio : EngineModuleRules
{
    protected override string ModuleName => "Audio";

    // Unity build: this module passes the unity collision policy checks (Docs/Design/UnityBuildPch.ko.md section 9.3).
    protected override UnityMode Unity => UnityMode.On;

    protected override string[] PublicDependencies => ["Core"];
}
