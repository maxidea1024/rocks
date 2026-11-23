using Rocks.Build;
using Rocks.Build.Rules;

// opusfile 0.12 (Xiph.Org, BSD-3-Clause) without HTTP support: Ogg Opus file decoder for the audio importer
// (Docs/Design/AudioSystem.ko.md section 3.29). Rocks.patch only adds parentheses that clang asks for. The ThirdParty.json in the
// same folder is the single source of sources, licenses, file hashes and traceable modifications.
public sealed class OpusFile : ThirdPartyModuleRules
{
    protected override string ModuleName => "OpusFile";

    // opusfile.h includes ogg/ogg.h and opus_multistream.h, so the library itself also compiles against the Ogg and Opus headers.
    // Consumers take all three libraries.
    public override NativeModuleInput Configure(BuildContext context)
    {
        NativeModuleInput input = base.Configure(context);
        return input with
        {
            PrivateIncludeDirectories = [.. input.PrivateIncludeDirectories, .. ConsumerIncludeDirectories(context, "Ogg", "Opus")],
        };
    }
}
