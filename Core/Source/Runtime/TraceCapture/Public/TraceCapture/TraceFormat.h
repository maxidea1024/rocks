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
// 기록(정의·스레드 스트림) = u16 형 id | [u64 순서 번호: Ordered 형] | 고정부
//                          | 가변 필드마다 u32 길이 | 가변 필드 내용(필드 표 순서대로 이어 붙임)
// 스키마 기록(스키마 스트림) = u16 기록 byte 수 | u16 형 id | u16 채널 id | u8 형 플래그
//                          | u8 필드 수 | u16 고정부 byte 수 | u16 이름 길이 | 이름
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
    // ThreadInfo(정의): u32 trace 스레드 id | u64 OS 스레드 id | i32 정렬 순위,
    // 가변: 이름(String) | 묶음(String). 같은 스레드의 기록이 다시 나오면 나중 것이 이긴다.
    static constexpr usize ThreadInfoIdOffset = 0;
    static constexpr usize ThreadInfoOsIdOffset = 4;
    static constexpr usize ThreadInfoSortOrderOffset = 12;
    static constexpr usize ThreadInfoFixedBytes = 16;
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

// 세션이 Start 때 스스로 등록하는 표준 채널·형(TR2: CPU 구간·카운터·표지·시간 띠·프레임·문맥)의
// 형식이다. 형 id는 등록 순서로 정해지므로 reader는 아래 이름으로 스키마를 찾는다. 이름 id·카운터
// id·표지 호출 지점 id·문맥 id는 세션마다 1부터 매기는 u32이고, 0은 "없음"이다.
struct TraceStandardFormat {
    // ---- 이름 ----
    static constexpr const char* ScopeNameType = "Trace.ScopeName";
    static constexpr const char* CpuScopeBatchType = "Trace.CpuScopeBatch";
    static constexpr const char* CounterDefinitionType = "Trace.CounterDefinition";
    static constexpr const char* CounterIntegerType = "Trace.CounterInteger";
    static constexpr const char* CounterFloatType = "Trace.CounterFloat";
    static constexpr const char* MarkSiteType = "Trace.MarkSite";
    static constexpr const char* MarkType = "Trace.Mark";
    static constexpr const char* SpanBeginType = "Trace.SpanBegin";
    static constexpr const char* SpanEndType = "Trace.SpanEnd";
    static constexpr const char* FrameBeginType = "Trace.FrameBegin";
    static constexpr const char* FrameEndType = "Trace.FrameEnd";
    static constexpr const char* ContextDefinitionType = "Trace.ContextDefinition";
    static constexpr const char* ContextPushType = "Trace.ContextPush";
    static constexpr const char* ContextPopType = "Trace.ContextPop";

    // ---- 이름 표 ----
    // ScopeName(정의): u32 이름 id | u8 잘림, 가변: 이름(String). 이름이 MaxScopeNameBytes를 넘으면
    // 앞부분만 두고 잘림을 켠다. 같은 이름은 세션 안에서 id 하나다(결정 T4).
    static constexpr usize ScopeNameIdOffset = 0;
    static constexpr usize ScopeNameTruncatedOffset = 4;
    static constexpr usize ScopeNameFixedBytes = 5;
    static constexpr usize MaxScopeNameBytes = 1024;

    // ---- CPU 구간 배치 ----
    // CpuScopeBatch(스레드 스트림): 가변 UInt8Array 하나. 사건 열이며 각 사건은
    //   시작: varint((Δtick << 2) | ScopeBeginTag) varint(이름 id)
    //   끝:   varint(Δtick << 2)
    // 이다. Δtick은 같은 배치의 앞 사건 tick과의 차이이고, 배치 첫 사건은 절대 tick이다. varint는
    // 7비트씩 낮은 쪽부터 쓰고 최상위 비트가 "이어짐"이다(LEB128 부호 없음).
    static constexpr usize ScopeBatchBytes = 256;
    static constexpr uint64 ScopeBeginTag = 1;
    static constexpr uint64 ScopeTagMask = 3;
    static constexpr uint32 ScopeTagBits = 2;

    // ---- 카운터 ----
    // CounterDefinition(정의): u32 id | u8 종류(0 정수, 1 실수) | u8 단위(0 개수, 1 byte, 2 백분율)
    // | u8 방식(0 바뀔 때만, 1 매번), 가변: 이름(String)
    static constexpr usize CounterDefinitionIdOffset = 0;
    static constexpr usize CounterDefinitionKindOffset = 4;
    static constexpr usize CounterDefinitionUnitOffset = 5;
    static constexpr usize CounterDefinitionModeOffset = 6;
    static constexpr usize CounterDefinitionFixedBytes = 7;
    // CounterInteger·CounterFloat(스레드 스트림, 순서 번호): u64 tick | u32 id | i64 또는 f64 값
    static constexpr usize CounterValueTickOffset = 0;
    static constexpr usize CounterValueIdOffset = 8;
    static constexpr usize CounterValueOffset = 12;
    static constexpr usize CounterValueFixedBytes = 20;

    // ---- 표지 ----
    // MarkSite(정의): u32 호출 지점 id | u32 줄, 가변: 형식 문자열(String) | 파일(String)
    static constexpr usize MarkSiteIdOffset = 0;
    static constexpr usize MarkSiteLineOffset = 4;
    static constexpr usize MarkSiteFixedBytes = 8;
    // Mark(스레드 스트림): u64 tick | u32 호출 지점 id, 가변: 형식 인자(UInt8Array). 인자마다 u8
    // tag와 값이다. Int64·UInt64·Float64는 8 byte, Bool은 1 byte, String은 u32 길이 + byte다.
    static constexpr usize MarkTickOffset = 0;
    static constexpr usize MarkSiteOffset = 8;
    static constexpr usize MarkFixedBytes = 12;
    static constexpr usize MaxMarkArgumentBytes = 512;
    static constexpr uint8 MarkArgumentInt64 = 1;
    static constexpr uint8 MarkArgumentUInt64 = 2;
    static constexpr uint8 MarkArgumentFloat64 = 3;
    static constexpr uint8 MarkArgumentBool = 4;
    static constexpr uint8 MarkArgumentString = 5;

    // ---- 시간 띠 ----
    // SpanBegin(스레드 스트림): u64 tick | u64 띠 id | u32 이름 id. SpanEnd: u64 tick | u64 띠 id.
    // 띠 id는 세션 안에서 1부터 매긴다. 시작과 끝은 id로만 짝짓는다(같은 이름의 띠가 겹쳐도 된다).
    static constexpr usize SpanTickOffset = 0;
    static constexpr usize SpanIdOffset = 8;
    static constexpr usize SpanNameOffset = 16;
    static constexpr usize SpanBeginFixedBytes = 20;
    static constexpr usize SpanEndFixedBytes = 16;

    // ---- 프레임 ----
    // FrameBegin·FrameEnd(스레드 스트림): u64 tick | u64 프레임 번호 | u8 종류(0 게임, 1 렌더)
    static constexpr usize FrameTickOffset = 0;
    static constexpr usize FrameNumberOffset = 8;
    static constexpr usize FrameKindOffset = 16;
    static constexpr usize FrameFixedBytes = 17;

    // ---- 문맥 ----
    // ContextDefinition(정의): u32 문맥 id | u64 세션 | u64 task | u64 에셋 | u64 entity | u64 요청
    static constexpr usize ContextDefinitionIdOffset = 0;
    static constexpr usize ContextDefinitionValuesOffset = 4;
    static constexpr usize ContextValueCount = 5;
    static constexpr usize ContextDefinitionFixedBytes = 4 + ContextValueCount * 8;
    // ContextPush(스레드 스트림): u64 tick | u32 문맥 id. 새 문맥을 쌓을 때와 저장한 문맥을 복원할
    // 때 같다(복원은 id만 기록한다). ContextPop: u64 tick.
    static constexpr usize ContextPushTickOffset = 0;
    static constexpr usize ContextPushIdOffset = 8;
    static constexpr usize ContextPushFixedBytes = 12;
    static constexpr usize ContextPopFixedBytes = 8;
    // 스레드 문맥 스택의 깊이 상한이다. 넘친 쌓기는 스택에 들어가지 않고 기록도 하지 않는다.
    static constexpr usize MaxContextDepth = 16;

    // ---- 스레드 정보 ----
    // 스레드 이름·묶음의 byte 상한이다. 넘으면 앞부분만 쓴다.
    static constexpr usize MaxThreadNameBytes = 63;
};

} // namespace rocks
