#pragma once

#include <Base/Types.h>

// 에셋 너겟 요청 정책이다(Docs/Design/AssetSystem.ko.md 9.1절 결정 14 — UE 5.8 ECachePolicy와 같은
// 비트). 요청 항목마다 준다.
//
// - Query: QueryLocal(로컬 backend·Memory 조회)·QueryRemote(원격 — A3에는 원격이 없어 QueryRemote만
//   켠 Get은 Miss).
// - Store: StoreLocal·StoreRemote(StoreLocal을 함의한다). 둘 다 없으면 Put은 저장하지 않고
//   Dropped(오류 없음)다.
// - SkipData: 값을 읽지 않는다(Record만). SkipMeta: meta를 비운다. PartialRecord: 청한 값 일부만
//   있어도 Partial로 돌려준다(아니면 Miss).
// - KeepAlive: Memory 적중에서도 backend 사용 시각을 갱신한다(backend 적중은 늘 갱신한다).
//
// 사용 예:
//   const NuggetPolicy policy = NuggetPolicy::Default | NuggetPolicy::SkipData;
//   if (HasAnyNuggetPolicy(policy, NuggetPolicy::SkipData)) { ... }
namespace rocks {

enum class NuggetPolicy : uint32 {
    None = 0,
    QueryLocal = 1u << 0,
    QueryRemote = 1u << 1,
    StoreLocal = 1u << 2,
    StoreRemote = 1u << 3,
    SkipData = 1u << 4,
    SkipMeta = 1u << 5,
    PartialRecord = 1u << 6,
    KeepAlive = 1u << 7,
    Query = QueryLocal | QueryRemote,
    Store = StoreLocal | StoreRemote,
    Default = Query | Store
};

constexpr NuggetPolicy operator|(NuggetPolicy left, NuggetPolicy right) noexcept {
    return static_cast<NuggetPolicy>(static_cast<uint32>(left) | static_cast<uint32>(right));
}

constexpr NuggetPolicy operator&(NuggetPolicy left, NuggetPolicy right) noexcept {
    return static_cast<NuggetPolicy>(static_cast<uint32>(left) & static_cast<uint32>(right));
}

constexpr NuggetPolicy operator~(NuggetPolicy value) noexcept {
    return static_cast<NuggetPolicy>(~static_cast<uint32>(value));
}

// bits 중 하나라도 켜져 있으면 true다.
constexpr bool HasAnyNuggetPolicy(NuggetPolicy policy, NuggetPolicy bits) noexcept {
    return (static_cast<uint32>(policy) & static_cast<uint32>(bits)) != 0;
}

} // namespace rocks
