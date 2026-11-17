using Rocks.Build.Rules;

// Brotli 1.1.1 pinned version (MIT). Builds only the decoder (c/common and c/dec); the encoder and tools are not brought in
// (counterpart of Godot Compression's Brotli decompression, for WOFF2 fonts and the like). The ThirdParty.json in the same folder is the single source
// of sources, definitions, licenses and file hashes (Docs/Design/Compression.ko.md sections 3 and 13).
public sealed class Brotli : ThirdPartyModuleRules
{
    protected override string ModuleName => "Brotli";
}
