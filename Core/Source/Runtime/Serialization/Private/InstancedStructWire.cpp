#include <Serialization/InstancedStructWire.h>

#include "WireInternal.h"

namespace rocks {
namespace {

// 넘긴 내용(content)을 InstancedStruct 값으로 해석한다. 실패하면 reader를 start로 되돌린다.
Result<InstancedStructWire> Interpret(ArchiveReader& reader, usize start,
                                      Span<const uint8> content) noexcept {
    InstancedStructWire value;
    if (content.Size() == 0) {
        return Result<InstancedStructWire>::Success(value);
    }

    auto header = ReadRecordHeader(content);
    if (!header) {
        detail::WireAccess::Rewind(reader, start);
        return Result<InstancedStructWire>::Failure(header.Error());
    }

    value.record = content;
    value.schema = header.Value().schema;
    value.schemaVersion = header.Value().schemaVersion;
    return Result<InstancedStructWire>::Success(value);
}

} // namespace

Result<InstancedStructWire> ReadInstancedStructField(ArchiveReader& payload) noexcept {
    const usize start = payload.Position();
    auto content = detail::WireAccess::SkipContent(payload, payload.Remaining());
    if (!content) {
        return Result<InstancedStructWire>::Failure(content.Error());
    }

    return Interpret(payload, start, content.Value());
}

Result<InstancedStructWire> ReadInstancedStructElement(ArchiveReader& elements) noexcept {
    const usize start = elements.Position();
    auto content = detail::WireAccess::ReadPrefixedContent(elements);
    if (!content) {
        return Result<InstancedStructWire>::Failure(content.Error());
    }

    return Interpret(elements, start, content.Value());
}

} // namespace rocks
