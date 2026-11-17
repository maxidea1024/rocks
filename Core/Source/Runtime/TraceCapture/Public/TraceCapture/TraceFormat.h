#pragma once

#include <Base/Types.h>

// trace 스트림의 byte 형식이다(Docs/Design/TraceSystem.ko.md 4·8절, 결정 T5). 쓰는
// 쪽(TraceCapture)과 읽는 쪽(시험의 최소 reader, 후속 TraceAnalysis)이 함께 쓰는 상수만 둔다. UE
// trace 형식과 호환하지 않는다. 모든 정수는 little-endian이다(Base/Endian.h의 IsLittleEndian 전제).
//
// 파일(.rtrace) = 머리(FileHeaderBytes) + 덩어리 열.
//   머리:   "RKTR" | u16 형식 버전 | u16 머리 byte 수 | u64 기준 tick | u64 tick 주파수
//           | i64 시작 UTC(100ns tick) | u64 세션 GUID high | u64 세션 GUID low | u8[32] 빌드 id
//           | u64 출력이 붙을 때의 순서 번호 | u32 머리 플래그 | u32 예약(0)
//   덩어리: u16 크기와 압축 비트 | u16 스트림 id | 내용
//           압축 비트가 켜져 있으면 내용은 u16 원본 크기 + LZ4 raw block이고, 크기는 그 둘을 합친
//           값이다.
// 스트림은 스키마(형 정의)·정의 기록·스레드별 기록으로 나뉘고, 같은 스트림의 덩어리 내용을 순서대로
// 이으면 그 스트림의 기록 열이 된다. 덩어리 경계는 기록 경계와 무관하다.
//
// 기록(정의·스레드 스트림) = u16 형 id | [u64 순서 번호: Ordered 형] | 고정부 | 가변 필드마다 u32
// 길이
//                          | 가변 필드 내용(필드 표 순서대로 이어 붙임)
// 스키마 기록(스키마 스트림) = u16 기록 byte 수 | u16 형 id | u16 채널 id | u8 형 플래그 | u8 필드
// 수
//                          | u16 고정부 byte 수 | u16 이름 길이 | 이름
//                          | 필드마다 (u8 필드 타입 | u16 고정부 offset | u16 이름 길이 | 이름)
namespace rocks {

// 이벤트 필드의 타입이다. 값은 스트림에 기록되므로 바꾸지 않는다(새 타입은 뒤에 더한다).
// 고정 크기 타입은 고정부의 offset에 놓이고, 문자열·배열은 가변부에 놓인다.
enum class TraceFieldType : uint8 {
    Bool = 1,
    Int8 = 2,
    Int16 = 3,
    Int32 = 4,
    Int64 = 5,
    UInt8 = 6,
    UInt16 = 7,
    UInt32 = 8,
    UInt64 = 9,
    Float32 = 10,
    Float64 = 11,
    // 다른 정의 기록이 발급한 id(이름 id·카운터 id 등)를 가리키는 64비트 값이다.
    Id = 12,
    // UTF-8 문자열이다. NUL을 붙이지 않는다.
    String = 32,
    Int8Array = 33,
    Int16Array = 34,
    Int32Array = 35,
    Int64Array = 36,
    UInt8Array = 37,
    UInt16Array = 38,
    UInt32Array = 39,
    UInt64Array = 40,
    Float32Array = 41,
    Float64Array = 42,
};

// 가변부(문자열·배열) 타입인지다.
constexpr bool IsVariableTraceField(TraceFieldType type) noexcept {
    return static_cast<uint8>(type) >= static_cast<uint8>(TraceFieldType::String);
}

// 고정 타입의 byte 크기, 배열 타입의 원소 크기다(문자열은 1). 모르는 값은 0이다.
constexpr usize TraceFieldElementBytes(TraceFieldType type) noexcept {
    switch (type) {
    case TraceFieldType::Bool:
    case TraceFieldType::Int8:
    case TraceFieldType::UInt8:
    case TraceFieldType::String:
    case TraceFieldType::Int8Array:
    case TraceFieldType::UInt8Array:
        return 1;
    case TraceFieldType::Int16:
    case TraceFieldType::UInt16:
    case TraceFieldType::Int16Array:
    case TraceFieldType::UInt16Array:
        return 2;
    case TraceFieldType::Int32:
    case TraceFieldType::UInt32:
    case TraceFieldType::Float32:
    case TraceFieldType::Int32Array:
    case TraceFieldType::UInt32Array:
    case TraceFieldType::Float32Array:
        return 4;
    case TraceFieldType::Int64:
    case TraceFieldType::UInt64:
    case TraceFieldType::Float64:
    case TraceFieldType::Id:
    case TraceFieldType::Int64Array:
    case TraceFieldType::UInt64Array:
    case TraceFieldType::Float64Array:
        return 8;
    }
    return 0;
}

// 형식 상수 모음이다. 인스턴스를 만들지 않는다.
struct TraceFormat {
    // ---- 파일 머리 ----
    static constexpr uint8 Magic[4] = {'R', 'K', 'T', 'R'};
    static constexpr uint16 Version = 1;
    static constexpr usize FileHeaderBytes = 96;

    // 머리 안 각 칸의 offset이다.
    static constexpr usize HeaderVersionOffset = 4;
    static constexpr usize HeaderBytesOffset = 6;
    static constexpr usize HeaderBaseTickOffset = 8;
    static constexpr usize HeaderTickFrequencyOffset = 16;
    static constexpr usize HeaderStartUtcOffset = 24;
    static constexpr usize HeaderSessionGuidHighOffset = 32;
    static constexpr usize HeaderSessionGuidLowOffset = 40;
    static constexpr usize HeaderBuildIdOffset = 48;
    static constexpr usize HeaderBuildIdBytes = 32;
    static constexpr usize HeaderAttachOrderOffset = 80;
    static constexpr usize HeaderFlagsOffset = 88;

    // 머리 플래그: 출력이 붙기 전에 정의 저장소가 넘쳤거나 정의 기록이 버려져, 이 출력이 받은
    // 정의가 일부 빠져 있을 수 있다(4.3절).
    static constexpr uint32 HeaderFlagDefinitionsIncomplete = 1u << 0;

    // ---- 덩어리 ----
    static constexpr usize ChunkHeaderBytes = 4;
    // 크기 칸의 최상위 비트가 압축 표시다.
    static constexpr uint16 ChunkCompressedBit = 0x8000;
    static constexpr uint16 ChunkSizeMask = 0x7FFF;
    // 압축 덩어리 내용 앞의 원본 크기 칸(u16)이다.
    static constexpr usize ChunkRawSizeBytes = 2;
    // 덩어리 하나가 담는 원본 내용의 상한이다(크기 칸 15비트 안).
    static constexpr usize MaxChunkPayloadBytes = 16 * 1024;
    // 원본 내용이 이 크기를 넘는 덩어리만 압축을 시도한다(UE와 같은 384 B).
    static constexpr usize CompressThresholdBytes = 384;

    // ---- 스트림 ----
    static constexpr uint16 SchemaStreamId = 0;
    static constexpr uint16 DefinitionStreamId = 1;
    // trace 스레드 id t(1부터)의 스트림 id는 FirstThreadStreamId + t - 1이다.
    static constexpr uint16 FirstThreadStreamId = 2;
    static constexpr uint32 MaxThreadId = NumericLimits<uint16>::Max() - FirstThreadStreamId + 1;

    // ---- 형 id ----
    static constexpr uint16 InvalidTypeId = 0;
    // 세션이 스스로 등록하는 형이다. 스키마는 다른 형처럼 스키마 스트림으로 나간다.
    static constexpr uint16 ChannelInfoTypeId = 1;
    static constexpr uint16 ChannelStateTypeId = 2;
    static constexpr uint16 ThreadInfoTypeId = 3;
    static constexpr uint16 LossTypeId = 4;
    static constexpr uint16 DefinitionLossTypeId = 5;
    static constexpr uint16 FirstUserTypeId = 16;
    // 형 id의 상한(포함)이다.
    static constexpr uint16 MaxTypeId = 4095;

    // 채널이 없는 형(세션 내부 형)의 채널 id다.
    static constexpr uint16 NoChannelId = NumericLimits<uint16>::Max();
    // 가변 필드의 offset 칸 값이다.
    static constexpr uint16 VariableFieldOffset = NumericLimits<uint16>::Max();

    // 형 플래그(스키마 기록의 u8)다.
    static constexpr uint8 TypeFlagOrdered = 1u << 0;
    static constexpr uint8 TypeFlagDefinition = 1u << 1;

    // ---- 크기 상한 ----
    // 형·필드·채널 이름의 byte 상한이다.
    static constexpr usize MaxNameBytes = 255;
    static constexpr usize MaxFields = 32;
    // 채널 id는 0 … MaxChannels - 1이다.
    static constexpr uint16 MaxChannels = 128;
    // 스레드 기록 하나(머리·고정부·길이·가변부 합)의 상한이다. 넘으면 손실로 센다.
    static constexpr usize MaxRecordBytes = 1024 * 1024;
    // 정의 기록 하나의 상한이다. 넘으면 버린 정의로 센다.
    static constexpr usize MaxDefinitionRecordBytes = 16 * 1024;

    // ---- 세션 내부 형의 고정부 배치 ----
    // ChannelInfo(정의): u16 채널 id | u8 읽기 전용 | u8 켜짐, 가변: 이름(String) | 설명(String)
    static constexpr usize ChannelInfoIdOffset = 0;
    static constexpr usize ChannelInfoReadOnlyOffset = 2;
    static constexpr usize ChannelInfoEnabledOffset = 3;
    static constexpr usize ChannelInfoFixedBytes = 4;
    // ChannelState(정의): u64 tick | u16 채널 id | u8 켜짐
    static constexpr usize ChannelStateTickOffset = 0;
    static constexpr usize ChannelStateIdOffset = 8;
    static constexpr usize ChannelStateEnabledOffset = 10;
    static constexpr usize ChannelStateFixedBytes = 11;
    // ThreadInfo(정의): u32 trace 스레드 id | u64 OS 스레드 id
    static constexpr usize ThreadInfoIdOffset = 0;
    static constexpr usize ThreadInfoOsIdOffset = 4;
    static constexpr usize ThreadInfoFixedBytes = 12;
    // Loss(스레드 스트림): u64 손실 시작 tick | u64 손실 끝 tick,
    // 가변: 채널 id(UInt16Array) | 채널별 손실 수(UInt64Array). 두 배열의 원소 수는 같다.
    static constexpr usize LossStartTickOffset = 0;
    static constexpr usize LossEndTickOffset = 8;
    static constexpr usize LossFixedBytes = 16;
    // DefinitionLoss(정의 스트림, writer가 냄): u64 tick | u64 세션 시작부터 버린 정의 기록 누계
    static constexpr usize DefinitionLossTickOffset = 0;
    static constexpr usize DefinitionLossCountOffset = 8;
    static constexpr usize DefinitionLossFixedBytes = 16;
};

} // namespace rocks
