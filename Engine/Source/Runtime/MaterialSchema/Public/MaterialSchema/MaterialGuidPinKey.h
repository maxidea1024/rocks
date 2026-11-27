#pragma once

#include <Base/Guid.h>
#include <Core/Result.h>
#include <Core/Text/Name.h>
#include <Core/Text/NamePool.h>
#include <Core/Text/StringView.h>

// Guid로 이름 붙인 동적 핀 키다(Docs/Design/MaterialSystem 19.13.3절): prefix 뒤에 Guid high·low를
// 소문자 16진 32자리로 붙인다. 함수 호출 입출력(P_<id>)과 속성 핀(A_<id>)이 쓰고, 번역기(6.12절
// MS2e2)도 같은 키로 호출 노드 핀을 찾는다.
//
// 사용 예:
//   auto key = MaterialGuidPinKey(names, "P_", input.id); // "P_0123…"
//
// 오류: prefix가 MaxMaterialGuidPinKeyPrefixBytes를 넘으면 InvalidArgument, 그 밖은 names.TryIntern
// 오류다.
namespace rocks::material {

// prefix 최대 byte 수다.
inline constexpr usize MaxMaterialGuidPinKeyPrefixBytes = 8;

ROCKS_MATERIALSCHEMA_API Result<Name> MaterialGuidPinKey(NamePool& names, StringView prefix,
                                                         Guid id) noexcept;

} // namespace rocks::material
