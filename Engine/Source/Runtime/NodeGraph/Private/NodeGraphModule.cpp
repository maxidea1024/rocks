#include <Core/Module/ModuleEntry.h>
#include <NodeGraph/GraphDocument.generated.h>
#include <NodeGraph/NodeGraph.module.generated.h>
#include <NodeGraph/NodeGraphModule.h>

// NodeGraph 모듈의 진입점이다(Docs/Design/ModuleLoader.ko.md 3.2절). 시작할 일이 없다.
ROCKS_IMPLEMENT_MODULE(rocks::DefaultModuleImplementation, NodeGraph)

namespace rocks::nodegraph {

Result<void> RegisterNodeGraphTypes(ModuleRegistration& registration) noexcept {
    return generated::NodeGraph::RegisterModuleTypes(registration);
}

} // namespace rocks::nodegraph
