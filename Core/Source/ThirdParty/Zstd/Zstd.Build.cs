using Rocks.Build.Rules;

// Zstandard 1.5.7 pinned version (BSD-3-Clause chosen; the dictionary builder's divsufsort is MIT). Contains zstd frame compression and decompression
// and dictionary training (dictBuilder, tool only) (single threaded, no legacy formats).
// The ThirdParty.json in the same folder is the single source of sources, definitions, licenses and file hashes (Docs/Design/Compression.ko.md section 3).
public sealed class Zstd : ThirdPartyModuleRules
{
    protected override string ModuleName => "Zstd";
}
