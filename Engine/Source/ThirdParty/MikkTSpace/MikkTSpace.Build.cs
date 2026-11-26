using Rocks.Build.Rules;

// MikkTSpace (mmikk/MikkTSpace commit 3e895b49d05e, zlib license): reference tangent space generator for the static mesh build
// (Docs/Design/MeshAnimationAssets section 5.2, decision MA-D5). Rocks.patch routes cos/acos through Rocks deterministic math and
// malloc/free through the caller's allocator; MeshTools defines those hooks. MikkTSpaceReference.c rebuilds the unmodified behaviour under
// renamed entry points for comparison tests. The ThirdParty.json in the same folder is the single source of sources, licenses, file hashes
// and traceable modifications.
public sealed class MikkTSpace : ThirdPartyModuleRules
{
    protected override string ModuleName => "MikkTSpace";
}
