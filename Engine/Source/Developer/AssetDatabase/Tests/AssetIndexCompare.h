#pragma once

#include <AssetDatabase/AssetIndex.h>

// 색인 비교다(Docs/Design/AssetSystem.ko.md 7.1절 16 — 캐시·감시 시험이 캐시 없는 검색과 같은지
// 단언한다): 항목과 진단의 모든 칸.
namespace rocks::asset_database_test {

inline bool SameEntry(const AssetIndexEntry& left, const AssetIndexEntry& right) noexcept {
    if (!(left.id == right.id) || !(left.type == right.type) ||
        !(left.fingerprint == right.fingerprint) || left.fileBytes != right.fileBytes ||
        left.modifiedTicks != right.modifiedTicks || left.readOnly != right.readOnly ||
        left.path.View() != right.path.View() || left.key.View() != right.key.View() ||
        left.fileName.View() != right.fileName.View() || left.duplicateId != right.duplicateId ||
        left.duplicatePath != right.duplicatePath ||
        left.dependencies.Size() != right.dependencies.Size() ||
        left.tags.Size() != right.tags.Size() ||
        left.importRecord.present != right.importRecord.present ||
        !(left.importRecord.owner == right.importRecord.owner) ||
        !(left.importRecord.importerId == right.importRecord.importerId) ||
        left.importRecord.importerName.View() != right.importRecord.importerName.View() ||
        left.importRecord.importerVersion != right.importRecord.importerVersion ||
        left.importRecord.outputs.Size() != right.importRecord.outputs.Size()) {
        return false;
    }

    for (usize index = 0; index < left.importRecord.outputs.Size(); ++index) {
        if (!(left.importRecord.outputs[index] == right.importRecord.outputs[index])) {
            return false;
        }
    }

    for (usize index = 0; index < left.dependencies.Size(); ++index) {
        const PackageDependencyEntry& a = left.dependencies[index];
        const PackageDependencyEntry& b = right.dependencies[index];
        if (!(a.asset == b.asset) || a.kind != b.kind || !(a.expectedType == b.expectedType) ||
            a.required != b.required) {
            return false;
        }
    }

    for (usize index = 0; index < left.tags.Size(); ++index) {
        if (left.tags[index].key.View() != right.tags[index].key.View() ||
            left.tags[index].value.View() != right.tags[index].value.View()) {
            return false;
        }
    }

    return true;
}

inline bool SameIndex(const AssetIndex& left, const AssetIndex& right) noexcept {
    if (left.Count() != right.Count() || left.Diagnostics().Size() != right.Diagnostics().Size()) {
        return false;
    }

    for (usize index = 0; index < left.Count(); ++index) {
        if (!SameEntry(left.At(index), right.At(index))) {
            return false;
        }
    }

    for (usize index = 0; index < left.Diagnostics().Size(); ++index) {
        const AssetIndexDiagnostic& a = left.Diagnostics()[index];
        const AssetIndexDiagnostic& b = right.Diagnostics()[index];
        if (a.code != b.code || a.error != b.error || a.fileName.View() != b.fileName.View() ||
            !(a.asset == b.asset) || a.pathError != b.pathError || a.section != b.section ||
            a.detail != b.detail || a.related.View() != b.related.View() ||
            !(a.relatedAsset == b.relatedAsset)) {
            return false;
        }
    }

    return true;
}

} // namespace rocks::asset_database_test
