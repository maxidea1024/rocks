#pragma once

#include <Base/Endian.h>
#include <Base/Guid.h>
#include <Core/Containers/Array.h>
#include <Core/Containers/Span.h>
#include <Core/Memory/Allocator.h>
#include <Core/Text/StringView.h>
#include <TraceCapture/TraceFormat.h>

// TR1 시험이 출력 byte를 해석하는 최소 reader다. 형식 해석만 하고 시험 판정은 하지 않는다. TR4의
// TraceAnalysis가 이 파일을 옮겨 정식 reader의 출발점으로 쓸 수 있게, 시험 프레임워크와 세션 내부에
// 의존하지 않는다(TraceFormat.h와 Compression만 쓴다).
namespace rocks::tracetest {

struct DecodedHeader {
    uint16 version = 0;
    uint64 baseTick = 0;
    uint64 tickFrequency = 0;
    int64 startUtcTicks = 0;
    Guid sessionGuid;
    uint8 buildId[TraceFormat::HeaderBuildIdBytes] = {};
    uint64 attachOrderNumber = 0;
    uint32 flags = 0;
};

struct DecodedField {
    StringView name;
    TraceFieldType type = TraceFieldType::Bool;
    uint16 offset = 0;
};

struct DecodedSchema {
    uint16 typeId = 0;
    uint16 channelId = 0;
    uint8 flags = 0;
    uint16 fixedBytes = 0;
    StringView name;
    usize fieldCount = 0;
    DecodedField fields[TraceFormat::MaxFields];

    bool IsOrdered() const noexcept {
        return (flags & TraceFormat::TypeFlagOrdered) != 0;
    }
};

// 기록 하나다. 모든 view는 decoder가 가진 스트림 byte를 가리킨다(decoder 수명 동안 유효).
struct DecodedRecord {
    uint16 typeId = 0;
    bool hasOrder = false;
    uint64 order = 0;
    Span<const uint8> fixed;
    usize variableCount = 0;
    Span<const uint8> variable[TraceFormat::MaxFields];
};

struct DecodedStream {
    uint16 id = 0;
    Array<uint8> bytes;
};

// 파일(또는 출력 콜백이 받은) byte 전체를 머리·덩어리·스트림으로 나눈다.
class TraceStreamDecoder {
public:
    explicit TraceStreamDecoder(Allocator& allocator) noexcept;

    // 머리를 읽고 덩어리를 스트림별로 잇는다(압축 덩어리는 LZ4로 푼다). 머리·덩어리 형식이 틀리면
    // false다. 끝이 덩어리 중간에서 끊겼으면 그 앞까지 읽고 Truncated를 켠다.
    bool Decode(Span<const uint8> bytes) noexcept;

    const DecodedHeader& Header() const noexcept {
        return header_;
    }

    bool Truncated() const noexcept {
        return truncated_;
    }

    usize ChunkCount() const noexcept {
        return chunkCount_;
    }

    usize CompressedChunkCount() const noexcept {
        return compressedChunkCount_;
    }

    // 스트림 id의 이어 붙인 byte다. 없으면 빈 view다.
    Span<const uint8> Stream(uint16 id) const noexcept;

    const Array<DecodedStream>& Streams() const noexcept {
        return streams_;
    }

    // 스키마 스트림을 읽은 결과다(Decode가 채운다).
    const DecodedSchema* FindSchema(uint16 typeId) const noexcept;
    const DecodedSchema* FindSchema(StringView name) const noexcept;

    // 스트림 하나를 기록 열로 읽어 out 뒤에 붙인다. 모르는 형 id나 잘린 기록이면 false다.
    bool DecodeRecords(uint16 streamId, Array<DecodedRecord>& out) const noexcept;

private:
    bool AppendToStream(uint16 id, Span<const uint8> bytes) noexcept;
    bool DecodeSchemas() noexcept;

    Allocator* allocator_;
    DecodedHeader header_;
    bool truncated_ = false;
    usize chunkCount_ = 0;
    usize compressedChunkCount_ = 0;
    Array<DecodedStream> streams_;
    Array<DecodedSchema> schemas_;
};

// CPU 구간 배치(Trace.CpuScopeBatch)의 사건 하나다.
struct DecodedScopeEvent {
    uint64 tick = 0;
    uint32 nameId = 0;
    bool begin = false;
};

// 배치 내용 하나를 사건 열로 풀어 out 뒤에 붙인다(TraceStandardFormat의 varint 형식). 형식이 틀리면
// false다.
bool DecodeScopeBatch(Span<const uint8> events, Array<DecodedScopeEvent>& out) noexcept;

// 짝지은 CPU 구간이다. 끝을 잃은 구간(손실·세션 종료)은 ended가 false이고 길이를 지어내지 않는다
// (endTick 0).
struct DecodedScope {
    uint32 nameId = 0;
    uint64 beginTick = 0;
    uint64 endTick = 0;
    uint32 depth = 0;
    bool ended = false;
};

// 한 스레드의 사건 열을 스택으로 짝짓는다. 시작 순서대로 out에 붙인다. 시작을 잃은 끝(손실로 시작
// 배치가 버려짐)은 orphanEnds로 센다.
bool BuildScopes(Span<const DecodedScopeEvent> events, Array<DecodedScope>& out,
                 usize& orphanEnds) noexcept;

// 고정부 offset의 little-endian 정수를 읽는다(uint16·uint32·uint64).
template <typename T>
T ReadFixed(const DecodedRecord& record, usize offset) noexcept {
    return LoadLittleEndian<T>(record.fixed.Data() + offset);
}

inline uint8 ReadFixedByte(const DecodedRecord& record, usize offset) noexcept {
    return record.fixed[offset];
}

} // namespace rocks::tracetest
