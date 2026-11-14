#pragma once

#include <Crypto/Kdf.h>

// 엔진이 쓰는 KDF context 등록부다(Docs/Design/Crypto.ko.md 10.1절). context는 용도마다 하나이고
// 여기 한 곳에 모아 겹치지 않게 한다.
//
// - "Rk"로 시작하는 context는 엔진 예약이다. 앱은 다른 접두를 쓴다.
// - 새 context를 더하면 All에도 넣는다. 시험이 모두 다른지와 접두를 확인한다.
// - HKDF info label(예: "rocks.segment.v1")도 생기면 여기에 둔다.
namespace rocks::crypto::labels {

// KeyBlob wrap 키 파생(10.4절)이다.
inline constexpr KdfContext KeyBlob("RkKeyBlb");

// 등록된 context 전체다(시험이 훑는다).
inline constexpr KdfContext All[] = {KeyBlob};

} // namespace rocks::crypto::labels
