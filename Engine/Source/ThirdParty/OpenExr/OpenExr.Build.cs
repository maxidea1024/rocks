using Rocks.Build;
using Rocks.Build.Rules;

// OpenEXRCore 3.4.3 (AcademySoftwareFoundation/openexr, BSD-3-Clause; internal libdeflate 1.24, MIT): the C decoder of OpenEXR for
// the texture source importer (Docs/Design/AssetSystem section 12.10), the same library and version as UE 5.8. Only the C core and
// the internal libdeflate copy are imported; the C++ library, Imath and HTJ2K (OpenJPH, C++ exceptions) are not. The Config folder
// holds the hand-written CMake configuration headers and OpenExrHtStub.c reports HTJ2K as not implemented. The ThirdParty.json in
// the same folder is the single source of sources, licenses, file hashes and traceable modifications.
public sealed class OpenExr : ThirdPartyModuleRules
{
    protected override string ModuleName => "OpenExr";
}
