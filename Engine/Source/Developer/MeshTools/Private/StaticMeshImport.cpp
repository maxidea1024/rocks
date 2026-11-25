#include <MeshTools/StaticMeshImport.h>

#include <AssetRuntime/MemoryBulkSource.h>
#include <Core/Text/NamePool.h>
#include <Material/Material.generated.h>
#include <Mesh/StaticMesh/StaticMesh.generated.h>
#include <MeshSource/MeshSourceValidation.h>
#include <Object/ObjectRegistry.h>

// 정적 메시 임포트 서비스다(Docs/Design/MeshAnimationAssets 5.5.3절 — MA2b3).
namespace rocks {
namespace {

// 참조 위치 번호(TypeMismatch detail)다: 슬롯 i의 material 2i·overlay 2i+1, 그 뒤 메시 참조 둘.
constexpr uint64 ReferencesPerSlot = 2;

// 검증을 마친 이름들이다(Borrow 밖에서 intern).
struct InternedNames {
    explicit InternedNames(Allocator& allocator) noexcept
        : slots(allocator), imported(allocator), sockets(allocator), screens(allocator) {
    }

    Array<Name> slots;
    Array<Name> imported;
    Array<Name> sockets;
    // LOD마다 화면 크기 값이다(PerPlatformFloat 모양).
    Array<PerPlatformFloat> screens;
    Name lodGroup;
};

// 열린 참조 대상이다(null id면 비움).
struct OpenedReferences {
    explicit OpenedReferences(Allocator& allocator) noexcept
        : materials(allocator), overlays(allocator) {
    }

    Array<ObjectHandle> materials;
    Array<ObjectHandle> overlays;
    ObjectHandle complexCollisionMesh{};
    ObjectHandle distanceFieldReplacementMesh{};
};

// 재임포트에서 보존할 옛 슬롯 값이다(5.5.3절 6).
struct PreservedSlot {
    Name importedSlotName;
    Name slotName;
    ObjectHandle material{};
    ObjectHandle overlayMaterial{};
    MeshUvDensity uvDensity;
};

Result<void> Invalid(AssetImportContext& context, uint64 detail,
                     ErrorCode error = ErrorCode::InvalidArgument) noexcept {
    return context.Fail(AssetDiagnosticCode::InvalidSetting, error, detail);
}

// text를 intern한다(실패는 InvalidSetting과 원래 오류 — 이름 규칙·pool 상한·종료).
Result<Name> InternChecked(AssetImportContext& context, NamePool& names, StringView text,
                           uint64 detail) noexcept {
    auto name = names.TryIntern(text);
    if (!name) {
        (void)Invalid(context, detail, name.Error());
    }

    return name;
}

// 프로파일 표를 Name 키로 만든다(빈 키·Name 비교로 겹치는 키는 InvalidSetting).
template <class Entry, class Map>
Result<void> InternProfiles(AssetImportContext& context, NamePool& names, Span<const Entry> values,
                            Map& out, uint64 detail) noexcept {
    out.Clear();
    for (const Entry& value : values) {
        if (value.profile.IsEmpty()) {
            return Invalid(context, detail);
        }

        auto name = InternChecked(context, names, value.profile, detail);
        if (!name) {
            return Result<void>::Failure(name.Error());
        }

        if (out.Contains(name.Value())) {
            return Invalid(context, detail);
        }

        if (auto inserted = out.Insert(name.Value(), value.value); !inserted) {
            return context.Fail(AssetDiagnosticCode::None, inserted.Error());
        }
    }

    return Result<void>::Success();
}

// id의 대표 객체를 열고 그 타입이 expected 파생인지 본다(null id는 비움).
Result<ObjectHandle> OpenTyped(AssetImportContext& context, AssetId id, TypeId expected,
                               uint64 position) noexcept {
    if (id.IsNull()) {
        return Result<ObjectHandle>::Success(ObjectHandle{});
    }

    auto opened = context.OpenAsset(id);
    if (!opened) {
        return opened;
    }

    auto type = context.Registry().AcquireType(context.Access(), opened.Value());
    if (!type) {
        (void)context.Fail(AssetDiagnosticCode::None, type.Error(), position);
        return Result<ObjectHandle>::Failure(type.Error());
    }

    if (!type.Value().IsA(expected)) {
        (void)context.Fail(AssetDiagnosticCode::TypeMismatch, ErrorCode::TypeMismatch, position);
        return Result<ObjectHandle>::Failure(ErrorCode::TypeMismatch);
    }

    return opened;
}

// MemoryBulkSource의 ref를 출력 패키지 bulk로 옮긴다(byte를 복사해 AddBulk에 넘긴다).
Result<void> Transfer(AssetImportContext& context, ObjectHandle output, MemoryBulkSource& bulk,
                      BulkDataRef& ref, Allocator& allocator) noexcept {
    auto bytes = bulk.ReadBulk(ref);
    if (!bytes) {
        return Result<void>::Failure(bytes.Error());
    }

    Array<uint8> copy(allocator);
    if (auto copied = copy.TryAppend(bytes.Value()); !copied) {
        return copied;
    }

    auto added = context.AddBulk(output, Move(copy));
    if (!added) {
        return Result<void>::Failure(added.Error());
    }

    ref = added.Value();
    return Result<void>::Success();
}

Result<void> StoreMesh(AssetImportContext& context, ObjectHandle output, NamePool& names,
                       const MeshSource& mesh, MeshSourceData& data,
                       Allocator& allocator) noexcept {
    MemoryBulkSource bulk(allocator);
    if (auto written = mesh.Store(bulk, names, data); !written) {
        return written;
    }

    if (auto moved = Transfer(context, output, bulk, data.topology, allocator); !moved) {
        return moved;
    }

    for (MeshAttributeRecord& record : data.attributes) {
        if (auto moved = Transfer(context, output, bulk, record.data, allocator); !moved) {
            return moved;
        }
    }

    return Result<void>::Success();
}

// 1단계 검증(5.5.3절 3 — LOD·원본·섹션·슬롯). 메시 데이터 진단은 meshFailure에도 둔다.
Result<void> ValidateMeshes(AssetImportContext& context, const StaticMeshImportSpec& spec,
                            MeshDiagnostic* meshFailure) noexcept {
    if (spec.lods.IsEmpty() || spec.lods.Size() > StaticMeshMaxLods) {
        return Invalid(context, spec.lods.Size());
    }

    for (usize index = 0; index < spec.lods.Size(); ++index) {
        if (spec.lods[index].source == nullptr && spec.lods[index].bakedSource == nullptr) {
            return Invalid(context, index);
        }
    }

    for (usize index = 0; index < spec.lods.Size(); ++index) {
        const MeshSource* meshes[] = {spec.lods[index].source, spec.lods[index].bakedSource};
        for (const MeshSource* mesh : meshes) {
            if (mesh == nullptr) {
                continue;
            }

            MeshDiagnostic failure;
            if (auto valid = ValidateMeshSource(*mesh, {}, &failure); !valid) {
                if (meshFailure != nullptr) {
                    *meshFailure = failure;
                }

                return context.Fail(failure.code, valid.Error(), index);
            }
        }
    }

    for (usize index = 0; index < spec.sectionInfo.Size(); ++index) {
        const MeshSectionInfoEntry& entry = spec.sectionInfo[index];
        const MeshSectionInfoEntry* previous = index == 0 ? nullptr : &spec.sectionInfo[index - 1];
        const bool ordered = previous == nullptr || entry.lod > previous->lod ||
                             (entry.lod == previous->lod && entry.section > previous->section);
        if (entry.lod < 0 || static_cast<usize>(entry.lod) >= spec.lods.Size() ||
            entry.section < 0 || !ordered) {
            return Invalid(context, index);
        }
    }

    if (spec.slots.Size() > StaticMeshMaxMaterialSlots) {
        return Invalid(context, spec.slots.Size());
    }

    return Result<void>::Success();
}

// 이름 검증과 intern(5.5.3절 3 — 슬롯·소켓·LOD 그룹·프로파일 키).
Result<void> InternNames(AssetImportContext& context, NamePool& names,
                         const StaticMeshImportSpec& spec, Allocator& allocator,
                         PerPlatformInt32& minLod, PerPlatformInt32& numStreamed,
                         InternedNames& out) noexcept {
    for (usize index = 0; index < spec.slots.Size(); ++index) {
        auto name = InternChecked(context, names, spec.slots[index].slotName, index);
        if (!name) {
            return Result<void>::Failure(name.Error());
        }

        auto imported = InternChecked(context, names, spec.slots[index].importedSlotName, index);
        if (!imported) {
            return Result<void>::Failure(imported.Error());
        }

        if (!out.slots.TryPushBack(name.Value()) || !out.imported.TryPushBack(imported.Value())) {
            return context.Fail(AssetDiagnosticCode::None, ErrorCode::OutOfMemory);
        }
    }

    for (usize index = 0; index < spec.sockets.Size(); ++index) {
        auto name = InternChecked(context, names, spec.sockets[index].name, index);
        if (!name) {
            return Result<void>::Failure(name.Error());
        }

        if (!out.sockets.TryPushBack(name.Value())) {
            return context.Fail(AssetDiagnosticCode::None, ErrorCode::OutOfMemory);
        }
    }

    auto group = InternChecked(context, names, spec.lodGroup, 0);
    if (!group) {
        return Result<void>::Failure(group.Error());
    }

    out.lodGroup = group.Value();
    for (usize index = 0; index < spec.lods.Size(); ++index) {
        PerPlatformFloat screen(allocator);
        screen.defaultValue = spec.lods[index].screenSize;
        if (auto made = InternProfiles(context, names, spec.lods[index].screenSizePerPlatform,
                                       screen.perPlatform, index);
            !made) {
            return made;
        }

        if (!out.screens.TryPushBack(Move(screen))) {
            return context.Fail(AssetDiagnosticCode::None, ErrorCode::OutOfMemory);
        }
    }

    if (auto made = InternProfiles(context, names, spec.minLodPerPlatform, minLod.perPlatform, 0);
        !made) {
        return made;
    }

    return InternProfiles(context, names, spec.numStreamedLodsPerPlatform, numStreamed.perPlatform,
                          0);
}

// 단순 충돌 검증과 사본(볼록 형상 bulk는 아직 넘기지 않는다).
Result<void> ValidateCollision(AssetImportContext& context, const StaticMeshImportSpec& spec,
                               SimpleCollision& out) noexcept {
    const usize convexCount = spec.collision != nullptr ? spec.collision->convexes.Size() : 0;
    if (spec.convexes.Size() != convexCount) {
        return context.Fail(AssetDiagnosticCode::InvalidCollisionData, ErrorCode::InvalidArgument,
                            spec.convexes.Size());
    }

    if (spec.collision == nullptr) {
        return Result<void>::Success();
    }

    const SimpleCollision& source = *spec.collision;
    if (!out.spheres.TryAppend(source.spheres.View()) ||
        !out.boxes.TryAppend(source.boxes.View()) ||
        !out.capsules.TryAppend(source.capsules.View()) ||
        !out.taperedCapsules.TryAppend(source.taperedCapsules.View()) ||
        !out.cylinders.TryAppend(source.cylinders.View()) ||
        !out.convexes.TryAppend(source.convexes.View())) {
        return context.Fail(AssetDiagnosticCode::None, ErrorCode::OutOfMemory);
    }

    CollisionDiagnostic failure;
    if (auto checked = ValidateSimpleCollision(out, &failure); !checked) {
        return context.Fail(failure.code, checked.Error(), failure.index);
    }

    for (usize index = 0; index < spec.convexes.Size(); ++index) {
        if (auto checked = ValidateConvexGeometry(spec.convexes[index], &failure); !checked) {
            return context.Fail(failure.code, checked.Error(), index);
        }
    }

    return Result<void>::Success();
}

Result<void> OpenReferences(AssetImportContext& context, const StaticMeshImportSpec& spec,
                            OpenedReferences& out) noexcept {
    for (usize index = 0; index < spec.slots.Size(); ++index) {
        const uint64 position = index * ReferencesPerSlot;
        auto material =
            OpenTyped(context, spec.slots[index].material, TypeIdOf<MaterialBase>(), position);
        if (!material) {
            return Result<void>::Failure(material.Error());
        }

        auto overlay = OpenTyped(context, spec.slots[index].overlayMaterial,
                                 TypeIdOf<MaterialBase>(), position + 1);
        if (!overlay) {
            return Result<void>::Failure(overlay.Error());
        }

        if (!out.materials.TryPushBack(material.Value()) ||
            !out.overlays.TryPushBack(overlay.Value())) {
            return context.Fail(AssetDiagnosticCode::None, ErrorCode::OutOfMemory);
        }
    }

    const uint64 meshes = spec.slots.Size() * ReferencesPerSlot;
    auto complex = OpenTyped(context, spec.complexCollisionMesh, TypeIdOf<StaticMesh>(), meshes);
    if (!complex) {
        return Result<void>::Failure(complex.Error());
    }

    auto replacement =
        OpenTyped(context, spec.distanceFieldReplacementMesh, TypeIdOf<StaticMesh>(), meshes + 1);
    if (!replacement) {
        return Result<void>::Failure(replacement.Error());
    }

    out.complexCollisionMesh = complex.Value();
    out.distanceFieldReplacementMesh = replacement.Value();
    return Result<void>::Success();
}

} // namespace

Result<void> ImportStaticMesh(AssetImportContext& context, ObjectHandle output,
                              const StaticMeshImportSpec& spec,
                              MeshDiagnostic* meshFailure) noexcept {
    ObjectRegistry& registry = context.Registry();
    const ObjectAccessContext& access = context.Access();
    NamePool* names = registry.Names();
    if (names == nullptr) {
        return context.Fail(AssetDiagnosticCode::None, ErrorCode::InvalidArgument);
    }

    auto outputType = registry.AcquireType(access, output);
    if (!outputType) {
        return context.Fail(AssetDiagnosticCode::None, outputType.Error());
    }

    if (outputType.Value().Id() != TypeIdOf<StaticMesh>()) {
        return context.Fail(AssetDiagnosticCode::TypeMismatch, ErrorCode::TypeMismatch);
    }

    Allocator& allocator = registry.ObjectAllocator();

    // 1. 검증(저장 전에 모두 — 5.5.3절 3).
    if (auto valid = ValidateMeshes(context, spec, meshFailure); !valid) {
        return valid;
    }

    InternedNames interned(allocator);
    PerPlatformInt32 minLod(allocator);
    PerPlatformInt32 numStreamed(allocator);
    if (auto named = InternNames(context, *names, spec, allocator, minLod, numStreamed, interned);
        !named) {
        return named;
    }

    SimpleCollision collision(allocator);
    if (auto valid = ValidateCollision(context, spec, collision); !valid) {
        return valid;
    }

    OpenedReferences references(allocator);
    if (auto opened = OpenReferences(context, spec, references); !opened) {
        return opened;
    }

    // 2. 저장(원본·볼록 형상 → 출력 패키지 bulk).
    Array<StaticMeshSourceLod> lods(allocator);
    if (auto reserved = lods.TryReserve(spec.lods.Size()); !reserved) {
        return context.Fail(AssetDiagnosticCode::None, reserved.Error());
    }

    for (usize index = 0; index < spec.lods.Size(); ++index) {
        const StaticMeshImportLod& source = spec.lods[index];
        StaticMeshSourceLod lod(allocator);
        lod.hasSource = source.source != nullptr;
        lod.hasBakedSource = source.bakedSource != nullptr;
        if (lod.hasSource) {
            if (auto stored =
                    StoreMesh(context, output, *names, *source.source, lod.source, allocator);
                !stored) {
                return context.Fail(AssetDiagnosticCode::None, stored.Error(), index);
            }
        }

        if (lod.hasBakedSource) {
            if (auto stored = StoreMesh(context, output, *names, *source.bakedSource,
                                        lod.bakedSource, allocator);
                !stored) {
                return context.Fail(AssetDiagnosticCode::None, stored.Error(), index);
            }
        }

        lod.build = source.build;
        lod.reduction = source.reduction;
        lod.screenSize = Move(interned.screens[index]);
        (void)lods.TryPushBack(Move(lod));
    }

    for (usize index = 0; index < spec.convexes.Size(); ++index) {
        MemoryBulkSource bulk(allocator);
        CollisionConvex& convex = collision.convexes[index];
        if (auto written = StoreConvexGeometry(spec.convexes[index], bulk, convex); !written) {
            return context.Fail(AssetDiagnosticCode::None, written.Error(), index);
        }

        if (auto moved = Transfer(context, output, bulk, convex.vertices, allocator); !moved) {
            return context.Fail(AssetDiagnosticCode::None, moved.Error(), index);
        }

        if (auto moved = Transfer(context, output, bulk, convex.indices, allocator); !moved) {
            return context.Fail(AssetDiagnosticCode::None, moved.Error(), index);
        }
    }

    // 3. 필드 쓰기(재임포트는 사용자 편집을 보존 — 5.5.3절 6).
    auto borrowed = registry.ResolveForWrite<StaticMesh>(access, output);
    if (!borrowed) {
        return context.Fail(AssetDiagnosticCode::None, borrowed.Error());
    }

    auto value = borrowed.Value().Get();
    if (!value) {
        return context.Fail(AssetDiagnosticCode::None, value.Error());
    }

    StaticMesh& mesh = *value.Value();
    const bool reimport = context.IsReimport();
    const bool keepSettings = reimport && !spec.replaceSettingsOnReimport;

    // 옛 슬롯·편집기 소켓을 먼저 모은다(다시 만들기 전).
    Array<PreservedSlot> oldSlots(allocator);
    Array<MeshSocket> keptSockets(allocator);
    if (reimport) {
        for (usize index = 0; index < mesh.materialSlots.Size(); ++index) {
            const MeshMaterialSlot& slot = mesh.materialSlots[index];
            PreservedSlot kept;
            kept.importedSlotName = slot.importedSlotName;
            kept.slotName = slot.slotName;
            kept.material = slot.material.Handle();
            kept.overlayMaterial = slot.overlayMaterial.Handle();
            kept.uvDensity = slot.uvDensity;
            if (!oldSlots.TryPushBack(kept)) {
                return context.Fail(AssetDiagnosticCode::None, ErrorCode::OutOfMemory);
            }
        }

        for (usize index = 0; index < mesh.sockets.Size(); ++index) {
            if (!mesh.sockets[index].createdAtImport) {
                if (!keptSockets.TryPushBack(Move(mesh.sockets[index]))) {
                    return context.Fail(AssetDiagnosticCode::None, ErrorCode::OutOfMemory);
                }
            }
        }
    }

    // 원본 LOD: 데이터는 새 값, 설정은 옛 LOD가 있으면 보존.
    for (usize index = 0; keepSettings && index < lods.Size() && index < mesh.sourceLods.Size();
         ++index) {
        const StaticMeshSourceLod& old = mesh.sourceLods[index];
        lods[index].build = old.build;
        lods[index].reduction = old.reduction;
        lods[index].screenSize.defaultValue = old.screenSize.defaultValue;
        lods[index].screenSize.perPlatform.Clear();
        for (const auto& item : old.screenSize.perPlatform) {
            if (!lods[index].screenSize.perPlatform.Insert(item.key, item.value)) {
                return context.Fail(AssetDiagnosticCode::None, ErrorCode::OutOfMemory);
            }
        }
    }

    Result<void> step = Result<void>::Success();
    mesh.sourceLods = Move(lods);
    if (!reimport || !spec.sectionInfo.IsEmpty()) {
        mesh.sectionInfo.Clear();
        step = mesh.sectionInfo.TryAppend(spec.sectionInfo);
    }

    if (step) {
        step = registry.ClearElements(access, output, mesh.materialSlots);
    }

    for (usize index = 0; step && index < spec.slots.Size(); ++index) {
        auto added = registry.AddElement(access, output, mesh.materialSlots);
        if (!added) {
            step = Result<void>::Failure(added.Error());
            break;
        }

        MeshMaterialSlot& slot = mesh.materialSlots[added.Value()];
        slot.importedSlotName = interned.imported[index];
        slot.slotName = interned.slots[index];
        slot.uvDensity = spec.slots[index].uvDensity;
        ObjectHandle material = references.materials[index];
        ObjectHandle overlay = references.overlays[index];
        for (const PreservedSlot& old : oldSlots) {
            if (old.importedSlotName == interned.imported[index]) {
                slot.slotName = old.slotName;
                slot.uvDensity = old.uvDensity;
                material = material.IsNull() ? old.material : material;
                overlay = overlay.IsNull() ? old.overlayMaterial : overlay;
                break;
            }
        }

        step = registry.SetElementReference(access, output, mesh.materialSlots, added.Value(),
                                            slot.material, material);
        if (step) {
            step = registry.SetElementReference(access, output, mesh.materialSlots, added.Value(),
                                                slot.overlayMaterial, overlay);
        }
    }

    mesh.sockets.Clear();
    if (step) {
        step = mesh.sockets.TryReserve(spec.sockets.Size() + keptSockets.Size());
    }

    for (usize index = 0; step && index < spec.sockets.Size(); ++index) {
        const StaticMeshImportSocket& source = spec.sockets[index];
        MeshSocket socket(allocator);
        socket.name = interned.sockets[index];
        socket.relativeLocation = source.relativeLocation;
        socket.relativeRotation = source.relativeRotation;
        socket.relativeScale = source.relativeScale;
        socket.createdAtImport = source.createdAtImport;
        step = socket.tag.TryAssign(source.tag);
        if (step) {
            (void)mesh.sockets.TryPushBack(Move(socket));
        }
    }

    for (usize index = 0; step && index < keptSockets.Size(); ++index) {
        (void)mesh.sockets.TryPushBack(Move(keptSockets[index]));
    }

    mesh.simpleCollision = Move(collision);
    if (step && !keepSettings) {
        step = registry.SetReference(access, output, mesh.complexCollisionMesh,
                                     references.complexCollisionMesh);
        if (step) {
            step = registry.SetReference(access, output, mesh.distanceFieldReplacementMesh,
                                         references.distanceFieldReplacementMesh);
        }

        mesh.complexity = spec.complexity;
        mesh.lodForCollision = spec.lodForCollision;
        mesh.doubleSidedGeometry = spec.doubleSidedGeometry;
        mesh.lightMapResolution = spec.lightMapResolution;
        mesh.lightMapCoordinateIndex = spec.lightMapCoordinateIndex;
        mesh.positiveBoundsExtension = spec.positiveBoundsExtension;
        mesh.negativeBoundsExtension = spec.negativeBoundsExtension;
        mesh.autoComputeLodScreenSize = spec.autoComputeLodScreenSize;
        mesh.lodGroup = interned.lodGroup;
        minLod.defaultValue = spec.minLod;
        numStreamed.defaultValue = spec.numStreamedLods;
        mesh.minLod = Move(minLod);
        mesh.numStreamedLods = Move(numStreamed);
        mesh.virtualGeometry = spec.virtualGeometry;
    }

    if (!step) {
        return context.Fail(AssetDiagnosticCode::None, step.Error());
    }

    StaticMeshRenderData empty(allocator);
    mesh.renderData = Move(empty);
    return Result<void>::Success();
}

} // namespace rocks
