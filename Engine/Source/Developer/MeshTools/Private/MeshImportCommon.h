#pragma once

#include <AssetDatabase/AssetImportBulk.h>
#include <AssetDatabase/AssetImporter.h>
#include <AssetRuntime/MemoryBulkSource.h>
#include <Core/Containers/Array.h>
#include <Core/Containers/Span.h>
#include <Core/Text/NamePool.h>
#include <Mesh/Common/MeshMaterialSlot.h>
#include <Mesh/Common/MeshSectionInfoEntry.h>
#include <MeshSource/MeshSource.h>
#include <Object/ObjectRegistry.h>

// 정적·스킨 메시 임포트 서비스가 함께 쓰는 도우미다(Docs/Design/MeshAnimationAssets 5.5.3절·6.7.3절
// 5). 이름 있는 namespace라 유니티 빌드에서 파일마다 겹치지 않는다.
namespace rocks::mesh_import {

// 참조 위치 번호(TypeMismatch detail)다: 슬롯 i의 material 2i·overlay 2i+1, 그 뒤 메시별 참조.
inline constexpr uint64 ReferencesPerSlot = 2;

inline Result<void> Invalid(AssetImportContext& context, uint64 detail,
                            ErrorCode error = ErrorCode::InvalidArgument) noexcept {
    return context.Fail(AssetDiagnosticCode::InvalidSetting, error, detail);
}

// text를 intern한다(실패는 InvalidSetting과 원래 오류 — 이름 규칙·pool 상한·종료).
inline Result<Name> InternChecked(AssetImportContext& context, NamePool& names, StringView text,
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

// 플랫폼별 값(PerPlatformFloat·Int32·Bool)을 복사한다(맵은 복사 생성이 없다).
template <class PerPlatform>
Result<void> CopyPerPlatform(const PerPlatform& source, PerPlatform& out) noexcept {
    out.defaultValue = source.defaultValue;
    out.perPlatform.Clear();
    for (const auto& item : source.perPlatform) {
        if (auto inserted = out.perPlatform.Insert(item.key, item.value); !inserted) {
            return Result<void>::Failure(inserted.Error());
        }
    }

    return Result<void>::Success();
}

// id의 대표 객체를 열고 그 타입이 expected 파생인지 본다(null id는 비움).
inline Result<ObjectHandle> OpenTyped(AssetImportContext& context, AssetId id, TypeId expected,
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

// 원본 메시를 영속 형태로 바꿔 그 bulk를 출력 패키지로 옮긴다.
inline Result<void> StoreMesh(AssetImportContext& context, ObjectHandle output, NamePool& names,
                              const MeshSource& mesh, MeshSourceData& data,
                              Allocator& allocator) noexcept {
    MemoryBulkSource bulk(allocator);
    if (auto written = mesh.Store(bulk, names, data); !written) {
        return written;
    }

    if (auto moved = TransferImportBulk(context, output, bulk, data.topology, allocator); !moved) {
        return moved;
    }

    for (MeshAttributeRecord& record : data.attributes) {
        if (auto moved = TransferImportBulk(context, output, bulk, record.data, allocator);
            !moved) {
            return moved;
        }
    }

    return Result<void>::Success();
}

// 섹션 정보가 (lod, section) 엄격 증가이고 LOD 범위 안인지 본다(위반 InvalidSetting, detail =
// 위치).
inline Result<void> ValidateSectionInfo(AssetImportContext& context,
                                        Span<const MeshSectionInfoEntry> table,
                                        usize lodCount) noexcept {
    for (usize index = 0; index < table.Size(); ++index) {
        const MeshSectionInfoEntry& entry = table[index];
        const MeshSectionInfoEntry* previous = index == 0 ? nullptr : &table[index - 1];
        const bool ordered = previous == nullptr || entry.lod > previous->lod ||
                             (entry.lod == previous->lod && entry.section > previous->section);
        if (entry.lod < 0 || static_cast<usize>(entry.lod) >= lodCount || entry.section < 0 ||
            !ordered) {
            return Invalid(context, index);
        }
    }

    return Result<void>::Success();
}

// 재임포트에서 보존할 옛 슬롯 값이다(5.5.3절 6).
struct PreservedSlot {
    Name importedSlotName;
    Name slotName;
    ObjectHandle material{};
    ObjectHandle overlayMaterial{};
    MeshUvDensity uvDensity;
};

// 메시의 머티리얼 슬롯을 명세 값으로 다시 만든다(옛 슬롯 old와 importedSlotName이 같으면 이름·UV
// 밀도와 비어 있는 참조를 옛 값으로 — 5.5.3절 6). imported·slots·materials·overlays·densities는
// 명세 슬롯 순서다.
inline Result<void> WriteSlots(ObjectRegistry& registry, const ObjectAccessContext& access,
                               ObjectHandle output, StructRefArray<MeshMaterialSlot>& target,
                               Span<const PreservedSlot> old, Span<const Name> imported,
                               Span<const Name> slots, Span<const ObjectHandle> materials,
                               Span<const ObjectHandle> overlays,
                               Span<const MeshUvDensity> densities) noexcept {
    if (auto cleared = registry.ClearElements(access, output, target); !cleared) {
        return cleared;
    }

    for (usize index = 0; index < imported.Size(); ++index) {
        auto added = registry.AddElement(access, output, target);
        if (!added) {
            return Result<void>::Failure(added.Error());
        }

        MeshMaterialSlot& slot = target[added.Value()];
        slot.importedSlotName = imported[index];
        slot.slotName = slots[index];
        slot.uvDensity = densities[index];
        ObjectHandle material = materials[index];
        ObjectHandle overlay = overlays[index];
        for (const PreservedSlot& previous : old) {
            if (previous.importedSlotName == imported[index]) {
                slot.slotName = previous.slotName;
                slot.uvDensity = previous.uvDensity;
                material = material.IsNull() ? previous.material : material;
                overlay = overlay.IsNull() ? previous.overlayMaterial : overlay;
                break;
            }
        }

        if (auto set = registry.SetElementReference(access, output, target, added.Value(),
                                                    slot.material, material);
            !set) {
            return set;
        }

        if (auto set = registry.SetElementReference(access, output, target, added.Value(),
                                                    slot.overlayMaterial, overlay);
            !set) {
            return set;
        }
    }

    return Result<void>::Success();
}

// 메시의 옛 슬롯 값을 모은다(다시 만들기 전 — 재임포트).
inline Result<void> CollectSlots(const StructRefArray<MeshMaterialSlot>& slots,
                                 Array<PreservedSlot>& out) noexcept {
    for (usize index = 0; index < slots.Size(); ++index) {
        const MeshMaterialSlot& slot = slots[index];
        PreservedSlot kept;
        kept.importedSlotName = slot.importedSlotName;
        kept.slotName = slot.slotName;
        kept.material = slot.material.Handle();
        kept.overlayMaterial = slot.overlayMaterial.Handle();
        kept.uvDensity = slot.uvDensity;
        if (auto pushed = out.TryPushBack(kept); !pushed) {
            return pushed;
        }
    }

    return Result<void>::Success();
}

} // namespace rocks::mesh_import
