#pragma once

#include <Base/Types.h>
#include <Core/Text/StringView.h>

// 암호 모듈의 실행 정보다(Docs/Design/Crypto.ko.md). 초기화는 모든 공개 함수가 처음 불릴 때 한 번
// 일어나므로 따로 부를 필요가 없다. 이 헤더는 진단·시험용으로 실제 선택된 구현과 하드웨어 경로를
// 알려 준다.
//
// 사용 예(시작 로그에 한 번 남기기):
//   const crypto::CryptoRuntimeInfo& info = crypto::GetCryptoRuntimeInfo();
//   Log("libsodium %.*s, rng %.*s, aes-ni %d", ..., info.sodiumAesni);
//
// 초기화 계약:
// - 첫 호출은 libsodium의 난수 구현을 Rocks OS 난수(platform::TryFillSecureRandom)로 바꾸고
//   (randombytes_set_implementation), misuse handler를 Fatal로 연결한 뒤 sodium_init을
//   부른다. 난수 교체는 libsodium 요구대로 init 전에 한다. handler를 먼저 두는 것은 init 안의
//   misuse도 Fatal로 보내기 위해서다. Crypto 밖에서 이미 init했다면(순서가 깨졌을 수 있다)
//   Fatal이다.
// - 여러 스레드가 동시에 처음 불러도 하나가 초기화를 끝낼 때까지 나머지가 기다린다(함수 안
//   static 초기화). 그래서 init 전에 libsodium의 기본(soft) 구현이 선택되는 일이 없다.
// - sodium_init 실패는 Fatal이다. 암호 기능 없이 계속하지 않는다.
namespace rocks::crypto {

struct CryptoRuntimeInfo {
    // libsodium 버전 문자열이다(예: "1.0.22"). 정적 수명이다.
    StringView sodiumVersion;
    // libsodium이 쓰는 난수 구현 이름이다. Rocks OS 난수로 바뀌었으면 "rocks-os"다.
    StringView randomImplementation;

    // libsodium이 실행 중에 감지한 CPU 기능이다(sodium_runtime_has_*). PlatformBase CpuFeatures와
    // 다르면 libsodium 빌드 정의가 빠져 하드웨어 경로가 조용히 꺼진
    // 것이다(Docs/Design/Crypto.ko.md 6.2절, 시험이 대조한다). ARM의 armCrypto는 libsodium이 AES
    // 기능 하나로 판정한다(PMULL은 보지 않는다).
    bool sodiumAesni = false;
    bool sodiumPclmul = false;
    bool sodiumAvx = false;
    bool sodiumAvx2 = false;
    bool sodiumArmCrypto = false;
};

// 처음 호출할 때 암호 모듈을 초기화하고 이후 같은 값을 돌려준다. 여러 스레드에서 불러도 된다.
const CryptoRuntimeInfo& GetCryptoRuntimeInfo() noexcept;

} // namespace rocks::crypto
