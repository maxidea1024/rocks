#include <Core/Module/ModuleEntry.h>

// Entry point of the SampleGame module (ROCKS_IMPLEMENT_MODULE, Docs/Design/ModuleLoader.ko.md
// section 3.2). Every C++ module implements one; a module with startup work derives from
// rocks::ModuleImplementation.
ROCKS_IMPLEMENT_MODULE(rocks::DefaultModuleImplementation, SampleGame)
