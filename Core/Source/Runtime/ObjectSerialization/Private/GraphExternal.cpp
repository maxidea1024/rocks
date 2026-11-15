#include "GraphSaveInternal.h"
#include <Base/Endian.h>

// 외부 참조다(Docs/Design/ObjectFoundations.ko.md 5.3·5.4절): 저장의 Admit 허락·Import 집계·값 위치
// 표(필드 writer offset → Record offset → 출력 offset)와 번호 재배정(AssignExternalIndices), 단계형
// 복원 2b의 imports 확인(편의 복원의 생성 전 확인도 같은 판정). 1단계의 범위 검사와 쓰임 집계는
// GraphLoad.cpp, 선언 타입 대조와 쌍 기록은 GraphReferences.cpp, 2b의 값 치환은 GraphApply.cpp다.
namespace rocks::graph_detail {

// Admit: 처음 보는 대상(또는 앞 결정이 Null인 대상)의 허락을 hook에 묻는다. Import면 slot(첫 출현
// 순)을 주고 위치를 기록해 쓰고, Null이면 진단(value 1) 뒤 Required 규칙을 거쳐 null을 쓰고,
// Reject면 진단(value 0) 뒤 InvalidArgument다. 정의되지 않은 결과 값은 InvalidArgument다.
Result<void> GraphEncodeContext::AdmitExternal(ArchiveWriter& out, ObjectHandle target,
                                               TypeLease lease,
                                               const ReferenceExpectation& expectation) noexcept {
    // Import면 받을 slot이다(Null·Reject는 slot을 쓰지 않으므로 다음 대상이 같은 번호를 받는다).
    const uint32 slot = static_cast<uint32>(externals_.Size()) + 1;
    auto admitted = AdmitExternalReference(target, lease.Id(), expectation.schemaType, slot);
    if (!admitted) {
        return Result<void>::Failure(admitted.Error());
    }

    switch (admitted.Value()) {
    case ExternalAdmission::Import: {
        if (externals_.Size() >= budget_.limits.maxExternalReferences) {
            return Result<void>::Failure(ErrorCode::CapacityExceeded);
        }

        // 외부 대상 표 성장(기존 원소 이동)과 해시 표 삽입을 청구한다.
        const usize moved = externals_.Size() == externals_.Capacity() ? externals_.Size() : 0;
        if (auto spent = budget_.Spend(2 + static_cast<uint64>(moved)); !spent) {
            return spent;
        }

        GraphExternal external;
        external.target = target;
        external.type = lease.Id();
        if (!externals_.TryPushBack(external)) {
            return Result<void>::Failure(ErrorCode::OutOfMemory);
        }

        // 표 셋(대상·lease·handle → slot)은 함께 늘고 하나라도 실패하면 함께 되돌린다.
        if (!externalLeases_.TryPushBack(Move(lease))) {
            (void)externals_.TryPopBack();
            return Result<void>::Failure(ErrorCode::OutOfMemory);
        }

        if (auto inserted = slots_.Insert(target, slot); !inserted) {
            (void)externalLeases_.TryPopBack();
            (void)externals_.TryPopBack();
            return Result<void>::Failure(inserted.Error());
        }

        return WriteImported(out, slot);
    }
    case ExternalAdmission::Null: {
        if (auto reported = Report(FieldDiagnosticKind::ExternalReference, 1); !reported) {
            return reported;
        }

        // 쓰는 값은 null이므로 Required 위치면 null 규칙을 적용한다(rejectMissingRequired면
        // RequiredMissing 진단 뒤 InvalidArgument).
        if (auto required = CheckRequired(true); !required) {
            return required;
        }

        return out.WriteU32(0);
    }
    case ExternalAdmission::Reject:
        return RejectExternal();
    }

    // 정의되지 않은 결과 값이다(hook 계약 위반).
    return Result<void>::Failure(ErrorCode::InvalidArgument);
}

// Import된 slot의 참조 하나다: 값 위치를 필드 writer 안 offset으로 위치 표에 기록하고(그 메모리를
// 그래프 바이트 누계·작업량에 청구한다), 대상의 출현 수·공통 플래그·Required를 모은 뒤 쓴다.
Result<void> GraphEncodeContext::WriteImported(ArchiveWriter& out, uint32 slot) noexcept {
    // 직렬화기가 준 필드 writer가 아니면 그 위치를 Record 안 위치로 바꿀 수 없다(codec 계약).
    if (fieldWriter_ == nullptr || &out != fieldWriter_) {
        return Result<void>::Failure(ErrorCode::InvalidArgument);
    }

    const usize at = out.ByteCount();
    if (at > NumericLimits<uint32>::Max()) {
        return Result<void>::Failure(ErrorCode::CapacityExceeded);
    }

    // 위치 표(참조 하나에 4 byte)도 그래프 바이트 누계에 든다: 이 노드 앞까지의 출력 + 위치 표가
    // maxBytes 안이어야 한다(이 노드의 Record까지 더한 검사는 노드 끝에서 한다).
    const usize maximum = budget_.limits.maxBytes;
    const usize table = PositionBytes() + sizeof(uint32);
    if (committedBytes_ > maximum || table > maximum - committedBytes_) {
        return Result<void>::Failure(ErrorCode::CapacityExceeded);
    }

    // 위치 4 byte와, 표가 자랄 때 기존 위치의 이동을 청구한다.
    const usize moved = positions_.Size() == positions_.Capacity() ? PositionBytes() : 0;
    if (auto spent = budget_.Spend(sizeof(uint32) + static_cast<uint64>(moved)); !spent) {
        return spent;
    }

    if (!positions_.TryPushBack(static_cast<uint32>(at))) {
        return Result<void>::Failure(ErrorCode::OutOfMemory);
    }

    GraphExternal& external = externals_[slot - 1];
    const uint32 flags = EffectiveFlags();
    ++external.occurrences;
    external.commonFlags &= flags;
    external.required = external.required || HasFlag(flags, FieldFlags::Required);
    return out.WriteU32(ExternalReferenceBit | slot);
}

Result<void> GraphEncodeContext::RebasePositions(usize first, usize end, usize base) noexcept {
    if (first >= end) {
        return Result<void>::Success();
    }

    if (auto spent = budget_.Spend(static_cast<uint64>(end - first)); !spent) {
        return spent;
    }

    for (usize index = first; index < end; ++index) {
        usize moved = 0;
        if (!TryAdd(static_cast<usize>(positions_[index]), base, moved) ||
            moved > NumericLimits<uint32>::Max()) {
            return Result<void>::Failure(ErrorCode::CapacityExceeded);
        }

        positions_[index] = static_cast<uint32>(moved);
    }

    return Result<void>::Success();
}

Result<void> CheckImportTargets(const StageData& data, Span<const TypeLease> actual) noexcept {
    if (actual.Size() != data.importLeases.Size()) {
        return Result<void>::Failure(ErrorCode::InvalidArgument);
    }

    // 실제 타입이 선언 타입의 native 파생이 아니면 호출자 계약 위반이다.
    for (usize index = 0; index < actual.Size(); ++index) {
        if (!actual[index].IsNativeA(data.importLeases[index].Ops().nativeType)) {
            return Result<void>::Failure(ErrorCode::InvalidArgument);
        }
    }

    // 실제 연결의 판정은 1단계가 기록한 (import, 기대 형식) 쌍이다 — 정적 thunk 대상 타입과 스키마
    // 기대 타입을 모두 만족해야 한다(노드 참조와 같은 native 상속 계약).
    for (const ImportExpectation& check : data.importChecks) {
        const TypeLease& target = actual[check.externalIndex - 1];
        const ReferenceExpectation& expectation = check.expectation;
        if (expectation.staticNative && !target.IsNativeA(expectation.staticNative)) {
            return Result<void>::Failure(ErrorCode::TypeMismatch);
        }

        if (expectation.schemaType == Guid{}) {
            continue;
        }

        // 1단계가 기대 타입 lease를 열어 두었다(없으면 1단계·2b가 어긋난 계약 위반).
        const TypeLease* expected = data.expectedLeases.Find(expectation.schemaType);
        if (!expected) {
            return Result<void>::Failure(ErrorCode::InvalidArgument);
        }

        if (!target.IsNativeA(expected->Ops().nativeType)) {
            return Result<void>::Failure(ErrorCode::TypeMismatch);
        }
    }

    return Result<void>::Success();
}

Result<void> CheckImports(Allocator& allocator, ConstructionContext& transaction,
                          const StageData& data, Span<const ObjectHandle> imports) noexcept {
    if (imports.Size() != data.importLeases.Size()) {
        return Result<void>::Failure(ErrorCode::InvalidArgument);
    }

    if (imports.Size() == 0) {
        return Result<void>::Success();
    }

    // import마다 실제 타입을 연다(생존 객체 또는 이 트랜잭션의 객체 — 아니면 StaleHandle). 판정은
    // 편의 복원의 생성 전 확인과 같은 함수다.
    Array<TypeLease> actual(allocator);
    if (auto reserved = actual.TryReserve(imports.Size()); !reserved) {
        return reserved;
    }

    for (ObjectHandle target : imports) {
        auto type = transaction.AcquireType(target);
        if (!type) {
            return Result<void>::Failure(type.Error());
        }

        if (!actual.TryPushBack(Move(type.Value()))) {
            platform::Fatal("Object graph import type reservation failed");
        }
    }

    return CheckImportTargets(data, actual.View());
}
} // namespace rocks::graph_detail

namespace rocks {

Result<void> ObjectGraphSaveResult::AssignExternalIndices(Span<const uint32> indexBySlot,
                                                          uint32 count) noexcept {
    using namespace graph_detail;
    if (bytes_.Size() < HeaderBytes || indexBySlot.Size() != externals_.Size()) {
        return Result<void>::Failure(ErrorCode::InvalidArgument);
    }

    if (count > maxExternalReferences_) {
        return Result<void>::Failure(ErrorCode::CapacityExceeded);
    }

    // 새 역표(새 번호 − 1 → slot)를 만들며 값을 검사한다: 0·count 초과·중복은 InvalidArgument다.
    // 검사를 모두 마친 뒤에야 byte를 바꾸므로 실패하면 결과는 그대로다.
    Array<uint32> next(slotByIndex_.GetAllocator());
    if (auto resized = next.TryResize(count); !resized) {
        return resized;
    }

    for (usize slot = 1; slot <= indexBySlot.Size(); ++slot) {
        const uint32 index = indexBySlot[slot - 1];
        if (index == 0 || index > count || next[index - 1] != 0) {
            return Result<void>::Failure(ErrorCode::InvalidArgument);
        }

        next[index - 1] = static_cast<uint32>(slot);
    }

    // 위치마다 지금 값(지금 외부 번호 — 아직 재배정하지 않았으면 slot)에서 slot을 찾아 새 번호로
    // 덮어쓴다.
    uint8* const bytes = bytes_.Data();
    for (uint32 position : positions_) {
        const uint32 current = LoadLittleEndian<uint32>(bytes + position) & ~ExternalReferenceBit;
        const usize known = slotByIndex_.IsEmpty() ? externals_.Size() : slotByIndex_.Size();
        if (current == 0 || current > known) {
            platform::Fatal("Object graph external position holds an unknown index");
        }

        const uint32 slot = slotByIndex_.IsEmpty() ? current : slotByIndex_[current - 1];
        if (slot == 0) {
            platform::Fatal("Object graph external position holds an unassigned index");
        }

        StoreLittleEndian<uint32>(bytes + position, ExternalReferenceBit | indexBySlot[slot - 1]);
    }

    StoreLittleEndian<uint32>(bytes + ExternalCountOffset, count);
    slotByIndex_ = Move(next);
    return Result<void>::Success();
}

} // namespace rocks
