#include <TraceAnalysis/TraceStreamDecoder.h>

#include <Compression/Compression.h>
#include <Platform/Memory.h>

namespace rocks {

namespace {

// 앞에서부터 읽는 byte 커서다. 범위를 넘는 읽기는 실패로 표시하고 0을 돌려준다.
struct Reader {
    Span<const uint8> bytes;
    usize position = 0;
    bool failed = false;

    bool Has(usize count) const noexcept {
        return !failed && count <= bytes.Size() - position;
    }

    template <typename T>
    T Read() noexcept {
        if (!Has(sizeof(T))) {
            failed = true;
            return 0;
        }

        const T value = LoadLittleEndian<T>(bytes.Data() + position);
        position += sizeof(T);
        return value;
    }

    uint8 ReadByte() noexcept {
        if (!Has(1)) {
            failed = true;
            return 0;
        }

        return bytes[position++];
    }

    Span<const uint8> Take(usize count) noexcept {
        if (!Has(count)) {
            failed = true;
            return {};
        }

        const Span<const uint8> view = bytes.Mid(position, count);
        position += count;
        return view;
    }

    StringView ReadText() noexcept {
        const uint16 length = Read<uint16>();
        const Span<const uint8> text = Take(length);
        return StringView(reinterpret_cast<const char*>(text.Data()), text.Size());
    }
};

} // namespace

TraceStreamDecoder::TraceStreamDecoder(Allocator& allocator) noexcept
    : allocator_(&allocator), streams_(allocator), schemas_(allocator) {
}

Span<const uint8> TraceStreamDecoder::Stream(uint16 id) const noexcept {
    for (const DecodedStream& stream : streams_) {
        if (stream.id == id) {
            return stream.bytes.View();
        }
    }

    return {};
}

bool TraceStreamDecoder::AppendToStream(uint16 id, Span<const uint8> bytes) noexcept {
    for (DecodedStream& stream : streams_) {
        if (stream.id == id) {
            return static_cast<bool>(stream.bytes.TryAppend(bytes));
        }
    }

    auto added = streams_.TryEmplaceBack(DecodedStream{id, Array<uint8>(*allocator_)});
    return added && added.Value()->bytes.TryAppend(bytes);
}

bool TraceStreamDecoder::Decode(Span<const uint8> bytes) noexcept {
    Reader reader{bytes};
    const Span<const uint8> magic = reader.Take(sizeof(TraceFormat::Magic));
    if (reader.failed || !platform::MemoryEquals(magic.Data(), TraceFormat::Magic, magic.Size())) {
        return false;
    }

    header_.version = reader.Read<uint16>();
    const uint16 headerBytes = reader.Read<uint16>();
    if (header_.version != TraceFormat::Version || headerBytes < TraceFormat::FileHeaderBytes ||
        bytes.Size() < headerBytes) {
        return false;
    }

    header_.baseTick = reader.Read<uint64>();
    header_.tickFrequency = reader.Read<uint64>();
    header_.startUtcTicks = reader.Read<int64>();
    header_.sessionGuid.high = reader.Read<uint64>();
    header_.sessionGuid.low = reader.Read<uint64>();
    const Span<const uint8> buildId = reader.Take(TraceFormat::HeaderBuildIdBytes);
    platform::MemoryCopy(header_.buildId, buildId.Data(), buildId.Size());
    header_.attachOrderNumber = reader.Read<uint64>();
    header_.flags = reader.Read<uint32>();
    reader.position = headerBytes;

    Array<uint8> raw(*allocator_);
    while (reader.position < bytes.Size()) {
        if (!reader.Has(TraceFormat::ChunkHeaderBytes)) {
            truncated_ = true;
            break;
        }

        const uint16 sizeAndFlags = reader.Read<uint16>();
        const uint16 stream = reader.Read<uint16>();
        const usize stored = sizeAndFlags & TraceFormat::ChunkSizeMask;
        if (!reader.Has(stored)) {
            truncated_ = true;
            break;
        }

        Span<const uint8> payload = reader.Take(stored);
        ++chunkCount_;
        if ((sizeAndFlags & TraceFormat::ChunkCompressedBit) != 0) {
            ++compressedChunkCount_;
            if (payload.Size() < TraceFormat::ChunkRawSizeBytes) {
                return false;
            }

            const usize rawBytes = LoadLittleEndian<uint16>(payload.Data());
            if (!raw.TryResize(rawBytes)) {
                return false;
            }

            auto unpacked = TryDecompress(CompressionFormat::Lz4,
                                          payload.RightChop(TraceFormat::ChunkRawSizeBytes),
                                          raw.View(), *allocator_);
            if (!unpacked || unpacked.Value() != rawBytes) {
                return false;
            }

            payload = raw.View();
        }

        if (!AppendToStream(stream, payload)) {
            return false;
        }
    }

    return DecodeSchemas();
}

bool TraceStreamDecoder::DecodeSchemas() noexcept {
    Reader reader{Stream(TraceFormat::SchemaStreamId)};
    while (reader.position < reader.bytes.Size()) {
        const usize start = reader.position;
        const uint16 recordBytes = reader.Read<uint16>();
        DecodedSchema schema;
        schema.typeId = reader.Read<uint16>();
        schema.channelId = reader.Read<uint16>();
        schema.flags = reader.ReadByte();
        schema.fieldCount = reader.ReadByte();
        schema.fixedBytes = reader.Read<uint16>();
        schema.name = reader.ReadText();
        if (schema.fieldCount > TraceFormat::MaxFields) {
            return false;
        }

        for (usize index = 0; index < schema.fieldCount; ++index) {
            schema.fields[index].type = static_cast<TraceFieldType>(reader.ReadByte());
            schema.fields[index].offset = reader.Read<uint16>();
            schema.fields[index].name = reader.ReadText();
        }

        if (reader.failed || reader.position - start != recordBytes ||
            !schemas_.TryPushBack(schema)) {
            return false;
        }
    }

    return true;
}

const DecodedSchema* TraceStreamDecoder::FindSchema(uint16 typeId) const noexcept {
    for (const DecodedSchema& schema : schemas_) {
        if (schema.typeId == typeId) {
            return &schema;
        }
    }

    return nullptr;
}

const DecodedSchema* TraceStreamDecoder::FindSchema(StringView name) const noexcept {
    for (const DecodedSchema& schema : schemas_) {
        if (schema.name == name) {
            return &schema;
        }
    }

    return nullptr;
}

TraceDecodeStatus
TraceStreamDecoder::DecodeRecordsPartial(uint16 streamId,
                                         Array<DecodedRecord>& out) const noexcept {
    Reader reader{Stream(streamId)};
    while (reader.position < reader.bytes.Size()) {
        DecodedRecord record;
        record.typeId = reader.Read<uint16>();
        if (reader.failed) {
            return TraceDecodeStatus::Truncated;
        }

        const DecodedSchema* schema = FindSchema(record.typeId);
        if (schema == nullptr) {
            return TraceDecodeStatus::UnknownType;
        }

        if (schema->IsOrdered()) {
            record.hasOrder = true;
            record.order = reader.Read<uint64>();
        }

        record.fixed = reader.Take(schema->fixedBytes);
        uint32 lengths[TraceFormat::MaxFields] = {};
        for (usize index = 0; index < schema->fieldCount; ++index) {
            if (IsVariableTraceField(schema->fields[index].type)) {
                lengths[record.variableCount++] = reader.Read<uint32>();
            }
        }

        for (usize index = 0; index < record.variableCount; ++index) {
            record.variable[index] = reader.Take(lengths[index]);
        }

        if (reader.failed) {
            return TraceDecodeStatus::Truncated;
        }

        if (!out.TryPushBack(record)) {
            return TraceDecodeStatus::OutOfMemory;
        }
    }

    return TraceDecodeStatus::Complete;
}

bool DecodeScopeBatch(Span<const uint8> events, Array<DecodedScopeEvent>& out) noexcept {
    constexpr uint8 ContinueBit = 0x80;
    constexpr uint32 MaxShift = 63;
    usize position = 0;
    auto readVarint = [&](uint64& value) {
        value = 0;
        for (uint32 shift = 0; position < events.Size(); shift += 7) {
            if (shift > MaxShift) {
                return false;
            }

            const uint8 byte = events[position++];
            value |= static_cast<uint64>(byte & ~ContinueBit) << shift;
            if ((byte & ContinueBit) == 0) {
                return true;
            }
        }

        return false;
    };

    uint64 tick = 0;
    while (position < events.Size()) {
        uint64 head = 0;
        if (!readVarint(head)) {
            return false;
        }

        DecodedScopeEvent event;
        tick += head >> TraceStandardFormat::ScopeTagBits;
        event.tick = tick;
        event.begin =
            (head & TraceStandardFormat::ScopeTagMask) == TraceStandardFormat::ScopeBeginTag;
        if (event.begin) {
            uint64 name = 0;
            if (!readVarint(name)) {
                return false;
            }

            event.nameId = static_cast<uint32>(name);
        } else if ((head & TraceStandardFormat::ScopeTagMask) != 0) {
            return false;
        }

        if (!out.TryPushBack(event)) {
            return false;
        }
    }

    return true;
}

bool BuildScopes(Span<const DecodedScopeEvent> events, Array<DecodedScope>& out,
                 usize& orphanEnds) noexcept {
    constexpr usize MaxDepth = 256;
    usize stack[MaxDepth];
    usize depth = 0;
    orphanEnds = 0;
    for (const DecodedScopeEvent& event : events) {
        if (event.begin) {
            if (depth == MaxDepth) {
                return false;
            }

            DecodedScope scope;
            scope.nameId = event.nameId;
            scope.beginTick = event.tick;
            scope.depth = static_cast<uint32>(depth);
            stack[depth++] = out.Size();
            if (!out.TryPushBack(scope)) {
                return false;
            }
        } else if (depth == 0) {
            ++orphanEnds;
        } else {
            DecodedScope& scope = out[stack[--depth]];
            scope.endTick = event.tick;
            scope.ended = true;
        }
    }

    return true;
}

} // namespace rocks
