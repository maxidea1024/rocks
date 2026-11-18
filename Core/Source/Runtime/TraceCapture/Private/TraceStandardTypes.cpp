#include "TraceSessionInternal.h"

#include <TraceCapture/TraceLogSink.h>

// 세션이 Start 때 등록하는 표준 채널과 형이다(Docs/Design/TraceSystem.ko.md 6절). 모두 상수
// 초기화되는 정적 객체이고, 채널은 꺼진 상태로 등록한다(묶음·명령줄로 켜는 것은 TR5).
namespace rocks {

TraceChannel TraceCpuChannel{"cpu", "CPU scopes (CpuScope)"};
TraceChannel TraceCounterChannel{"counter", "Counter values"};
TraceChannel TraceMarkChannel{"mark", "Marks"};
TraceChannel TraceSpanChannel{"span", "Thread-independent time spans"};
TraceChannel TraceFrameChannel{"frame", "Game and render frame boundaries"};
TraceChannel TraceContextChannel{"context", "Correlation id context push and restore"};
TraceChannel TraceTaskChannel{"task", "Task lifetime, dependencies and waits"};
TraceChannel TraceStatsChannel{"stats", "Trace capture statistics (pool, output, losses)"};
TraceChannel TraceLogChannel{"log", "Log records (Trace.LogMessageSpec, Trace.LogMessage)"};

namespace detail {

namespace {

using Format = TraceStandardFormat;

constexpr TraceField ScopeNameFields[] = {
    {"id", TraceFieldType::UInt32, Format::ScopeNameIdOffset},
    {"truncated", TraceFieldType::Bool, Format::ScopeNameTruncatedOffset},
    {"name", TraceFieldType::String, TraceField::Variable},
};

constexpr TraceField LogMessageSpecFields[] = {
    {"eventId", TraceFieldType::UInt64, Format::LogSpecEventIdOffset},
    {"line", TraceFieldType::UInt32, Format::LogSpecLineOffset},
    {"level", TraceFieldType::UInt8, Format::LogSpecLevelOffset},
    {"category", TraceFieldType::String, TraceField::Variable},
    {"template", TraceFieldType::String, TraceField::Variable},
    {"file", TraceFieldType::String, TraceField::Variable},
    {"function", TraceFieldType::String, TraceField::Variable},
};

constexpr TraceField LogMessageFields[] = {
    {"tick", TraceFieldType::UInt64, Format::LogMessageTickOffset},
    {"thread", TraceFieldType::UInt64, Format::LogMessageThreadOffset},
    {"frame", TraceFieldType::UInt64, Format::LogMessageFrameOffset},
    {"eventId", TraceFieldType::UInt64, Format::LogMessageEventIdOffset},
    {"session", TraceFieldType::UInt64, Format::LogMessageCorrelationOffset},
    {"task", TraceFieldType::UInt64, Format::LogMessageCorrelationOffset + 1 * sizeof(uint64)},
    {"asset", TraceFieldType::UInt64, Format::LogMessageCorrelationOffset + 2 * sizeof(uint64)},
    {"entity", TraceFieldType::UInt64, Format::LogMessageCorrelationOffset + 3 * sizeof(uint64)},
    {"request", TraceFieldType::UInt64, Format::LogMessageCorrelationOffset + 4 * sizeof(uint64)},
    {"flags", TraceFieldType::UInt32, Format::LogMessageFlagsOffset},
    {"level", TraceFieldType::UInt8, Format::LogMessageLevelOffset},
    {"correlationMask", TraceFieldType::UInt8, Format::LogMessageCorrelationMaskOffset},
    {"truncated", TraceFieldType::Bool, Format::LogMessageTruncatedOffset},
    {"message", TraceFieldType::String, TraceField::Variable},
};

constexpr TraceField CpuScopeBatchFields[] = {
    {"events", TraceFieldType::UInt8Array, TraceField::Variable},
};

constexpr TraceField CounterDefinitionFields[] = {
    {"id", TraceFieldType::UInt32, Format::CounterDefinitionIdOffset},
    {"kind", TraceFieldType::UInt8, Format::CounterDefinitionKindOffset},
    {"unit", TraceFieldType::UInt8, Format::CounterDefinitionUnitOffset},
    {"mode", TraceFieldType::UInt8, Format::CounterDefinitionModeOffset},
    {"name", TraceFieldType::String, TraceField::Variable},
};

constexpr TraceField CounterIntegerFields[] = {
    {"tick", TraceFieldType::UInt64, Format::CounterValueTickOffset},
    {"id", TraceFieldType::UInt32, Format::CounterValueIdOffset},
    {"value", TraceFieldType::Int64, Format::CounterValueOffset},
};

constexpr TraceField CounterFloatFields[] = {
    {"tick", TraceFieldType::UInt64, Format::CounterValueTickOffset},
    {"id", TraceFieldType::UInt32, Format::CounterValueIdOffset},
    {"value", TraceFieldType::Float64, Format::CounterValueOffset},
};

constexpr TraceField MarkSiteFields[] = {
    {"id", TraceFieldType::UInt32, Format::MarkSiteIdOffset},
    {"line", TraceFieldType::UInt32, Format::MarkSiteLineOffset},
    {"format", TraceFieldType::String, TraceField::Variable},
    {"file", TraceFieldType::String, TraceField::Variable},
};

constexpr TraceField MarkFields[] = {
    {"tick", TraceFieldType::UInt64, Format::MarkTickOffset},
    {"site", TraceFieldType::UInt32, Format::MarkSiteOffset},
    {"arguments", TraceFieldType::UInt8Array, TraceField::Variable},
};

constexpr TraceField SpanBeginFields[] = {
    {"tick", TraceFieldType::UInt64, Format::SpanTickOffset},
    {"span", TraceFieldType::UInt64, Format::SpanIdOffset},
    {"name", TraceFieldType::UInt32, Format::SpanNameOffset},
};

constexpr TraceField SpanEndFields[] = {
    {"tick", TraceFieldType::UInt64, Format::SpanTickOffset},
    {"span", TraceFieldType::UInt64, Format::SpanIdOffset},
};

constexpr TraceField FrameFields[] = {
    {"tick", TraceFieldType::UInt64, Format::FrameTickOffset},
    {"frame", TraceFieldType::UInt64, Format::FrameNumberOffset},
    {"kind", TraceFieldType::UInt8, Format::FrameKindOffset},
};

constexpr TraceField ContextDefinitionFields[] = {
    {"id", TraceFieldType::UInt32, Format::ContextDefinitionIdOffset},
    {"session", TraceFieldType::UInt64, Format::ContextDefinitionValuesOffset},
    {"task", TraceFieldType::UInt64, Format::ContextDefinitionValuesOffset + 8},
    {"asset", TraceFieldType::UInt64, Format::ContextDefinitionValuesOffset + 16},
    {"entity", TraceFieldType::UInt64, Format::ContextDefinitionValuesOffset + 24},
    {"request", TraceFieldType::UInt64, Format::ContextDefinitionValuesOffset + 32},
};

constexpr TraceField ContextPushFields[] = {
    {"tick", TraceFieldType::UInt64, Format::ContextPushTickOffset},
    {"id", TraceFieldType::UInt32, Format::ContextPushIdOffset},
};

constexpr TraceField TaskFields[] = {
    {"tick", TraceFieldType::UInt64, Format::TaskTickOffset},
    {"task", TraceFieldType::Id, Format::TaskIdOffset},
    {"related", TraceFieldType::Id, Format::TaskRelatedOffset},
    {"kind", TraceFieldType::UInt8, Format::TaskKindOffset},
    {"error", TraceFieldType::String, TraceField::Variable},
};

constexpr TraceField StatsFields[] = {
    {"tick", TraceFieldType::UInt64, Format::StatsTickOffset},
    {"poolInUse", TraceFieldType::UInt64, Format::StatsPoolInUseOffset},
    {"poolPeak", TraceFieldType::UInt64, Format::StatsPoolPeakOffset},
    {"poolLimit", TraceFieldType::UInt64, Format::StatsPoolLimitOffset},
    {"outputBytes", TraceFieldType::UInt64, Format::StatsOutputBytesOffset},
    {"definitionStore", TraceFieldType::UInt64, Format::StatsDefinitionStoreOffset},
    {"lostRecords", TraceFieldType::UInt64, Format::StatsLostRecordsOffset},
    {"definitionsDropped", TraceFieldType::UInt64, Format::StatsDefinitionsDroppedOffset},
    {"recentBytes", TraceFieldType::UInt64, Format::StatsRecentBytesOffset},
    {"recentDropped", TraceFieldType::UInt64, Format::StatsRecentDroppedOffset},
};

constexpr TraceField ContextPopFields[] = {
    {"tick", TraceFieldType::UInt64, Format::ContextPushTickOffset},
};

} // namespace

TraceEventType TraceScopeNameType{Format::ScopeNameType, TraceCpuChannel,
                                  Format::ScopeNameFixedBytes, ScopeNameFields,
                                  TraceEventFlags::Definition};
TraceEventType TraceCpuScopeBatchType{Format::CpuScopeBatchType, TraceCpuChannel, 0,
                                      CpuScopeBatchFields};
TraceEventType TraceCounterDefinitionType{Format::CounterDefinitionType, TraceCounterChannel,
                                          Format::CounterDefinitionFixedBytes,
                                          CounterDefinitionFields, TraceEventFlags::Definition};
TraceEventType TraceCounterIntegerType{Format::CounterIntegerType, TraceCounterChannel,
                                       Format::CounterValueFixedBytes, CounterIntegerFields,
                                       TraceEventFlags::Ordered};
TraceEventType TraceCounterFloatType{Format::CounterFloatType, TraceCounterChannel,
                                     Format::CounterValueFixedBytes, CounterFloatFields,
                                     TraceEventFlags::Ordered};
TraceEventType TraceMarkSiteType{Format::MarkSiteType, TraceMarkChannel, Format::MarkSiteFixedBytes,
                                 MarkSiteFields, TraceEventFlags::Definition};
TraceEventType TraceMarkEventType{Format::MarkType, TraceMarkChannel, Format::MarkFixedBytes,
                                  MarkFields};
TraceEventType TraceSpanBeginType{Format::SpanBeginType, TraceSpanChannel,
                                  Format::SpanBeginFixedBytes, SpanBeginFields};
TraceEventType TraceSpanEndType{Format::SpanEndType, TraceSpanChannel, Format::SpanEndFixedBytes,
                                SpanEndFields};
TraceEventType TraceFrameBeginType{Format::FrameBeginType, TraceFrameChannel,
                                   Format::FrameFixedBytes, FrameFields};
TraceEventType TraceFrameEndType{Format::FrameEndType, TraceFrameChannel, Format::FrameFixedBytes,
                                 FrameFields};
TraceEventType TraceContextDefinitionType{Format::ContextDefinitionType, TraceContextChannel,
                                          Format::ContextDefinitionFixedBytes,
                                          ContextDefinitionFields, TraceEventFlags::Definition};
TraceEventType TraceContextPushType{Format::ContextPushType, TraceContextChannel,
                                    Format::ContextPushFixedBytes, ContextPushFields};
TraceEventType TraceContextPopType{Format::ContextPopType, TraceContextChannel,
                                   Format::ContextPopFixedBytes, ContextPopFields};
TraceEventType TraceTaskEventType{Format::TaskType, TraceTaskChannel, Format::TaskFixedBytes,
                                  TaskFields, TraceEventFlags::Ordered};
TraceEventType TraceStatsType{Format::StatsType, TraceStatsChannel, Format::StatsFixedBytes,
                              StatsFields};
TraceEventType TraceLogMessageSpecType{Format::LogMessageSpecType, TraceLogChannel,
                                       Format::LogSpecFixedBytes, LogMessageSpecFields,
                                       TraceEventFlags::Definition};
TraceEventType TraceLogMessageType{Format::LogMessageType, TraceLogChannel,
                                   Format::LogMessageFixedBytes, LogMessageFields,
                                   TraceEventFlags::Ordered};

Result<void> RegisterStandardTraceTypes(TraceSession& session) noexcept {
    TraceChannel* const channels[] = {&TraceCpuChannel,  &TraceCounterChannel, &TraceMarkChannel,
                                      &TraceSpanChannel, &TraceFrameChannel,   &TraceContextChannel,
                                      &TraceTaskChannel, &TraceStatsChannel,   &TraceLogChannel};
    for (TraceChannel* channel : channels) {
        if (auto registered = session.RegisterChannel(*channel, false); !registered) {
            return registered;
        }
    }

    TraceEventType* const types[] = {
        &TraceScopeNameType,      &TraceCpuScopeBatchType,  &TraceCounterDefinitionType,
        &TraceCounterIntegerType, &TraceCounterFloatType,   &TraceMarkSiteType,
        &TraceMarkEventType,      &TraceSpanBeginType,      &TraceSpanEndType,
        &TraceFrameBeginType,     &TraceFrameEndType,       &TraceContextDefinitionType,
        &TraceContextPushType,    &TraceContextPopType,     &TraceTaskEventType,
        &TraceStatsType,          &TraceLogMessageSpecType, &TraceLogMessageType};
    for (TraceEventType* type : types) {
        if (auto registered = session.RegisterEventType(*type); !registered) {
            return registered;
        }
    }

    return Result<void>::Success();
}

} // namespace detail

} // namespace rocks
