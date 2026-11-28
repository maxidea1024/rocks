#include "GraphInternal.h"

// graph v2 envelope(header·NodeTable·Record 경계) 읽기다(Docs/Design/ObjectFoundations.ko.md
// 5.1절). 편의 복원(GraphLoad.cpp)이 1단계 앞에서 부른다.
namespace rocks::graph_detail {

// header·NodeTable·Record 경계를 읽는다(Docs/Design/ObjectFoundations.ko.md 5.1절). 순서와 오류:
// magic InvalidArgument → formatVersion이 2가 아니면 Unsupported(v1은 읽지 않는다) → 정의되지 않은
// flags·NodeTable 없는 BaseOuter InvalidArgument → nodeCount 0 InvalidArgument·maxObjects 초과
// CapacityExceeded → externalCount가 maxExternalReferences 초과 CapacityExceeded → 노드 수에 비해
// 짧은 입력 InvalidArgument(할당 전) → NodeTable 항목(타입 0 InvalidArgument)·Record 길이·남는
// byte.
Result<Envelope> ReadEnvelope(Allocator& allocator, Span<const uint8> source,
                              Budget& budget) noexcept {
    auto envelope = ArchiveReader::Create(source, budget.Archive(true));
    if (!envelope) {
        return Result<Envelope>::Failure(envelope.Error());
    }

    auto magic = envelope.Value().ReadRaw(sizeof(Magic));
    if (!magic) {
        return Result<Envelope>::Failure(magic.Error());
    }

    for (usize index = 0; index < sizeof(Magic); ++index) {
        if (magic.Value()[index] != Magic[index]) {
            return Result<Envelope>::Failure(ErrorCode::CorruptData);
        }
    }

    auto version = envelope.Value().ReadU32();
    if (!version) {
        return Result<Envelope>::Failure(version.Error());
    }

    if (version.Value() != FormatVersion) {
        return Result<Envelope>::Failure(ErrorCode::Unsupported);
    }

    auto flags = envelope.Value().ReadU32();
    if (!flags) {
        return Result<Envelope>::Failure(flags.Error());
    }

    const bool nodeTable = (flags.Value() & NodeTableFlag) != 0;
    if ((flags.Value() & ~KnownFlags) != 0 ||
        ((flags.Value() & BaseOuterFlag) != 0 && !nodeTable)) {
        return Result<Envelope>::Failure(ErrorCode::CorruptData);
    }

    auto count = envelope.Value().ReadU32();
    if (!count) {
        return Result<Envelope>::Failure(count.Error());
    }

    if (count.Value() == 0) {
        return Result<Envelope>::Failure(ErrorCode::CorruptData);
    }

    if (count.Value() > budget.limits.maxObjects) {
        return Result<Envelope>::Failure(ErrorCode::CapacityExceeded);
    }

    auto externals = envelope.Value().ReadU32();
    if (!externals) {
        return Result<Envelope>::Failure(externals.Error());
    }

    if (externals.Value() > budget.limits.maxExternalReferences) {
        return Result<Envelope>::Failure(ErrorCode::CapacityExceeded);
    }

    // 노드마다 최소 byte(Record 길이 prefix와 header, NodeTable이면 항목)를 할당 전에 확인한다.
    const usize perNode = RecordPrefixBytes + RecordHeaderBytes + (nodeTable ? NodeEntryBytes : 0);
    if (static_cast<uint64>(count.Value()) * perNode > envelope.Value().Remaining()) {
        return Result<Envelope>::Failure(ErrorCode::CorruptData);
    }

    auto spent = budget.Spend(envelope.Value().WorkUsed());
    if (!spent) {
        return Result<Envelope>::Failure(spent.Error());
    }

    spent = budget.Spend(count.Value());
    if (!spent) {
        return Result<Envelope>::Failure(spent.Error());
    }

    Envelope result(allocator);
    result.flags = flags.Value();
    result.externalCount = externals.Value();
    // 노드 수만큼 먼저 예약하므로 이후 원소가 옮겨지지 않는다(FieldView가 node.migrated를
    // 가리킨다).
    auto reserved = result.nodes.TryReserve(count.Value());
    if (!reserved) {
        return Result<Envelope>::Failure(reserved.Error());
    }

    for (uint32 index = 0; index < count.Value(); ++index) {
        if (!result.nodes.TryEmplaceBack(allocator)) {
            platform::Fatal("Object graph node reservation failed");
        }
    }

    // 예약/초기화 비용을 먼저 차감한 나머지 예산만 body cursor에 준다.
    auto body = ArchiveReader::Create({source.Data() + HeaderBytes, source.Size() - HeaderBytes},
                                      budget.Archive(true));
    if (!body) {
        return Result<Envelope>::Failure(body.Error());
    }

    for (usize index = 0; nodeTable && index < result.nodes.Size(); ++index) {
        auto kind = body.Value().ReadU8();
        if (!kind) {
            return Result<Envelope>::Failure(kind.Error());
        }

        auto type = body.Value().ReadGuid();
        if (!type) {
            return Result<Envelope>::Failure(type.Error());
        }

        if (type.Value() == Guid{} ||
            (kind.Value() != NodeKindObject && kind.Value() != NodeKindDeclaredSubobject)) {
            return Result<Envelope>::Failure(ErrorCode::CorruptData);
        }

        auto outer = body.Value().ReadU32();
        if (!outer) {
            return Result<Envelope>::Failure(outer.Error());
        }

        auto name = body.Value().ReadStringView();
        if (!name) {
            return Result<Envelope>::Failure(name.Error());
        }

        LoadNode& node = result.nodes[index];
        node.kind = kind.Value();
        node.outer = outer.Value();
        // 선언 subobject 항목은 타입 칸에 SubobjectGuid를 싣고 이름이 비어 있으며 소유 노드가 있다
        // (1단계가 소유 노드 타입 표로 타입·이름을 채운다).
        if (node.kind == NodeKindDeclaredSubobject) {
            if (name.Value().ByteCount() != 0 || node.outer == 0) {
                return Result<Envelope>::Failure(ErrorCode::CorruptData);
            }

            node.subobjectId = type.Value();
        } else {
            node.type = type.Value();
            node.name = name.Value();
        }
    }

    for (usize index = 0; index < result.nodes.Size(); ++index) {
        auto record = body.Value().ReadBytes();
        if (!record) {
            return Result<Envelope>::Failure(record.Error());
        }

        result.nodes[index].source = record.Value();
    }

    if (!body.Value().AtEnd()) {
        return Result<Envelope>::Failure(ErrorCode::CorruptData);
    }

    spent = budget.Spend(body.Value().WorkUsed());
    if (!spent) {
        return Result<Envelope>::Failure(spent.Error());
    }

    return Result<Envelope>::Success(Move(result));
}

} // namespace rocks::graph_detail
