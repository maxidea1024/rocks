#include <ShaderCore/ShaderTypeRegistry.h>

#include "ShaderCoreInternal.h"

#include <ModuleLoader/HostServices.h>
#include <ModuleLoader/ModuleStartContext.h>
#include <ShaderCore/ShaderVirtualPath.h>
#include <string.h>

// 셰이더 타입 등록부(Docs/Design/ShaderSystem 12.2.1절 SSD3)다. 규칙은 공개 헤더 주석을 따른다.
namespace rocks::shader {

namespace type_registry_detail {

constexpr StringView EntryExtension("rsf");
constexpr StringView IncludeExtension("rsh");
// 엔트리 함수 이름의 최대 byte 수(NUL 제외)다.
constexpr usize MaxShaderEntryPointBytes = 64;
// 단계 비트 전체다.
constexpr uint32 AllShaderStages = ShaderStageVertex | ShaderStagePixel | ShaderStageCompute;
// 소속 수다(이름 표 크기).
constexpr usize ShaderTypeOwnerCount = 4;

constexpr const char* OwnerNames[ShaderTypeOwnerCount] = {"Engine", "EnginePlugin", "Project",
                                                          "ProjectPlugin"};

// 한 비트만 켜졌는가.
constexpr bool IsSingleBit(uint32 value) noexcept {
    return value != 0 && (value & (value - 1)) == 0;
}

// 소속과 가상 루트가 맞는가.
bool OwnerMatchesRoot(ShaderTypeOwner owner, ShaderVirtualRoot root) noexcept {
    switch (owner) {
    case ShaderTypeOwner::Engine:
        return root == ShaderVirtualRoot::Engine;
    case ShaderTypeOwner::Project:
        return root == ShaderVirtualRoot::Project;
    case ShaderTypeOwner::EnginePlugin:
    case ShaderTypeOwner::ProjectPlugin:
        return root == ShaderVirtualRoot::Plugin;
    }

    return false;
}

// 경로가 <루트>/Generated/ 아래인가(생성 원문 자리 — 실제 파일이 아니다).
bool IsGeneratedPath(StringView path, const ShaderVirtualPathInfo& info) noexcept {
    usize rootBytes = 0;
    switch (info.root) {
    case ShaderVirtualRoot::Engine:
        rootBytes = sizeof(EngineShaderVirtualRoot) - 1;
        break;
    case ShaderVirtualRoot::Project:
        rootBytes = sizeof(ProjectShaderVirtualRoot) - 1;
        break;
    case ShaderVirtualRoot::Plugin:
        rootBytes = sizeof(PluginShaderVirtualPrefix) - 1 + info.pluginName.ByteCount();
        break;
    }

    const StringView generated(GeneratedShaderDirectoryName);
    const usize start = rootBytes + 1;
    return path.ByteCount() > start + generated.ByteCount() &&
           StringView(path.Data() + start, generated.ByteCount()) == generated &&
           path.Data()[start + generated.ByteCount()] == '/';
}

bool IsKnownOwner(ShaderTypeOwner owner) noexcept {
    return static_cast<usize>(owner) < ShaderTypeOwnerCount;
}

bool IsKnownHlsl2021(ShaderHlsl2021 value) noexcept {
    return value == ShaderHlsl2021::FormatDefault || value == ShaderHlsl2021::On ||
           value == ShaderHlsl2021::Off;
}

} // namespace type_registry_detail

const char* ShaderTypeOwnerName(ShaderTypeOwner owner) noexcept {
    using namespace type_registry_detail;
    return IsKnownOwner(owner) ? OwnerNames[static_cast<usize>(owner)] : "unknown";
}

ShaderTypeRegistry::ShaderTypeRegistry(Allocator& allocator) noexcept
    : types_(allocator), vertexInputs_(allocator) {
}

void ShaderTypeRegistry::Fail(ErrorCode code, StringView typeName, const char* reason) noexcept {
    failure_ = ShaderTypeRegistrationFailure{};
    failure_.code = code;
    failure_.reason = reason;
    const usize bytes = typeName.ByteCount() < MaxShaderTypeNameBytes ? typeName.ByteCount()
                                                                      : MaxShaderTypeNameBytes;
    if (bytes > 0) {
        memcpy(failure_.typeName, typeName.Data(), bytes);
    }

    failure_.typeName[bytes] = '\0';
}

bool ShaderTypeRegistry::Check(const ShaderTypeDesc* desc, Span<const ShaderTypeDesc* const> batch,
                               usize index) noexcept {
    using namespace type_registry_detail;
    if (desc == nullptr) {
        Fail(ErrorCode::InvalidArgument, StringView(), "shader type descriptor is null");
        return false;
    }

    const StringView name = desc->name;
    if (!core_detail::IsShaderIdentifier(name, MaxShaderTypeNameBytes)) {
        Fail(ErrorCode::InvalidArgument, name,
             "shader type name must be an ASCII identifier of at most 64 bytes");
        return false;
    }

    if (desc->nameHash != ShaderTypeNameHash(name)) {
        Fail(ErrorCode::InvalidArgument, name,
             "shader type name hash does not match ShaderTypeNameHash(name)");
        return false;
    }

    // 이미 등록된 타입과 같은 목록의 앞 타입을 함께 본다.
    const auto collides = [&](const ShaderTypeDesc* other) noexcept {
        if (other == nullptr || other->nameHash != desc->nameHash) {
            return false;
        }

        Fail(ErrorCode::InvalidArgument, name,
             other->name == name ? "shader type name is already registered"
                                 : "shader type name hash collides with another shader type "
                                   "(names that differ only in letter case also collide)");
        return true;
    };
    for (const ShaderTypeDesc* registered : types_) {
        if (collides(registered)) {
            return false;
        }
    }

    for (usize other = 0; other < index; ++other) {
        if (collides(batch[other])) {
            return false;
        }
    }

    ShaderVirtualPathInfo info;
    if (const ShaderVirtualPathCheck checked = CheckShaderVirtualPath(desc->sourcePath, info);
        !checked) {
        Fail(ErrorCode::InvalidArgument, name, checked.reason);
        return false;
    }

    if (info.extension != EntryExtension) {
        Fail(ErrorCode::InvalidArgument, name, "shader type source must be a .rsf entry file");
        return false;
    }

    if (!IsKnownOwner(desc->owner) || !OwnerMatchesRoot(desc->owner, info.root)) {
        Fail(ErrorCode::InvalidArgument, name,
             "shader type owner does not match the source root (Engine: /Engine, plugins: "
             "/Plugin/<name>, Project: /Project)");
        return false;
    }

    if (!core_detail::IsShaderIdentifier(desc->entryPoint, MaxShaderEntryPointBytes)) {
        Fail(ErrorCode::InvalidArgument, name, "shader type entry point must be an identifier");
        return false;
    }

    if (!IsSingleBit(desc->stage) || (desc->stage & ~AllShaderStages) != 0) {
        Fail(ErrorCode::InvalidArgument, name, "shader type stage must be exactly one stage bit");
        return false;
    }

    if ((desc->requiredFeatures & ~AllShaderFeatures) != 0 ||
        (desc->flags & ~AllShaderTypeFlags) != 0 || !IsKnownHlsl2021(desc->hlsl2021)) {
        Fail(ErrorCode::InvalidArgument, name,
             "shader type features, flags or HLSL 2021 choice are outside the vocabulary");
        return false;
    }

    if (!ValidateShaderPermutationDomain(desc->permutation)) {
        Fail(ErrorCode::InvalidArgument, name,
             "shader type permutation domain is invalid (names, values or more than 65536 "
             "permutations)");
        return false;
    }

    const char* layoutReason = "";
    if (!ValidateShaderParameterLayouts(desc->parameterLayouts, &layoutReason)) {
        Fail(ErrorCode::InvalidArgument, name, layoutReason);
        return false;
    }

    return true;
}

bool ShaderTypeRegistry::CheckVertexInput(const ShaderVertexInputType& input,
                                          Span<const ShaderVertexInputType> batch,
                                          usize index) noexcept {
    using namespace type_registry_detail;
    const StringView name = input.name;
    if (!core_detail::IsShaderIdentifier(name, MaxShaderTypeNameBytes)) {
        Fail(ErrorCode::InvalidArgument, name,
             "vertex input type name must be an ASCII identifier of at most 64 bytes");
        return false;
    }

    if (input.id == NoVertexInputType || input.id != ShaderTypeNameHash(name)) {
        Fail(ErrorCode::InvalidArgument, name,
             "vertex input type id does not match ShaderTypeNameHash(name)");
        return false;
    }

    const auto collides = [&](const ShaderVertexInputType& other) noexcept {
        if (other.id != input.id) {
            return false;
        }

        Fail(ErrorCode::InvalidArgument, name,
             other.name == name ? "vertex input type name is already registered"
                                : "vertex input type name hash collides with another vertex "
                                  "input type (names that differ only in letter case also "
                                  "collide)");
        return true;
    };
    for (const ShaderVertexInputType& registered : vertexInputs_) {
        if (collides(registered)) {
            return false;
        }
    }

    for (usize other = 0; other < index; ++other) {
        if (collides(batch[other])) {
            return false;
        }
    }

    ShaderVirtualPathInfo info;
    if (const ShaderVirtualPathCheck checked = CheckShaderVirtualPath(input.sourcePath, info);
        !checked) {
        Fail(ErrorCode::InvalidArgument, name, checked.reason);
        return false;
    }

    if (info.extension != IncludeExtension || info.shared) {
        Fail(ErrorCode::InvalidArgument, name,
             "vertex input type source must be a .rsh include file outside Shared");
        return false;
    }

    if (IsGeneratedPath(input.sourcePath, info)) {
        Fail(ErrorCode::InvalidArgument, name,
             "vertex input type source cannot be a Generated path (it is the include target)");
        return false;
    }

    if (!IsKnownOwner(input.owner) || !OwnerMatchesRoot(input.owner, info.root)) {
        Fail(ErrorCode::InvalidArgument, name,
             "vertex input type owner does not match the source root (Engine: /Engine, plugins: "
             "/Plugin/<name>, Project: /Project)");
        return false;
    }

    return true;
}

Result<void>
ShaderTypeRegistry::RegisterVertexInputTypes(Span<const ShaderVertexInputType> inputs) noexcept {
    if (frozen_) {
        Fail(ErrorCode::ScopeClosed, StringView(), "the shader type registry is frozen");
        return Result<void>::Failure(ErrorCode::ScopeClosed);
    }

    for (usize index = 0; index < inputs.Size(); ++index) {
        if (!CheckVertexInput(inputs[index], inputs, index)) {
            return Result<void>::Failure(failure_.code);
        }
    }

    if (!vertexInputs_.TryReserve(vertexInputs_.Size() + inputs.Size())) {
        Fail(ErrorCode::OutOfMemory, StringView(), "out of memory");
        return Result<void>::Failure(ErrorCode::OutOfMemory);
    }

    for (const ShaderVertexInputType& input : inputs) {
        (void)vertexInputs_.TryPushBack(input);
    }

    return Result<void>::Success();
}

void ShaderTypeRegistry::UnregisterVertexInputTypes(
    Span<const ShaderVertexInputType> inputs) noexcept {
    for (const ShaderVertexInputType& input : inputs) {
        for (usize index = 0; index < vertexInputs_.Size(); ++index) {
            if (vertexInputs_[index].id == input.id && vertexInputs_[index].name == input.name) {
                // 순서를 지킨다(Freeze 뒤 정렬이 깨지지 않게).
                (void)vertexInputs_.TryRemoveAt(index);
                break;
            }
        }
    }
}

const ShaderVertexInputType*
ShaderTypeRegistry::FindVertexInputType(VertexInputTypeId id) const noexcept {
    for (const ShaderVertexInputType& input : vertexInputs_) {
        if (input.id == id) {
            return &input;
        }
    }

    return nullptr;
}

Result<void> ShaderTypeRegistry::Register(Span<const ShaderTypeDesc* const> types) noexcept {
    if (frozen_) {
        Fail(ErrorCode::ScopeClosed, StringView(), "the shader type registry is frozen");
        return Result<void>::Failure(ErrorCode::ScopeClosed);
    }

    for (usize index = 0; index < types.Size(); ++index) {
        if (!Check(types[index], types, index)) {
            return Result<void>::Failure(failure_.code);
        }
    }

    if (!types_.TryReserve(types_.Size() + types.Size())) {
        Fail(ErrorCode::OutOfMemory, StringView(), "out of memory");
        return Result<void>::Failure(ErrorCode::OutOfMemory);
    }

    for (const ShaderTypeDesc* desc : types) {
        (void)types_.TryPushBack(desc);
    }

    return Result<void>::Success();
}

void ShaderTypeRegistry::Unregister(Span<const ShaderTypeDesc* const> types) noexcept {
    for (const ShaderTypeDesc* desc : types) {
        for (usize index = 0; index < types_.Size(); ++index) {
            if (types_[index] == desc) {
                // 순서를 지킨다(Freeze 뒤 정렬이 깨지지 않게).
                (void)types_.TryRemoveAt(index);
                break;
            }
        }
    }
}

Result<void> ShaderTypeRegistry::Freeze() noexcept {
    if (frozen_) {
        return Result<void>::Success();
    }

    types_.Sort([](const ShaderTypeDesc* left, const ShaderTypeDesc* right) noexcept {
        return left->nameHash < right->nameHash;
    });
    vertexInputs_.Sort(
        [](const ShaderVertexInputType& left, const ShaderVertexInputType& right) noexcept {
            return left.id < right.id;
        });
    frozen_ = true;
    return Result<void>::Success();
}

const ShaderTypeDesc* ShaderTypeRegistry::Find(uint64 nameHash) const noexcept {
    if (!frozen_) {
        for (const ShaderTypeDesc* desc : types_) {
            if (desc->nameHash == nameHash) {
                return desc;
            }
        }

        return nullptr;
    }

    usize low = 0;
    usize high = types_.Size();
    while (low < high) {
        const usize middle = low + (high - low) / 2;
        if (types_[middle]->nameHash < nameHash) {
            low = middle + 1;
        } else {
            high = middle;
        }
    }

    return low < types_.Size() && types_[low]->nameHash == nameHash ? types_[low] : nullptr;
}

Sha256Digest ShaderTypeRegistry::TypeSetDigest() const noexcept {
    // 해시를 little-endian 8 byte로 정렬 순서대로 잇는다(Freeze 전이면 등록 순서 — 의미 없음).
    Sha256 hasher;
    for (const ShaderTypeDesc* desc : types_) {
        uint8 bytes[sizeof(uint64)];
        for (usize index = 0; index < sizeof(uint64); ++index) {
            bytes[index] = static_cast<uint8>(desc->nameHash >> (index * 8));
        }

        hasher.Update(Span<const uint8>(bytes, sizeof(bytes)));
    }

    return hasher.Finish();
}

Result<void> ShaderTypeModuleRegistration::Startup(ModuleStartContext& context) noexcept {
    return Attach(context.Services(), context.Lease());
}

Result<void> ShaderTypeModuleRegistration::Attach(HostServices& services,
                                                  ModuleLease lease) noexcept {
    if (registry_ != nullptr) {
        return Result<void>::Failure(ErrorCode::InvalidArgument);
    }

    ShaderTypeRegistry* registry = services.Find<ShaderTypeRegistry>();
    if (registry == nullptr) {
        return Result<void>::Success();
    }

    if (auto registered = registry->Register(types_); !registered) {
        return registered;
    }

    if (auto registered = registry->RegisterVertexInputTypes(vertexInputs_); !registered) {
        registry->Unregister(types_);
        return registered;
    }

    registry_ = registry;
    lease_ = Move(lease);
    return Result<void>::Success();
}

void ShaderTypeModuleRegistration::Shutdown() noexcept {
    if (registry_ == nullptr) {
        return;
    }

    registry_->UnregisterVertexInputTypes(vertexInputs_);
    registry_->Unregister(types_);
    registry_ = nullptr;
    lease_.Reset();
}

} // namespace rocks::shader

ROCKS_DEFINE_HOST_SERVICE(rocks::shader::ShaderTypeRegistry)
