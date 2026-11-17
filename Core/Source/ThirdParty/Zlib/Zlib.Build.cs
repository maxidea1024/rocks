using Rocks.Build.Rules;

// zlib 1.4 pinned version (Zlib). Deflate, Zlib and Gzip compression and decompression (Z_SOLO: no default allocator, no gz file IO).
// The ThirdParty.json in the same folder is the single source of sources, definitions, licenses and file hashes (Docs/Design/Compression.ko.md section 3).
public sealed class Zlib : ThirdPartyModuleRules
{
    protected override string ModuleName => "Zlib";
}
