#include "TraceSessionInternal.h"

// 세션이 Start 때 등록하는 표준 채널과 형이다(Docs/Design/TraceSystem.ko.md 6절). 모두 상수
// 초기화되는 정적 객체이고, 채널은 꺼진 상태로 등록한다(묶음·명령줄로 켜는 것은 TR5).
namespace rocks {

TraceChannel TraceCpuChannel{"cpu", "CPU 구간(CpuScope)"};
TraceChannel TraceCounterChannel{"counter", "카운터 값"};
TraceChannel TraceMarkChannel{"mark", "표지"};
TraceChannel TraceSpanChannel{"span", "스레드와 무관한 시간 띠"};
TraceChannel TraceFrameChannel{"frame", "게임·렌더 프레임 경계"};
TraceChannel TraceContextChannel{"context", "상관 id 문맥의 쌓기·복원"};
TraceChannel TraceTaskChannel{"task", "task 수명·의존·기다림"};

namespace detail {

namespace {

using Format = TraceStandardFormat;

constexpr TraceField ScopeNameFields[] = {
    {"id", TraceFieldType::UInt32, Format::ScopeNameIdOffset},
    {"truncated", TraceFieldType::Bool, Format::ScopeNameTruncatedOffset},
    {"name", TraceFieldType::String, TraceField::Variable},
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

Result<void> RegisterStandardTraceTypes(TraceSession& session) noexcept {
    TraceChannel* const channels[] = {&TraceCpuChannel,  &TraceCounterChannel, &TraceMarkChannel,
                                      &TraceSpanChannel, &TraceFrameChannel,   &TraceContextChannel,
                                      &TraceTaskChannel};
    for (TraceChannel* channel : channels) {
        if (auto registered = session.RegisterChannel(*channel, false); !registered) {
            return registered;
        }
    }

    TraceEventType* const types[] = {
        &TraceScopeNameType,      &TraceCpuScopeBatchType, &TraceCounterDefinitionType,
        &TraceCounterIntegerType, &TraceCounterFloatType,  &TraceMarkSiteType,
        &TraceMarkEventType,      &TraceSpanBeginType,     &TraceSpanEndType,
        &TraceFrameBeginType,     &TraceFrameEndType,      &TraceContextDefinitionType,
        &TraceContextPushType,    &TraceContextPopType,    &TraceTaskEventType};
    for (TraceEventType* type : types) {
        if (auto registered = session.RegisterEventType(*type); !registered) {
            return registered;
        }
    }

    return Result<void>::Success();
}

} // namespace detail

} // namespace rocks
