#pragma once

#include <Core/Containers/Array.h>
#include <Core/Memory/SystemAllocator.h>
#include <Core/Text/NamePool.h>
#include <MaterialCompiler/MaterialDiagnosticCode.h>
#include <MaterialCompiler/MaterialTranslator.h>
#include <MaterialSchema/MaterialGraphSchema.h>
#include <MaterialSchema/MaterialNodeCatalog.h>
#include <MaterialSchema/MaterialPinValue.generated.h>
#include <MaterialSchema/MaterialPinValue.h>
#include <MaterialSchema/MaterialSchemaModule.h>
#include <MaterialSchema/Nodes/Constants/Constant2Properties.generated.h>
#include <MaterialSchema/Nodes/Constants/Constant2Properties.h>
#include <MaterialSchema/Nodes/Constants/Constant3Properties.generated.h>
#include <MaterialSchema/Nodes/Constants/Constant3Properties.h>
#include <MaterialSchema/Nodes/Constants/Constant4Properties.generated.h>
#include <MaterialSchema/Nodes/Constants/Constant4Properties.h>
#include <MaterialSchema/Nodes/Constants/ConstantDoubleProperties.generated.h>
#include <MaterialSchema/Nodes/Constants/ConstantDoubleProperties.h>
#include <MaterialSchema/Nodes/Constants/ConstantProperties.generated.h>
#include <MaterialSchema/Nodes/Constants/ConstantProperties.h>
#include <MaterialSchema/Nodes/Math/ComponentMaskProperties.generated.h>
#include <MaterialSchema/Nodes/Math/ComponentMaskProperties.h>
#include <MaterialSchema/Nodes/Math/ConvertProperties.generated.h>
#include <MaterialSchema/Nodes/Math/CosineProperties.generated.h>
#include <MaterialSchema/Nodes/Math/CosineProperties.h>
#include <MaterialSchema/Nodes/Math/ConvertProperties.h>
#include <MaterialSchema/Nodes/Math/InverseLinearInterpolateProperties.generated.h>
#include <MaterialSchema/Nodes/Math/InverseLinearInterpolateProperties.h>
#include <MaterialSchema/Nodes/Math/OperatorProperties.generated.h>
#include <MaterialSchema/Nodes/Math/OperatorProperties.h>
#include <MaterialSchema/Nodes/Math/SineProperties.generated.h>
#include <MaterialSchema/Nodes/Math/SineProperties.h>
#include <MaterialSchema/Nodes/Math/SwitchProperties.generated.h>
#include <MaterialSchema/Nodes/Math/SwitchProperties.h>
#include <MaterialSchema/Nodes/Parameters/ScalarParameterProperties.generated.h>
#include <MaterialSchema/Nodes/Parameters/ScalarParameterProperties.h>
#include <MaterialSchema/Nodes/Parameters/VectorParameterProperties.generated.h>
#include <MaterialSchema/Nodes/Parameters/VectorParameterProperties.h>
#include <MaterialSchema/Nodes/Utility/NamedRerouteUsageProperties.generated.h>
#include <MaterialSchema/Nodes/Utility/NamedRerouteUsageProperties.h>
#include <NodeGraph/GraphDocument.h>
#include <NodeGraph/NodeTypeRegistry.h>
#include <Object/ModuleRegistration.h>
#include <Object/ObjectRegistry.h>
#include <ObjectSerialization/StructRecord.h>
#include <Platform/Memory.h>
#include <TestSupport/Test.h>

// 머티리얼 번역 시험 환경이다(Docs/Design/MaterialSystem 6.8절): MaterialSchema 타입을 등록한
// Registry, NamePool, 노드 등록표와 엔진 노드를 등록한 카탈로그, 속성 Record를 쓰는 리플렉션 codec.
namespace material_graph_fixture {

using namespace rocks;
using namespace rocks::material;

inline constexpr ContextId TranslatorRegistryContext{0x4d415454524e534cull, 1};

// MaterialSchema 타입을 등록한 Registry, NamePool, 노드 등록표, 카탈로그다(멤버 선언 순서가 수명
// 순서).
struct TranslatorEnv {
    TranslatorEnv() noexcept
        : types(allocator), modules(types, allocator), names(allocator),
          namesReady(names.Initialize().IsOk()),
          objects(allocator, types, TranslatorRegistryContext, Options()),
          registry(allocator, names), catalog(allocator),
          schema(MaterialGraphKind::Material, catalog, names, allocator),
          functionSchema(MaterialGraphKind::Function, catalog, names, allocator) {
        // 함수 그래프 전용 노드(FunctionInput·FunctionOutput — 6.12절 MS2e2)도 카탈로그에
        // 넣는다(엔진은 두 도메인을 모두 등록한다).
        ready = namesReady && RegisterMaterialSchemaTypes(modules).IsOk() &&
                registry.Initialize().IsOk() && schema.Ready() && functionSchema.Ready() &&
                registry.RegisterSchema(schema, ModuleLease()).IsOk() &&
                registry.RegisterSchema(functionSchema, ModuleLease()).IsOk() &&
                catalog.RegisterGroups(registry, schema, BuiltinMaterialNodeGroups(), ModuleLease())
                    .IsOk() &&
                catalog
                    .RegisterGroups(registry, functionSchema, BuiltinMaterialNodeGroups(),
                                    ModuleLease())
                    .IsOk();
    }

    ~TranslatorEnv() {
        CHECK(objects.Shutdown(objects.OwnerAccess()));
        CHECK(modules.Unregister().IsOk());
    }

    RegistryOptions Options() noexcept {
        RegistryOptions options;
        options.names = &names;
        return options;
    }

    Name Intern(StringView text) noexcept {
        auto name = names.TryIntern(text);
        return name ? name.Value() : Name();
    }

    template <class S>
    Array<uint8> Save(const S& value) noexcept {
        auto lease = types.AcquireValueType(GeneratedType<S>::Id(), TypeKind::Struct);
        if (!lease) {
            return Array<uint8>(allocator);
        }

        auto saved =
            SaveStructRecord(allocator, objects, objects.OwnerAccess(), lease.Value(), &value);
        return saved ? Move(saved.Value()) : Array<uint8>(allocator);
    }

    SystemAllocator allocator;
    TypeRegistry types;
    ModuleRegistration modules;
    NamePool names;
    bool namesReady = false;
    ObjectRegistry objects;
    nodegraph::NodeTypeRegistry registry;
    MaterialNodeCatalog catalog;
    MaterialGraphSchema schema;
    MaterialGraphSchema functionSchema;
    bool ready = false;
};

} // namespace material_graph_fixture
