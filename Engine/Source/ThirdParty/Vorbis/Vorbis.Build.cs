using Rocks.Build;
using Rocks.Build.Rules;

// libvorbis 1.3.7 with vorbisfile (Xiph.Org, BSD-3-Clause): Ogg Vorbis source decoder for the audio importer
// (Docs/Design/AudioSystem.ko.md section 3.28). Rocks.patch replaces deprecated Microsoft CRT calls and removes an unused
// accumulator. The ThirdParty.json in the same folder is the single source of sources, licenses, file hashes and traceable
// modifications.
public sealed class Vorbis : ThirdPartyModuleRules
{
    protected override string ModuleName => "Vorbis";

    // libvorbis and its public headers include ogg/ogg.h, so Ogg is a public dependency (consumers see it through Vorbis).
    public override NativeModuleInput Configure(BuildContext context)
    {
        NativeModuleInput input = base.Configure(context);
        return input with
        {
            PublicDependencies = ["Ogg"],
        };
    }
}
