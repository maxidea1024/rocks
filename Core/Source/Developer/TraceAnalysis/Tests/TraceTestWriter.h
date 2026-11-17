#pragma once

#include <Base/Endian.h>
#include <Core/Containers/Array.h>
#include <Core/Containers/Span.h>
#include <Platform/Memory.h>
#include <TraceCapture/TraceFormat.h>

// 분석 시험의 입력을 tick·순서·형 id까지 정해 직접 만드는 writer다. TraceCapture 세션은 시각이
// 실행마다 달라 golden report를 만들 수 없으므로, 같은 형식(TraceFormat.h)을 이 writer로 쓴다.
// 덩어리는 압축하지 않는다(압축 덩어리 해석은 TraceCapture 시험이 실제 세션으로 확인한다).
namespace rocks::tracetest {

struct TestField {
    const char* name;
    TraceFieldType type;
    uint16 offset;
};

class TraceTestWriter {
public:
    TraceTestWriter(Allocator& allocator, uint64 frequency, uint32 flags = 0,
                    uint64 attachOrder = 0) noexcept
        : allocator_(&allocator), frequency_(frequency), flags_(flags), attachOrder_(attachOrder),
          streams_(allocator) {
    }

    // 스키마 기록 하나를 스키마 스트림에 쓴다(TraceFormat.h의 스키마 기록 형식).
    void Schema(uint16 typeId, const char* name, uint16 channelId, uint8 flags, uint16 fixedBytes,
                Span<const TestField> fields) noexcept {
        Array<uint8>& out = Stream(TraceFormat::SchemaStreamId);
        const usize start = out.Size();
        Put16(out, 0);
        Put16(out, typeId);
        Put16(out, channelId);
        Put8(out, flags);
        Put8(out, static_cast<uint8>(fields.Size()));
        Put16(out, fixedBytes);
        PutText(out, name);
        for (const TestField& field : fields) {
            Put8(out, static_cast<uint8>(field.type));
            Put16(out, field.offset);
            PutText(out, field.name);
        }

        StoreLittleEndian<uint16>(out.Data() + start, static_cast<uint16>(out.Size() - start));
    }

    // 기록 하나를 stream에 쓴다. order가 MaxOrder가 아니면 순서 번호를 붙인다(Ordered 형).
    void Record(uint16 stream, uint16 typeId, Span<const uint8> fixed,
                Span<const Span<const uint8>> variable = {},
                uint64 order = NumericLimits<uint64>::Max()) noexcept {
        Array<uint8>& out = Stream(stream);
        Put16(out, typeId);
        if (order != NumericLimits<uint64>::Max()) {
            Put64(out, order);
        }

        Append(out, fixed);
        for (const Span<const uint8>& part : variable) {
            Put32(out, static_cast<uint32>(part.Size()));
        }

        for (const Span<const uint8>& part : variable) {
            Append(out, part);
        }
    }

    // 파일 byte(머리 + 스트림마다 덩어리)다. 스트림은 스키마·정의·스레드 순으로 낸다.
    Array<uint8> Finish() noexcept {
        Array<uint8> file(*allocator_);
        uint8 header[TraceFormat::FileHeaderBytes] = {};
        platform::MemoryCopy(header, TraceFormat::Magic, sizeof(TraceFormat::Magic));
        StoreLittleEndian<uint16>(header + TraceFormat::HeaderVersionOffset, TraceFormat::Version);
        StoreLittleEndian<uint16>(header + TraceFormat::HeaderBytesOffset,
                                  static_cast<uint16>(TraceFormat::FileHeaderBytes));
        StoreLittleEndian<uint64>(header + TraceFormat::HeaderTickFrequencyOffset, frequency_);
        StoreLittleEndian<uint64>(header + TraceFormat::HeaderSessionGuidHighOffset, 0x1234);
        StoreLittleEndian<uint64>(header + TraceFormat::HeaderSessionGuidLowOffset, 0x5678);
        StoreLittleEndian<uint64>(header + TraceFormat::HeaderAttachOrderOffset, attachOrder_);
        StoreLittleEndian<uint32>(header + TraceFormat::HeaderFlagsOffset, flags_);
        Append(file, Span<const uint8>(header, sizeof(header)));
        for (uint32 id = 0; id <= NumericLimits<uint16>::Max(); ++id) {
            for (const Entry& entry : streams_) {
                if (entry.id != id) {
                    continue;
                }

                for (usize offset = 0; offset < entry.bytes.Size();
                     offset += TraceFormat::MaxChunkPayloadBytes) {
                    const usize take =
                        entry.bytes.Size() - offset < TraceFormat::MaxChunkPayloadBytes
                            ? entry.bytes.Size() - offset
                            : TraceFormat::MaxChunkPayloadBytes;
                    Put16(file, static_cast<uint16>(take));
                    Put16(file, entry.id);
                    Append(file, entry.bytes.View().Mid(offset, take));
                }
            }
        }

        return file;
    }

private:
    struct Entry {
        uint16 id;
        Array<uint8> bytes;
    };

    Array<uint8>& Stream(uint16 id) noexcept {
        for (Entry& entry : streams_) {
            if (entry.id == id) {
                return entry.bytes;
            }
        }

        auto added = streams_.TryEmplaceBack(Entry{id, Array<uint8>(*allocator_)});
        return added.Value()->bytes;
    }

    static void Append(Array<uint8>& out, Span<const uint8> bytes) noexcept {
        auto appended = out.TryAppend(bytes);
        (void)appended;
    }

    static void Put8(Array<uint8>& out, uint8 value) noexcept {
        Append(out, Span<const uint8>(&value, 1));
    }

    static void Put16(Array<uint8>& out, uint16 value) noexcept {
        uint8 bytes[2];
        StoreLittleEndian<uint16>(bytes, value);
        Append(out, Span<const uint8>(bytes, 2));
    }

    static void Put32(Array<uint8>& out, uint32 value) noexcept {
        uint8 bytes[4];
        StoreLittleEndian<uint32>(bytes, value);
        Append(out, Span<const uint8>(bytes, 4));
    }

    static void Put64(Array<uint8>& out, uint64 value) noexcept {
        uint8 bytes[8];
        StoreLittleEndian<uint64>(bytes, value);
        Append(out, Span<const uint8>(bytes, 8));
    }

    static void PutText(Array<uint8>& out, const char* text) noexcept {
        const usize length = platform::CStringLength(text);
        Put16(out, static_cast<uint16>(length));
        Append(out, Span<const uint8>(reinterpret_cast<const uint8*>(text), length));
    }

    Allocator* allocator_;
    uint64 frequency_;
    uint32 flags_;
    uint64 attachOrder_;
    Array<Entry> streams_;
};

// 고정부 byte를 차례로 채우는 도우미다.
template <usize Bytes>
struct FixedBytes {
    uint8 bytes[Bytes] = {};

    FixedBytes& U8(usize offset, uint8 value) noexcept {
        bytes[offset] = value;
        return *this;
    }

    FixedBytes& U16(usize offset, uint16 value) noexcept {
        StoreLittleEndian<uint16>(bytes + offset, value);
        return *this;
    }

    FixedBytes& U32(usize offset, uint32 value) noexcept {
        StoreLittleEndian<uint32>(bytes + offset, value);
        return *this;
    }

    FixedBytes& U64(usize offset, uint64 value) noexcept {
        StoreLittleEndian<uint64>(bytes + offset, value);
        return *this;
    }

    Span<const uint8> View() const noexcept {
        return Span<const uint8>(bytes, Bytes);
    }
};

} // namespace rocks::tracetest
