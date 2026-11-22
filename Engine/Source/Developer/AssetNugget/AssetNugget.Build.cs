using Rocks.Build;
using Rocks.Build.Rules;

// Asset nugget (Docs/Design/AssetSystem.ko.md section 9 and 9.1 - the derived data cache in the Rockpile record contract): nugget keys
// (tagged SHA-256 writer, common header, SCC keys, reflected values), records and .rnug v2 files, the file system backend and the
// nugget cache with batched asynchronous requests. Developer module: editor and tools only (games read cooked data).
// ReflectionText: value walk for reflected settings. Serialization: record TLV. Compression: RCBK storage compression. Task: requests.
// DeterministicMath: the deterministic math version in every key.
public sealed class AssetNugget : EngineModuleRules
{
    protected override string Group => "Developer";

    protected override string ModuleName => "AssetNugget";

    protected override string[] PublicDependencies => ["Core", "Serialization", "ReflectionText", "Compression", "Task", "DeterministicMath"];
}
