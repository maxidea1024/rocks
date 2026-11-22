#pragma once

#include <Core/Containers/Array.h>
#include <Core/Result.h>
#include <Object/AssetId.h>

// SCC Record의 값 번호와 meta다(Docs/Design/AssetSystem.ko.md 9.1절 결정 3·15 — Rockpile 12.3절).
// SCC 하나를 Record 하나(key = SCC key)로 두고, 값 번호는 (구성원 순번 << 16) | 부분 번호, meta는
// 구성원 AssetId 목록(16 byte씩, 순서 = 구성원 순번)이다.
//
// 사용 예:
//   Array<uint8> meta(allocator);
//   WriteNuggetSccMeta(members, meta);                        // members[i]가 순번 i
//   const uint32 id = NuggetSccValueId(1, 0);                  // 구성원 1의 부분 0
//   auto index = FindNuggetSccMember(meta.View(), members[1]); // 1
namespace rocks {

// 구성원 상한과 부분 번호 비트 수다.
inline constexpr usize NuggetSccMaxMembers = 4096;
inline constexpr uint32 NuggetSccPartBits = 16;
inline constexpr uint32 NuggetSccMaxParts = 1u << NuggetSccPartBits;
// meta의 구성원 하나 크기다(AssetId high → low, 각각 big-endian).
inline constexpr usize NuggetSccMemberBytes = 16;

// 값 번호다. member < NuggetSccMaxMembers, part < NuggetSccMaxParts(넘으면 assert — 호출자 결함).
uint32 NuggetSccValueId(uint32 member, uint32 part) noexcept;

// 값 번호의 구성원 순번이다.
constexpr uint32 NuggetSccMemberOf(uint32 valueId) noexcept {
    return valueId >> NuggetSccPartBits;
}

// 구성원 목록을 meta로 쓴다(meta를 비우고 채운다). 오류: 빈 목록·null·중복 InvalidArgument,
// NuggetSccMaxMembers 초과 CapacityExceeded, 할당 실패.
Result<void> WriteNuggetSccMeta(Span<const AssetId> members, Array<uint8>& meta) noexcept;

// meta에서 asset의 순번을 찾는다. 오류: 크기가 16 byte의 배수가 아니거나 구성원이 상한을 넘음
// InvalidArgument, 없으면 NotFound.
Result<uint32> FindNuggetSccMember(Span<const uint8> meta, AssetId asset) noexcept;

} // namespace rocks
