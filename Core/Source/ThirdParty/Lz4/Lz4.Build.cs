using Rocks.Build.Rules;

// LZ4 1.11.1 pinned version (BSD-2-Clause). LZ4 raw block and LZ4 frame (lz4frame, xxHash) compression and decompression. lz4.c and lz4hc.c are built
// as one translation unit through Lz4Unity.c; lz4frame.c and xxhash.c are built separately.
// The ThirdParty.json in the same folder is the single source of sources, definitions, licenses and file hashes (Docs/Design/Compression.ko.md section 3).
public sealed class Lz4 : ThirdPartyModuleRules
{
    protected override string ModuleName => "Lz4";
}
