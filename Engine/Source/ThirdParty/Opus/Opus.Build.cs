using Rocks.Build;
using Rocks.Build.Rules;
using Rocks.Tooling.Common;

// libopus 1.6.1 (Xiph.Org and contributors, BSD-3-Clause), float build without SIMD or runtime CPU detection: Opus codec for the
// Ogg Opus source decoder of the audio importer (Docs/Design/AudioSystem section 3.29), the Opus platform data encoder
// (AudioTools) and the runtime Opus decoder module AudioOpus (section 3.32). The ThirdParty.json in the same folder is the single
// source of sources, licenses and file hashes.
public sealed class Opus : ThirdPartyModuleRules
{
    // Library-internal header directories. They are added to the library build only, so that headers with common names (debug.h,
    // errors.h, main.h) do not reach consuming modules through the manifest includeDirectories.
    private static readonly string[] InternalIncludeDirectories = ["celt", "silk", "silk/float", "src"];

    protected override string ModuleName => "Opus";

    public override NativeModuleInput Configure(BuildContext context)
    {
        NativeModuleInput input = base.Configure(context);
        string library = LibraryDirectory(context, ModuleName);
        string upstream = $"{library}/{ThirdPartyManifest.Read(context.PathInRoot(library)).UpstreamDirectory}";
        return input with
        {
            PrivateIncludeDirectories =
            [
                .. input.PrivateIncludeDirectories,
                .. InternalIncludeDirectories.Select(directory => context.PathInRoot($"{upstream}/{directory}")),
            ],
        };
    }
}
