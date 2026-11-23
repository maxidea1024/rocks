# Crypto — 범용 암호 기반(libsodium + HW 전용 AES 경량 계층)

기준일: 2026-11-14, 개정 1(교차검증 반영). 엔진·도구·게임이 함께 쓰는 범용 암호 기반을 Core 계층 모듈 `Crypto`
(`Core/Source/Runtime/Crypto`)로 둔다. 외부 암호 의존은 libsodium 하나다. libsodium에 없는 AES 모드는 HW 가속 전용
경량 계층으로 채우며, 이 계층은 libsodium의 AES-GCM intrinsics 구현에서 파생한다. AES 명령이 없는 기기에서는 AES를
제공하지 않는다. 이 문서는 세 갈래(독립 설계·반례 검토·사실 확인)의 병렬 교차검증을 거쳤다(15절). 구현·실행 검증 결과는
[구현 상태](../Implementation/FoundationStatus.ko.md)와 별도 검증 기록을 따른다.

## 0. 핵심 결정

- **역할 분담.** libsodium이 primitive 전부와 AES-256-GCM 일회성을 맡는다. AES-128-GCM·GCM 멀티파트·CTR·CMAC는 Rocks의
  HW 전용 경량 계층이 맡는다. 다른 외부 암호 라이브러리는 들이지 않는다. 그 밖의 자체 코드는 어댑터·디스패치·형식 조립·
  키 수명 관리다.
- **API.** 입력은 `Span<const uint8>`, 출력은 호출자가 준 `Span<uint8>`이고 반환값은 쓴 byte 수다. 고정 길이
  결과(digest·서명·공개키)는 값 타입으로 돌려주고 `View()`로 Span을 얻는다. 예외·STL·외부 헤더는 공개 API에 나오지 않는다.
- **스트리밍.** 증분(Update/Finish), 메시지 스트림(libsodium secretstream), 세그먼트 스트림(STREAM 구성 + key commitment,
  임의 접근·병렬)의 세 층이다. 복호 스트림은 검증 전 평문을 내보내지 않는다.
- **가속.** AES-NI·PCLMULQDQ와 ARMv8 AES·PMULL intrinsics를 함수 단위 target 속성으로 컴파일한다. CPU 기능은
  PlatformBase가 한 번 조사하고, libsodium 내부 감지가 그 값과 같은지를 시험으로 고정한다.
- **HW 없는 기기.** AES·AEGIS는 `Unsupported`다. 소프트웨어 AES는 두지 않는다. 기본 AEAD는 모든 기기에서
  XChaCha20-Poly1305다.
- **실패.** `ErrorCode::AuthenticationFailed`를 새로 둔다. 출력에 쓰기 시작한 뒤 실패하면 출력을 0으로 지우고, 스트림
  오류는 Reset 전까지 유지된다(fail-closed).
- **libsodium 경고.** 저장소 경고 프로필에서 남는 경고는 추적 가능한 수정으로 없앤다(2026-11-14 사용자 결정, 5.1절). upstream에는 내지 않는다(2026-11-15 사용자 결정: Rocks가 libsodium을 따로 재배포하지 않으므로 코드 수정과 기록으로 충분하다). 경고 수정은 `Rocks.patch`(manifest `modifications`)에, AES-256-GCM `MESSAGEBYTES_MAX`가 구현이 받는 평문 상한보다 한 블록 큰 문제는 어댑터 상한(`MaxAes256GcmMessageBytes`, 2^36 − 48 byte)으로 막고 manifest `notes`에 적는다.

## 1. 범위

**1차**

- 난수(OS CSPRNG 단일 경로), 상수 시간 비교·소거, 상수 시간 hex·base64
- 해시: BLAKE2b, SHA-512, SHA3-256/512, SHAKE·TurboSHAKE(SHA-256은 Core `Sha256`, 결정 D7)
- MAC: BLAKE2b keyed, HMAC-SHA-256/512, AES-CMAC, Poly1305(hazmat)
- KDF: HKDF-SHA-256/512, BLAKE2b KDF / 비밀번호: Argon2id
- AEAD: XChaCha20-Poly1305, ChaCha20-Poly1305, AEGIS-256/128L, AES-256/128-GCM
- KEM: X-Wing(ML-KEM768 + X25519), ML-KEM768
- AES 모드(hazmat): CTR, CMAC
- 서명 Ed25519·Ed25519ph, X25519 kx, box·sealed box
- 스트리밍 세 층, 임의 접근, 비동기 분할
- 키 타입·키 직렬화, 잠금 메모리, KeyRing

**후속**

- OS 키 저장소(DPAPI·Keychain·Android Keystore)
- 서명된 청크 표·매니페스트 서명 형식
- 전송 채널 보호(재생 창, rekey 주기): Crypto 부품은 [SecureChannel](SecureChannel.ko.md) 3절로 구현(2026-11-15), 패킷·DoS는 NetCore
- AES-CBC·CCM·KW/KWP: 요구가 생기면 경량 계층에 더한다
- P-256 ECDSA/ECDH, RSA 검증: libsodium에 없어 필요해지면 별도 설계
- HPKE: libsodium 로드맵 항목이다

**범위 밖**

- TLS/DTLS, X.509 경로 검증(OS·HTTP 계층 몫)
- 코드 서명, DRM, 치트 방지
- FIPS 140 인증 경계
- 소프트웨어 AES, AES-XTS, AES-GCM-SIV

## 2. 보안 원칙

- **nonce는 고수준 API가 관리한다.** 무작위 nonce는 192bit 이상인 알고리즘(XChaCha20-Poly1305, AEGIS-256)에만 쓴다. 96bit nonce 알고리즘은 스트림별로 파생한 키에 카운터 nonce를 붙여서만 쓴다. 호출자가 nonce를 직접 주는 API는 `rocks::crypto::hazmat`에 둔다.
- **검증 전 평문을 내보내지 않는다.** 예외는 이름에 `Unverified`가 붙은 hazmat API(AES-GCM 멀티파트 복호) 하나다.
- **쓰기 시작한 뒤 실패하면 지운다.** 출력에 쓰기 시작한 뒤의 실패는 출력 Span 전체를 0으로 지우고, Append 변형은 길이를 되돌리면서 붙였던 영역도 지운다. 쓰기 전에 거부한 경우(길이·겹침·Unsupported)는 입력과 출력을 건드리지 않는다. 스트림 객체는 Reset 전까지 같은 오류를 유지한다.
- **키는 알고리즘에 묶인 move-only 객체다.** 같은 키 byte를 다른 알고리즘이나 용도에 쓰려면 KDF로 파생한다. 예외는 kx 세션 키다: libsodium crypto_kx와 byte가 같아야 해서 알고리즘이 키 byte에 묶이지 않으므로, 프로토콜이 알고리즘을 고정하거나 협상을 인증한다(8절). 복사 생성자는 없고, 리플렉션·직렬화·서식 출력 경로에 들어가지 않는다.
- **키는 ID로 찾는다.** GCM·ChaCha20-Poly1305·AEGIS 모두 key commitment를 가정하지 않는다. 그래서 형식은 키 ID를 기록하고, 키는 KeyRing에서 ID로 찾으며 기본적으로 시도 복호를 하지 않는다. 비밀번호 후보처럼 여러 키를 시도할 수밖에 없는 형식은 commitment를 둔다(9.5절).
- **비밀에 의존하는 분기·메모리 주소를 두지 않는다.** AES·AEGIS는 HW 명령이 있을 때만 쓰고, 소프트웨어 AES와 AEGIS soft 경로는 쓰지 않는다(HW가 없으면 `Unsupported`). 예외는 Argon2id다. 첫 pass 후반부터 데이터 의존 주소로 메모리를 읽는 것이 알고리즘 설계다. CPU 쪽 한계(Apple M1·M2의 데이터 메모리 의존 prefetcher)는 6.3절에서 다룬다.
- **암호 연산은 libsodium이 맡는다.** 예외는 AES 경량 계층 하나다. 이 계층은 libsodium의 AES-GCM 구현에서 파생하고 HW 명령만 쓴다(5.2절). 나머지 자체 코드는 길이 검사·디스패치·형식 조립·메모리 관리이고, 비교·소거도 `sodium_memcmp`·`sodium_memzero`를 쓴다.
- **배포본에 넣은 대칭 키는 비밀이 아니다.** 클라이언트에 실린 키는 난독화일 뿐이다. 배포 콘텐츠의 무결성은 Ed25519 공개키 서명으로 확인하고, 신뢰 공개키를 여러 개 두어 회전한다.
- **오류는 출처로 나눈다.** 호출자가 소유한 버퍼(키·nonce·출력)의 길이 오류는 `InvalidArgument`다. 메시지에서 온 byte(sealed·tag·서명·헤더)의 형식 오류, tag·MAC·서명·commitment 불일치, 절단은 모두 `AuthenticationFailed` 하나로 모은다. tag는 정확히 TagBytes여야 하며, 잘린 tag는 받지 않는다. 비밀과 무관한 공개 필드의 "모르는 버전·알고리즘"만 `Unsupported`로 구분한다(진단용이며 oracle이 아니다).

## 3. 구성과 배치

```text
사용처(예)      파일·세이브 암호화 | 전송 채널 | 콘텐츠·업데이트 서명 검증 | 토큰·비밀번호 | 도구: 키 생성·서명
공개 API        rocks::crypto — 일회성(Aead·Hash·Mac·Kdf·Sign·Kx·Kem) | 스트리밍(Hasher·SecretStream·SegmentStream)
  (자체)        | 키(SecretBytes·키 타입·KeyRing·KeyBlob) | AsyncCrypto(Task 분할) | hazmat(nonce 지정 AEAD·AES 모드·keystream)
어댑터          Span·Result 변환·길이 검사·출력 소거 | 디스패치(알고리즘 × CPU → 구현) | 세그먼트 형식 조립(9.5절)
  (자체)        | SecretArena | AES 경량 계층(HW 전용, libsodium 파생)
외부 원본       libsodium 1.0.22-stable(ISC, 추적 가능한 수정 포함) | Wycheproof(Apache-2.0, 시험 데이터)
기반            Core(Span 보강·Result·ErrorCode·StreamProgress·Allocator·Delegate)
                | PlatformBase(CpuFeatures 확장·SecureRandom·VirtualMemory) | Task(WorkerScheduler)
```

```text
Core/Source/ThirdParty/
  Sodium/       Sodium.Build.cs  ThirdParty.json  libsodium-1.0.22-stable-<commit>/{LICENSE, src/libsodium/..., builds/msvc/version.h}  Glue/sodium/version.h
  Wycheproof/   ThirdParty.json(kind: data)  wycheproof-<commit>/testvectors_v1/*.json
Core/Source/Runtime/Crypto/
  Crypto.Build.cs
  Public/Crypto/   Crypto.h  Bytes.h  Random.h  Hash.h  Mac.h  Kdf.h  PasswordHash.h  Aead.h  AesModes.h(hazmat AES 모드)
                   SecretStream.h  SegmentStream.h  Signature.h  KeyExchange.h  Kem.h  Secret.h  KeyRing.h  AsyncCrypto.h
                   Channel.h(보안 채널 Noise NK·datagram)
                   Hazmat/{AeadNonce.h, StreamCipher.h, Poly1305.h}(후속)
  Private/         Sodium/(libsodium 어댑터)  AesHazmat.cpp(AesModes.h 어댑터)
                   Aes/{AesKernels.h, AesModesInternal.h, AesX64.cpp, AesArm64.cpp, AesModes.cpp, Derivation.json}(경량 계층)
                   Dispatch.cpp  SecretStream.cpp  SegmentFormat.cpp  SegmentStream{Sequential,RandomAccess}.cpp  SecretArena.cpp
                   Channel{Handshake,Datagram}.cpp  ChannelInternal.h  ChannelTestAccess.h(시험 전용)
  Tests/
```

- **의존.** Crypto의 PublicDependencies는 `Core`, `Task`, `Sodium`뿐이다. 외부 암호 라이브러리는 libsodium 하나다. Task는 `AsyncCrypto`만 쓴다. 정적 링크이므로 동기 API만 쓰는 모듈은 Task object를 링크하지 않는다. Compression 모듈과 같은 구성이다.
- **헤더 격리.** 외부 헤더는 Private에서만 include한다. 공개 헤더는 `sodium.h`나 intrinsics 헤더를 포함하지 않는다. 라이브러리 상태의 크기·정렬은 공개 헤더에 드러내지 않는다. 상태에는 키 확장 결과가 들어 있으므로 SecretArena 슬롯에 둔다(10.2절).
- **Core 모듈은 외부 의존 없이 유지한다.** Core `Sha256`(콘텐츠 동일성 해시, Rockpile R0의 하드웨어 경로 계획 포함)은 그대로 둔다. Crypto는 SHA-256을 공개 API로 다시 내놓지 않는다(결정 D7).
- **Compression과 독립.** 두 모듈을 잇는 것은 Core로 옮긴 `StreamProgress`와 호출자 코드다(9.6절).
- **단일 원천.** 난수·소거·상수 시간 비교는 Crypto API 한 곳에 둔다(난수는 내부적으로 PlatformBase SecureRandom). 다른 모듈은 자체 구현 대신 이 API를 쓴다.

## 4. 알고리즘 선정

| 용도 | 기본 | 대안 | 담당 | 근거·주의 |
|---|---|---|---|---|
| AEAD 범용 | XChaCha20-Poly1305 | AEGIS-256(HW일 때) | [libsodium] | 모든 CPU에서 상수 시간이다. nonce가 24 byte라 무작위로 만들어도 안전하다. |
| AEAD 상호운용 | AES-256-GCM | AES-128-GCM, ChaCha20-Poly1305(RFC 8439) | [libsodium] [경량 계층] | AES-256-GCM 일회성은 libsodium 원본, AES-128과 멀티파트는 경량 계층이 맡는다(결정 D4). HW가 없으면 `Unsupported`다. x86 조건은 AES-NI·PCLMULQDQ·**AVX**다(6.1절). |
| AEAD HW 최고속 | AEGIS-256 | AEGIS-128L | [libsodium] | tag는 32 byte로 고정이다. HW가 없으면 libsodium은 soft 구현으로 내려가는데, 공식 문서가 "부채널 보호를 보장하지 않는다"고 적는다. Rocks는 AES와 같은 규칙으로 `Unsupported`를 돌려준다(결정 D3). |
| 메시지 스트림 | secretstream XChaCha20-Poly1305 | — | [libsodium] | 태그(MESSAGE·PUSH·REKEY·FINAL) 자체도 암호화된다. 순서와 절단을 검출한다. |
| 세그먼트 스트림 | Rocks 세그먼트 형식 · ChaCha20-Poly1305 | 같은 형식 · AES-256-GCM(HW 기기군) | [형식] [연산] | 임의 접근과 병렬 처리가 된다. STREAM 구성(Tink·age와 같은 스트림별 키 파생 + 카운터 nonce + last 표시)에 key commitment·키 ID·매개변수 결합을 더했다(9.5절). |
| 해시 | BLAKE2b-256 | SHA-256/512, SHA3-256/512, SHAKE·TurboSHAKE | [libsodium] | SHA-3은 1.0.22, XOF는 1.0.21부터 들어 있다. 콘텐츠 동일성용 SHA-256은 Core `Sha256`이 맡는다. |
| MAC | BLAKE2b keyed | HMAC-SHA-256/512, AES-CMAC | [libsodium] [경량 계층] | 임의 길이 HMAC 키는 증분 API(`_init`)만 받는다. 일회성 `crypto_auth_hmacsha256`·`512`는 키가 32 byte로 고정이므로, 어댑터는 항상 증분 API를 쓴다. |
| KDF | HKDF-SHA-256 | HKDF-SHA-512, BLAKE2b KDF(64bit id + 8 byte context) | [libsodium] | libsodium HKDF의 상태 API는 extract에만 있고 expand는 일회성이다. 출력 상한은 255 × 해시 길이다. |
| 비밀번호 | Argon2id | — | [libsodium] | INTERACTIVE(2, 64 MiB) / MODERATE(3, 256 MiB) / SENSITIVE(4, 1 GiB) 프리셋을 쓰고, 입력 매개변수에 상한을 둔다. |
| 서명 | Ed25519 | Ed25519ph(스트리밍) | [libsodium] | ph 서명(dom2 접두 + SHA-512(m))은 Ed25519(m)과도, SHA-512(m)에 대한 순수 서명과도 호환되지 않으므로 서명 알고리즘 ID를 따로 둔다. 검증 규칙(비정규 S·small-order R·비정규/small-order 공개키 거부, cofactorless 검증식, `ED25519_COMPAT` 미정의)은 libsodium이 정본이며, 도구도 같은 Rocks 경로로 검증한다. 혼합 차수(torsion이 섞인) 공개키·R은 거부하지 않는다(부분군 검사가 필요하면 키 등록 때 따로 한다). |
| 키 캡슐화(PQ) | X-Wing | ML-KEM768 | [libsodium] | 1.0.22에 들어왔다. libsodium은 대부분의 용도에 X-Wing(ML-KEM768 + X25519 하이브리드)을 권한다. 1.0.22 구성은 draft-connolly-cfrg-xwing-kem-11과 같다(draft 부록 벡터로 고정, TLS X25519MLKEM768과는 호환되지 않는다). ML-KEM768 단독은 FIPS 203 전용 환경·상호운용에만 쓰고, 공유 비밀이 암호문·공개키에 묶이지 않으므로(MAL-BIND) 서명 기반 AKE 등에서는 transcript 묶기 파생(`TryDeriveBoundKdfKey`)으로 세션 키를 뽑는다. |
| 키 교환 · 공개키 암호화 | X25519(crypto_kx) · sealed box | box | [libsodium] | kx는 rx/tx 키를 분리하고 역할을 묶는다(세션 난수가 없어 같은 키 쌍이면 같은 키다). sealed box는 epk ‖ box 구조이고 오버헤드가 48 byte다. 키 쌍 타입을 용도별로 나눈다(KxKeyPair·BoxKeyPair, byte 형식은 libsodium과 같다). |
| 키 해시 | SipHash-2-4 | SipHash-2-4 128bit | [libsodium] | hash flooding 방어용이다. 128bit 출력판은 full 빌드에만 있다. |
| 키 wrap | XChaCha20-Poly1305(헤더 transcript에 묶은 파생 + commitment, keyId·wrapKeyId 기록, 10.4절) | AES-KW/KWP(RFC 3394/5649, 후속) | [libsodium] | KW는 상호운용 요구가 생기면 경량 계층에 더한다. |
| 저수준 AES(hazmat) | CTR | CMAC. CBC·CCM은 요구 시 | [경량 계층] | 상호운용 전용이다. CBC 복호와 KW unwrap은 역방향 라운드(AESDEC·AESIMC / AESD·AESIMC)가 필요해 요구가 생길 때 더한다. |
| 넣지 않음 | SHA-1·MD5(보안 용도), 소프트웨어 AES, AES-XTS·AES-GCM-SIV(libsodium에 없고 경량 계층 범위 밖), RC4·DES·3DES, scrypt |  |  |  |

## 5. libsodium과 AES 경량 계층

### 5.1 libsodium 1.0.22-stable

- **고정 대상.** point release 1.0.22(2026-04-09)가 아니라 그 위의 stable 브랜치 commit(1.0.22-stable)을 고정한다. 라이선스는 ISC다. 1.0.22에는 `HAVE_XGETBV_ASM`이 없어서, XGETBV는 `HAVE_AVX_ASM`(.S 어셈블리 경로 정의)일 때만 켜진다. 그러면 asm 없이 빌드한 Windows 밖 x86-64(Android x86_64)에서 AVX를 감지하지 못하고, AES-256-GCM·AEGIS aesni·AVX2 경로가 오류 없이 꺼진다. stable 브랜치는 이 정의를 따로 두었고, misuse handler를 전역 잠금을 잡은 채 부르던 문제도 고쳤다. libsodium 정책상 stable에는 새 기능·파괴적 변경 없이 수정만 들어간다. 출처는 JsonTestSuite처럼 GitHub commit archive URL과 SHA-256으로 고정한다(결정 D13).
- **full 빌드.** minimal 빌드는 `stream_xchacha20`·`siphashx24`·ed25519 core 등을 뺀다. hazmat keystream과 128bit SipHash를 쓰려면 full 빌드가 필요하다.

> **libsodium은 자체 빌드를 지원하지 않는다.** 그래서 `CONFIGURED`·`HAVE_*` 정의는 Compression 3.4절의 "공식 설정 지점만 쓴다"는 원칙 밖이다. zstd `MEM_STATIC`처럼 경고 정책 문서의 승인과 manifest `notes` 기록을 받는다. 정의 값은 대상별 upstream configure 결과에서 뽑아 고정하고, Compression 3.6절처럼 전체 번역 단위를 저장소 경고 프로필로 먼저 컴파일해 확인한다. 공식 문서는 사용자 지정 컴파일 플래그를 "untested and unsupported"라고 적는다. 이슈 #1090에서는 `HAVE_*` 정의가 빠져 `crypto_aead_aes256gcm_is_available()`이 0이 된 사례에 대해, 유지보수자가 "on your own"이라고 답했다. Clang으로 Windows MSVC ABI를 빌드하는 경로도 공식적으로 시험된 적이 없다(이슈 #648). Rocks는 빌드 정의를 아래처럼 고정하고, 하드웨어 경로가 실제로 켜졌는지를 시험(6.3절)으로 지킨다.

| 대상 | 정의·인자 | 이유 |
|---|---|---|
| 공통 | `NATIVE_LITTLE_ENDIAN`, `SODIUM_STATIC`(consumerDefines에도 둔다), 64 bit 대상 `HAVE_TI_MODE` | 정적 링크. ARM AES 경로는 little-endian 정의가 있어야 켜진다. `HAVE_TI_MODE`가 없으면 Poly1305·X25519·Ed25519가 32 bit limb 경로를 탄다(clang-cl도 `__int128`을 지원한다). |
| Windows(clang-cl) | 추가 없음. `_MSC_VER`가 정의되므로 `common.h`가 x86 `HAVE_*INTRIN_H`·`HAVE_WMMINTRIN_H`와 ARM64 `HAVE_ARMCRYPTO`를 스스로 정의한다. | 로컬 clang에서 `x86_64-pc-windows-msvc` 대상 매크로를 확인했다. `_xgetbv` 경로로 AVX도 감지된다. |
| macOS·Android | `CONFIGURED=1`, `HAVE_ATOMIC_OPS`, `HAVE_PTHREAD`, 플랫폼 `HAVE_*`(mmap·mlock·madvise·sysconf·getentropy·arc4random 등) | `CONFIGURED`가 없으면 "undocumented method" `#warning`이 나와 경고 0 정책에 걸린다. `utils.c`에도 `#warning Unknown page size`(aligned malloc은 있고 sysconf가 없을 때)가 있다. `HAVE_PTHREAD`와 `HAVE_ATOMIC_OPS`가 둘 다 없으면 `sodium_init`의 잠금이 no-op이 된다. |
| x86-64(Windows 외) | `HAVE_CPUID`, `HAVE_XGETBV_ASM`(stable에만 있음), `HAVE_{MM,EMM,PMM,TMM,SMM,AVX,AVX2,AVX512F,WMM}INTRIN_H` | 빠지면 AVX를 감지하지 못하고 AES-GCM이 "사용 불가"가 된다(오류 없이 꺼진다). |
| Android arm64 | `HAVE_ARMCRYPTO`, `HAVE_GETAUXVAL`, `HAVE_SYS_AUXV_H` | Android 대상은 `__ARM_FEATURE_AES`가 기본 정의되지 않는다. 그래서 런타임 감지 정의가 빠지면 AES-GCM은 쓸 수 없고 AEGIS는 soft로 내려간다. |
| asm | `HAVE_AMD64_ASM`·`HAVE_AVX_ASM`을 정의하지 않는다 | 빌드 도구가 `.S`를 지원하지 않고, Windows x64 ABI와도 맞지 않는다. 잃는 것은 x25519 sandy2x와 salsa20 xmm6 경로다. 영향은 실측으로 확인한다. |
| 컴파일 인자 | `-fno-strict-aliasing -fno-strict-overflow`(ThirdParty 모듈 한정, manifest `compileArguments`). LTO 끔(manifest `linkTimeOptimization: false`, Release ThinLTO에서 뺀다) | upstream configure와 같다. configure는 `-fno-strict-overflow`를 받으면 `-fwrapv`를 따로 주지 않는다. clang은 이를 `-fwrapv`로 바꾸며(clang 21부터 `-fwrapv-pointer` 포함), clang 20 이하(NDK)는 둘을 함께 주면 하나를 unused 경고로 거부한다. build.zig의 `-flax-vector-conversions`(configure는 gcc 4.3 미만에서만)·`-Werror=vla`(경고 정책을 바꾸는 인자)·`-fvisibility=hidden`(정적 링크에서는 의미 없음)은 넣지 않는다. LTO를 끄는 이유는 upstream이 LTO 없이 빌드·시험해 상수 시간 장벽과 CPU 클래스별 target 함수의 코드 생성을 그 형태로 검증했기 때문이다. 설치 문서는 sanitizer 전체를 켜지 말라고 한다(signed-integer-overflow는 예시다. 결정 D9). |
| SIMD 번역 단위 | 원본의 `#pragma clang attribute push(__attribute__((target("aes,avx,pclmul"))), apply_to = function)` | 함수 단위 target이라 전역 `-maes`가 필요 없다. 진단 지시문이 아니므로 manifest `pragmaDirectives`에 기록만 한다(Brotli `_Pragma`와 같은 처리). |
| version.h | Rocks 글루 `Glue/sodium/version.h`를 둔다. 내용은 `builds/msvc/version.h`(MINIMAL 미정의)와 같고, 원본 SHA-256을 manifest에 기록한다 | tree에 생성본이 없고, 헤더가 `sodium/version.h` 경로로 include하므로 include 경로 추가만으로는 안 된다. zig 빌드는 같은 파일을 그 위치로 복사한다. 원본 디렉터리는 byte 그대로 둔다. |

- **초기화.** 모든 공개 진입점이 대기하는 한 번 실행(blocking once)을 거친다. 기다리지 않는 test-and-set이면 다른 스레드가 init 전에 AEGIS·BLAKE2b·ChaCha의 기본(soft) 구현을 탈 수 있다. 구현 선택이 `sodium_init`에서 일어나기 때문이다. 순서는 `randombytes_set_implementation(Rocks)` → `sodium_set_misuse_handler(Fatal)` → `sodium_init()`이다. RNG 교체는 init 전에 한 번만 할 수 있다. handler는 libsodium의 정적 잠금만 쓰므로 init 전에 설치할 수 있고, 먼저 두어야 init 안의 misuse(페이지 크기 확인 등)도 기본 handler(`abort`)가 아닌 Fatal로 간다. `sodium_init`이 -1을 돌려주면 Fatal이다. 1(이미 초기화)도 Fatal이다. libsodium은 Crypto만 쓰므로, 다른 경로가 먼저 init했다면 RNG 교체 순서가 깨졌을 수 있기 때문이다. 비교·소거처럼 init과 무관한 함수도 진입점에서 같은 once를 거쳐 계약을 하나로 둔다(2026-11-14 리뷰 반영). 암호 기능 없이 계속 진행하지 않는다.
- **난수 단일 경로.** Rocks 구현은 구조체의 모든 함수(`implementation_name`·`random`·`stir`·`uniform`·`buf`·`close`)를 채운다. libsodium이 `random`·`implementation_name`을 NULL 검사 없이 부르기 때문이다. `buf`는 `platform::TryFillSecureRandom`(BCryptGenRandom / arc4random_buf)을 직접 부르고, 실패하면 Fatal이다(`randombytes_buf`는 실패를 돌려줄 수 없다). `sodium_init` 안에서 `stir`·`buf`가 불리므로, 이 구현은 Crypto 초기화에 의존하지 않는다(재귀 방지). 사용자 공간 DRBG는 두지 않는다. 이렇게 libsodium과 Rocks Guid·SecureRandom이 같은 OS 경로를 쓴다.
- **misuse handler 도달 금지.** handler는 Fatal이지만, 신뢰할 수 없는 길이로 원격에서 Fatal을 일으킬 수 있으면 안 된다. 예를 들어 secretstream pull은 `mlen > MESSAGEBYTES_MAX`일 때 misuse를 부른다. 그래서 어댑터가 misuse 조건을 모두 먼저 걸러 `AuthenticationFailed`·`CapacityExceeded`로 돌려준다. 음성 시험이 이 계약을 고정한다.
- **복호 실패 출력.** libsodium AES-GCM 복호는 실패하면 출력을 `0xd0`으로 채운다. 어댑터가 0으로 다시 지운다. 복호 출력은 프로세스 전용 메모리여야 한다. mmap 파일이나 공유 메모리면 검증 전 평문이 잠시 보일 수 있다.
- **심볼.** libsodium에는 심볼 접두사 설정이 없다. Rocks가 libsodium을 제공하므로, 엔진에 링크되는 다른 모듈·플러그인은 자체 libsodium을 정적 링크하지 않는다(같은 심볼 충돌).
- **할당.** libsodium은 대부분 할당하지 않는다. 예외는 Argon2 작업 메모리(libsodium 내부 할당)이고, manifest `notes`와 할당 정책에 예외로 기록한다. `sodium_malloc`은 쓰지 않는다(10.2절).
- **상태 정렬.** generichash 상태는 384 B에 64 B 정렬, aes256gcm 상태는 512 B에 16 B 정렬, onetimeauth 상태는 256 B에 16 B 정렬이다. 모두 SecretArena 슬롯에 두며, 슬롯이 정렬을 보장한다.
- **고지.** ISC는 모든 사본에 저작권·허가 문구를 요구하므로 `binaryNoticeRequired: true`로 둔다.

### 5.2 AES 경량 계층: 출처와 범위

libsodium의 범용 AES는 AES-256-GCM 일회성(사전 계산 포함) 하나뿐이다. ipcrypt(1.0.21)의 AES-128 단일 블록과 AES-XTS 기반 구성은 IP 주소 암호화 전용이다. libsodium에 없는 AES-128-GCM, GCM 멀티파트, CTR, CMAC는 Rocks의 경량 계층이 맡는다. 원칙은 세 가지다. HW 명령만 쓴다. 소프트웨어 AES는 두지 않는다. 외부 의존을 더하지 않는다.

**외부 AES 라이브러리를 들이지 않는 이유.** 조사한 후보는 모두 덩치가 크거나 빌드 조건(Clang만, asm 없음)에 맞지 않았다.

| 후보 | 걸림돌 |
|---|---|
| TF-PSA-Crypto 1.1 LTS / Mbed TLS 4.x 계열 | AES-NI·ARMv8 CE를 intrinsics로 쓰지만, 1.x는 PSA API만 공개한다. 그래서 전역 키 저장소·스레드 계층·driver wrapper가 함께 들어온다. 소프트웨어 AES·GHASH는 테이블이고 XTS도 없다. 저수준 API가 공개된 Mbed TLS 3.6 LTS는 2027-03에 지원이 끝난다. |
| AWS-LC 5.14, BoringSSL, OpenSSL 4.1 | AES 가속이 perlasm 어셈블리다. asm을 끄면 HW 가속도 꺼지고, Windows x64는 NASM이 필요하다. 라이브러리도 크다. BoringSSL은 README가 외부 의존을 권하지 않는다. |
| SymCrypt 103.13 | Python으로 asm을 변환해야 하고, Android와 clang-cl을 지원하지 않는다. |
| BearSSL 0.6 | ARMv8 CE 경로가 없고 beta 표기다. |

**출처: libsodium 구현에서 파생.** 이미 들이는 libsodium 원본에 `aead_aes256gcm_aesni.c`(1,020줄)와 `aead_aes256gcm_armcrypto.c`(1,072줄, 1.0.22-stable 기준)가 있다. 둘 다 intrinsics만 쓴다. 키 확장(`expand256`, x86은 `_mm_aeskeygenassist_si128`, ARM은 0 라운드 키를 준 AESE), 병렬 CTR(`encrypt_xor_wide`, x86 7블록·ARM 6블록), carry-less 곱셈 GHASH(`clmul128`·`gcm_reduce`, Karatsuba 대신 교과서식 4회 곱셈), H 거듭제곱 사전 계산(`precomp`)으로 이루어진다. 경량 계층은 이 구조와 루틴을 그대로 가져오고, 바꾸는 부분을 다음으로 한정한다.

- AES-128 키 확장과 라운드 수 매개변수(10·14). AES-128 키 확장은 libsodium ipcrypt 구현(`ipcrypt_aesni.c`·armcrypto)에 이미 있는 것을 가져온다.
- 증분 상태: 부분 블록 keystream과 GHASH 버퍼, AAD에서 평문으로 넘어가는 단계, 길이 블록
- CTR과 CMAC 노출. 원본 `encrypt_xor_wide`·`incr_counters`는 byte 역순 카운터에 64 bit lane 덧셈(`ADD64x2`)을 쓴다. 메시지 블록 한도(2^32 − 2) 안에서만 inc32와 같으므로, 경량 계층은 카운터를 이어 받는 멀티파트를 위해 진짜 32 bit lane 덧셈(inc32)으로 바꾸고, CTR의 64 bit lane·128 bit(아래 lane 올림을 비교 mask로 위 lane에 더한다) 증가와 폭 선택은 새로 쓴 코드다. 카운터 폭은 매개변수로 두고(기본 128 bit), wrap은 쓰기 전에 `CapacityExceeded`다. CMAC 서브키 doubling은 L = E_K(0)이 비밀이므로 분기 없는 mask로 한다.

AES-256-GCM 일회성은 libsodium 원본 함수를 그대로 쓴다. 같은 기기에서 원본과 경량 계층의 AES-256-GCM을 대조하는 차등 시험이 GCM 파생 코드의 기준(oracle)이 된다. 다만 원본과 같은 구조라 원본에 있는 결함은 양쪽이 같이 통과하고, 원본에 없는 부분(CTR carry, CMAC, AES-128)은 기준이 되지 못한다. 그래서 시험 전용 스칼라 참조(블록 암호화 + 카운터 증가, 출하 코드 아님)를 따로 둔다(12절). 파생 파일 머리에는 출처(libsodium 1.0.22, 원본 경로)와 ISC 고지를 둔다. 원본 파일의 SHA-256을 파생 기록 파일(`Crypto/Private/Aes/Derivation.json`: 파생 파일마다 upstream 경로와 SHA-256)에 적어 두어, libsodium을 올리면서 원본이 바뀌면 정책 검사 `derived-source`가 실패하고 diff 검토를 요구하게 한다(P8). 이 검사는 upstream 경로가 manifest의 upstream 디렉터리 안에 있고 SHA-256이 맞는지, 파생 파일이 있고 출처 주석에 각 upstream 파일 이름을 적었는지, 기록에 알 수 없는 필드가 없는지 본다.

**범위.** C2 단계에서 GCM(AES-128/256, 일회성·멀티파트)과 CTR을 먼저, CMAC를 이어서 만든다. CBC·KW·CCM은 역방향 라운드나 별도 형식이 필요하므로 요구가 생길 때 더한다. XTS·GCM-SIV와 96 bit가 아닌 GCM nonce는 넣지 않는다. 구현(2026-11-15): AES-128-GCM(공개 AEAD), GCM 멀티파트·CTR(`Crypto/AesModes.h`), AES-CMAC(`MacAlgorithm::AesCmac`)을 모두 넣었다(13절 C2).

### 5.3 AES 경량 계층: 구조와 빌드

- **파일.** `Crypto/Private/Aes/AesX64.cpp`(AES-NI·PCLMULQDQ 커널), `AesArm64.cpp`(AESE·AESMC·PMULL 커널), `AesModes.cpp`(아키텍처와 무관한 커널 선택·자가 시험, 키 스케줄 수명, GCM 일회성·멀티파트, CTR 용량, CMAC), 내부 헤더 `AesKernels.h`(키 스케줄·커널 표)·`AesModesInternal.h`(모드 함수·상태 구조체)로 나눈다. 공개 hazmat 어댑터는 `Crypto/Private/AesHazmat.cpp`다. 구현 규모(2026-11-15)는 커널 x64 약 550줄·arm64 약 610줄, `AesModes.cpp` 약 680줄, 어댑터 `AesHazmat.cpp` 약 520줄이다. `Derivation.json`은 세 파생 파일(AesX64·AesArm64는 aesni·armcrypto와 ipcrypt 원본, AesModes는 aesni·armcrypto 원본)을 기록한다.
- **커널 표(구현).** 아키텍처마다 `expandKey`(16·32 byte 키 → 라운드 키·H 거듭제곱), `ctrXor`(버퍼, 병렬 블록, 카운터 폭 `AesCounterWidth` Bits32·Bits64·Bits128을 버퍼마다 한 번 골라 템플릿 경로로 들어간다. 마지막 부분 블록은 keystream 앞부분만 쓰고 카운터는 그 블록만큼 올라간다. 폭 안의 wrap은 막지 않고 모드 계층이 미리 센다), `ghashUpdate`(버퍼, GCM byte 순서 누적값, 마지막 호출만 16의 배수가 아니어도 된다), `cbcMacUpdate`(블록 단위, CMAC), `encryptBlock`(블록 하나: GCM J0와 CMAC 서브키 L에만 쓴다) 다섯 함수와 구현 이름(`rocks-aes/x64`·`rocks-aes/arm64`)을 두고, `CpuFeatures`로 첫 사용 때 한 번 고른다(`GetAesKernels`). 상태 기계(`AesModes.cpp`)는 블록마다 커널을 간접 호출하지 않는다(속성 함수는 일반 호출자로 inline되지 않는다). GCM은 4032 byte(252블록, x64 7·arm64 6블록 병렬 폭의 공배수) 조각마다 CTR과 GHASH를 번갈아 부른다. 고른 뒤 자가 시험을 한다: AES-128·AES-256 각각 GCM 봉인 tag(AD 33 byte, 평문 245 byte), 같은 버퍼 제자리 열기 왕복, CBC-MAC 15블록의 알려진 답이다(Python cryptography·pycryptodome로 확인한 값). 불일치하면 표를 비우고 `StderrLogSink`에 한 줄을 남긴다. 값을 받는 `SelectAesKernels`·`CheckAesKernels`로 시험이 HW 없는 경로와 자가 시험 거부를 재현한다(11절 결정성). 공개 AES 계열(AES-128/256-GCM, AEGIS, AesModes.h, AES-CMAC)의 사용 가능 여부는 하나다(`detail::IsAesAvailable`): libsodium HW 판정(6.2절)과 이 표(HW 조건 + 자가 시험)가 모두 참이어야 하고, 아니면 모든 AES 호출이 `Unsupported`다(6.1절 "부분 AES 없음").
- **컴파일.** 함수 단위 target을 쓴다. x86-64는 libsodium 원본과 같은 `"aes,avx,pclmul"`이다(`avx`가 SSSE3·SSE4.1을 함께 켜고 SSE 명령을 VEX로 바꾸므로 감지 조건은 `aes && pclmulqdq && avx`, avx는 OS의 YMM 상태 확인 포함). ARM64는 원본의 `"neon,crypto,aes,fuse-aes"`에서 `crypto`(SHA2까지 켠다)를 뺀 `"neon,aes,fuse-aes"`로, 감지 조건 `armAes && armPmull`과 같은 범위만 켠다. 원본은 `#pragma clang attribute push`로 파일 전체에 걸지만, Rocks 소스에서는 Base의 `ROCKS_TARGET_X64_AES`·`ROCKS_TARGET_ARM64_AES` 매크로(`Base/Compiler.h`, P7)로 함수마다 건다. 매크로는 해당 아키텍처에서만 정의하므로 커널 파일은 `#if ROCKS_ARCH_*`로 감싼다(다른 아키텍처에서 쓰면 컴파일 오류다). 속성 함수가 속성이 없는 호출자로 inline된다고 기대하지 않는다(대상 기본 CPU가 기능을 이미 포함할 때만 inline될 수 있다. 예: macOS arm64). 그래서 커널 표 함수는 블록이 아니라 버퍼 단위로 둔다. P7 probe(2026-11-15)에서 Windows clang-cl 21(`/W4 /WX`), NDK clang arm64·x86_64, macOS arm64 대상(Swift 툴체인의 upstream clang 21) 모두 이 매크로로 원본이 쓰는 intrinsics(AES·PCLMUL·PMULL·PMULL2·TBL·SSSE3 셔플)가 경고 없이 컴파일되고, 속성이 없으면 Windows·Android는 "requires target feature 'aes'"로, NDK arm64는 AES 함수 안의 SHA2 intrinsic을 "requires target feature 'sha2'"로 거부함을 확인했다(macOS arm64는 기본 CPU apple-m1이 AES·SHA2를 포함해 속성 없이도 컴파일된다). Apple clang은 같은 형식의 target 문자열을 쓰는 libsodium 원본이 macOS CI에서 컴파일되는 것으로 간접 확인한다. C2 전까지는 매크로를 쓰는 소스가 없어 CI가 컴파일하지 않으므로 이 probe가 근거다.
- **정책 검사(P7).** 지원하지 않는 CPU에서 부르면 잘못된 명령 예외이므로, 확장 명령은 감지 뒤 고른 커널 표로만 부르는 디렉터리에 가둔다(`PolicyChecks.TargetIntrinsics`, 시험 소스 포함, 주석·리터럴 제외).
  - `target-attribute`: target 속성(`__attribute__((target(...)))`·`[[gnu::target]]`·`target_clones`·`target_version`·x86 `cpu_specific`·`cpu_dispatch`)과 `#pragma clang attribute`(내용과 관계없이)·`#pragma GCC target`, 그 `_Pragma`·`__pragma` 형태를 `Base/Compiler.h` 밖에서 직접 쓰지 않는다. 여러 줄·줄 이음에 걸친 형태도 잡는다. 허용 디렉터리 안에서도 매크로를 쓴다.
  - `target-intrinsics`: 계열별 허용 목록이다. AES 라운드(AES-NI·VAES·Key Locker, AESE·AESD·AESMC·AESIMC)·carry-less 곱셈(PCLMULQDQ·VPCLMULQDQ, PMULL 64 bit)과 `ROCKS_TARGET_*_AES`는 `Crypto/Private/Aes/`, SHA 계열(SHA-NI, ARMv8 SHA1·SHA2·SHA512·SHA3·SM3·SM4)은 Rockpile R0의 Core SHA-256 HW 경로(`Core/Private/Encoding/`)만 쓴다. 다른 계열의 디렉터리에서도 거부한다. 범용 SIMD(SSE·AVX·NEON 산술, PMULL 8 bit)는 대상이 아니다. builtin 형태(`__builtin_ia32_aesenc128`·`__builtin_neon_vaeseq_u8` 등)도 같은 계열로 본다(`compiler-builtin`은 PlatformMath 헤더에 `__builtin_*` 전체를 허용하므로 그것만으로는 막히지 않는다). 어느 계열에도 속하지 않은 `ROCKS_TARGET_*` 매크로는 정의·사용 모두 거부하므로, SHA 경로는 자기 target 매크로를 `Compiler.h`에 더할 때 이 계열에 넣어야 한다. 모듈 규칙은 `ROCKS_TARGET_*`를 정의할 수 없다(빌드 입력 검증, `ROCKS_ARCH_*`와 같다).
  - `arm_neon.h` 자체는 이미 PlatformBase 공개 헤더(`NeonPlatformMath.h`·`VectorRegisterNeon.h`)가 include하므로 막지 않는다. 외부 원본은 이 검사에서 빠진다(libsodium의 target pragma는 manifest `pragmaDirectives`가 기록하고, zstd의 target 속성 매크로는 원본 SHA-256으로 고정된다).
  - lexical 검사의 한계: 매크로로 속성·이름을 조립하는 형태, inline asm 명령, 검사 대상이 아닌 확장자(.inl·.hpp)는 잡지 않는다. 의도적 우회이므로 리뷰가 맡는다.
- **상수 시간.** AES 라운드와 carry-less 곱셈은 명령이 처리하므로 비밀에 따라 달라지는 테이블 조회가 없다. 키 확장도 x86은 `AESKEYGENASSIST`, ARM은 0 라운드 키를 준 `AESE`로 S-box를 명령에 맡긴다(원본 방식). 태그 비교는 `sodium_memcmp`다.
- **키 객체.** AES 키를 만들 때 라운드 키와 GCM H 거듭제곱을 한 번 계산해 키 객체의 SecretArena 슬롯(512 byte 등급)에 둔다. AES-256-GCM `AeadKey`는 libsodium `beforenm` 상태도 이때 함께 만들어 두 슬롯을 갖는다. 지연 계산하면 "생성 뒤 불변" 계약이 깨지기 때문이다. 이후 바뀌지 않으므로 여러 스레드가 공유한다. 구현: `AeadKey`(AES-128/256-GCM), `MacKey`(AES-CMAC), hazmat `AesKey`(CTR)가 스케줄을 품는다. 상태 객체(GCM 멀티파트, CTR 스트림, AES-CMAC `MacState`)는 키 스케줄을 자기 슬롯(1 KiB 등급)에 복사하므로 키 객체와 수명이 독립이다(키를 먼저 버려도 되고, 해제된 슬롯을 다른 키가 다시 받아 엉뚱한 키로 계산하는 일이 없다).
- **Rocks 코드 규칙.** 파생 코드는 ThirdParty 원본(byte 보존)이 아니라 Rocks 소스다. 서식·경고 0·주석 규칙을 그대로 적용하고, 함수마다 대응하는 원본 함수 이름을 주석에 남긴다. 원본의 `#define __ARM_FEATURE_CRYPTO`·`__ARM_FEATURE_AES`(예약 식별자, PlatformBase가 `arm_neon.h`를 먼저 include하므로 효과도 없다)와 `#pragma clang attribute`는 가져오지 않고 매크로로 바꾼다.

### 5.4 시험 데이터: Wycheproof

Wycheproof는 Apache-2.0 data 라이브러리(`"kind": "data"`)로, commit을 고정해 들인다. 시험 전용이며 바이너리에 들어가지 않는다. Core `Json` 파서로 읽는다(`Crypto/Tests/WycheproofTests.cpp`).

- C1(2026-11-15): commit `65ca494727a5`(릴리스가 없어 GitHub commit archive와 SHA-256으로 고정)의 `testvectors_v1/` 가운데 `aes_gcm`, `chacha20_poly1305`, `xchacha20_poly1305`, `hmac_sha256`·`hmac_sha512`, `hkdf_sha256`·`hkdf_sha512`, `ed25519`, `x25519`, `mlkem_768`과 LICENSE만 들인다. 시험은 파일마다 모든 사례를 세고 갈래별 개수를 고정해, 조용히 건너뛰는 사례가 생기면 실패한다.
- AEGIS는 들이지 않는다. Wycheproof AEGIS 벡터는 128 bit tag이고 libsodium은 256 bit tag만 내므로 tag와 invalid(변조된 tag) 사례를 검증할 수 없다. 암호문 부분은 tag 길이와 무관해 valid 사례의 암호문만 비교할 수는 있지만, draft 부록 TV1–TV3(tag256)과 libsodium 표(AeadTests)가 같은 구현을 tag까지 덮으므로 들이지 않는다.
- AES-128-GCM 108건은 이미 들인 `aes_gcm`에 있고 C2에서 사상이 Unsupported에서 검증으로 바뀌었다(96 bit nonce 67건 검증, 다른 nonce 41건 `Unsupported`). 파일을 더하는 것은 CMAC(`aes_cmac`, C2)과 secretstream(C3)이다(그때 manifest `files`를 늘린다).
- C2(2026-11-15): 같은 commit archive(SHA-256 일치 확인)에서 `aes_cmac_test.json`(311건)을 들였다. 사상: 128·256 bit 키 그룹 각 102건(valid 21, 변조 tag 81)은 일회성·증분 tag와 검증 결과를 확인하고, AES-192 키 102건과 키 길이 오류 5건(0·8·64·160·320 bit)은 `MacKey::TryImport`가 `InvalidArgument`다. AES HW가 없으면 311건 모두 `Unsupported`로 센다.
- C3(2026-11-15): 같은 archive에서 `xchacha20_poly1305_secretstream_test.json`(7건, sodium_compat 경계 회귀의 push 상태 벡터)을 들였다. 벡터가 "이 상태에서 push하면 이 암호문·이 상태"라 공개 API로는 닿지 않으므로 시험 접근점(`Private/SecretStreamTestAccess.h`)으로 상태(하위 키·카운터·inonce)를 넣는다. 사상: Writer push의 암호문과 뒤 상태, 같은 상태 Reader의 pull 평문·tag·뒤 상태가 모두 같아야 한다(초기 상태 1건, 카운터 2^16 경계에서 조기 rekey 없음 3건, 2^32 wrap 자동 rekey 3건).
- 실행 범위: 시험은 저장소 루트 기준 상대 경로로 파일을 읽는다. Windows·macOS 시험과 Android 기기 시험에서 돈다. Android 기기 runner(`Programs/Rocks.Build.Cli/AndroidDevice.cs`)는 data 라이브러리(`"kind": "data"`)의 manifest와 `files`를 기기 작업 디렉터리 아래 같은 상대 경로로 함께 올린다(2026-11-15. x86_64 에뮬레이터에서 Wycheproof·Core JSON·Unicode 시험 포함 1317개 통과).

## 6. AES 하드웨어 가속

> **AES 가속은 AES 명령 intrinsics 경로다.** x86-64는 AES-NI(`_mm_aesenc_si128`·`_mm_aesenclast_si128`·`_mm_aeskeygenassist_si128`)와 GHASH용 PCLMULQDQ(`_mm_clmulepi64_si128`)를 쓰고, ARMv8은 Crypto Extensions(`vaeseq_u8`·`vaesmcq_u8`)와 PMULL(`vmull_p64`)을 쓴다. 이 intrinsics 코드는 두 곳에 있다. 하나는 libsodium(AES-256-GCM·AEGIS)이고, 다른 하나는 그 구현에서 파생한 Rocks 경량 계층(AES-128-GCM·GCM 멀티파트·CTR·CMAC)이다. 둘 다 함수 단위 target 속성으로 컴파일하므로 전역 `-maes`가 필요 없다. 소프트웨어 AES는 두지 않는다.

### 6.1 경로 표

| 알고리즘 | x86-64 / AES-NI+PCLMULQDQ+AVX | ARMv8 / AES+PMULL | HW 없음 |
|---|---|---|---|
| AES-256-GCM 일회성, AES-256 세그먼트 | libsodium aesni | libsodium armcrypto | [Unsupported] / 소프트웨어 경로 없음 |
| AES-128-GCM, GCM 멀티파트, CTR, CMAC | 경량 계층 x64 | 경량 계층 arm64 |  |
| AEGIS-256/128L | libsodium aesni | libsodium armcrypto |  |
| (X)ChaCha20-Poly1305, secretstream, BLAKE2b, Argon2id | libsodium 내부 SIMD 디스패치 또는 portable C [상수 시간] |  |  |

**HW 조건은 하나다.** x86-64는 AES-NI·PCLMULQDQ·AVX, ARMv8은 AES·PMULL이다. x86은 libsodium이 AES-256-GCM에 요구하는 조건과 같다. ARM은 libsodium보다 엄격하다. libsodium은 Linux·Android에서 HWCAP의 AES bit만 보고, macOS는 컴파일 시점 매크로로, Windows ARM64는 항상 참으로 판정하며 PMULL을 따로 보지 않는다. Rocks는 libsodium 경로도 이 조건으로 막아, 같은 기기에서 AES 알고리즘 일부만 되는 상태를 없앤다. 그 대가로 AES-NI는 있지만 AVX가 없는 x86-64(Goldmont 계열 Celeron·Pentium Silver 등)에서는 AES 전체가 `Unsupported`다. AES-256-GCM은 일회성(libsodium)과 멀티파트(경량 계층)가 다른 코드를 타므로, 두 경로의 출력이 byte 단위로 같은지를 12절의 차등 시험이 고정한다.

### 6.2 CPU 기능 감지와 일치 시험

PlatformBase `CpuFeatures`에 다음 필드를 더한다. 기존 `avx` 필드(XGETBV 확인 포함)는 그대로 쓴다. SHA 필드는 Rockpile R0의 SHA-256 하드웨어 경로가 쓴다. 쓰는 곳이 없는 VAES·VPCLMULQDQ는 넣지 않는다.

| 필드 | x86-64 | ARM64 |
|---|---|---|
| `aes`·`pclmulqdq` | CPUID.1:ECX[25]·[1] | — |
| `sha` | CPUID.7.0:EBX[29] | — |
| `armAes`·`armPmull`·`armSha2` | — | macOS `sysctlbyname("hw.optional.arm.FEAT_AES")`·`FEAT_PMULL`·`FEAT_SHA256`, Android `getauxval(AT_HWCAP)`의 `HWCAP_AES`·`HWCAP_PMULL`·`HWCAP_SHA2` |
| `armDit` | — | macOS `sysctlbyname("hw.optional.arm.FEAT_DIT")`(이름이 없는 구버전 OS는 false). 다른 OS는 아직 감지하지 않는다(6.3절 DIT). 모든 Apple Silicon에 있으므로 macOS arm64 시험이 참을 요구한다 |
| `armSb` | — | macOS `sysctlbyname("hw.optional.arm.FEAT_SB")`. DIT를 켠 뒤 barrier 선택에 쓴다(6.3절 DIT) |

- **일치 시험.** `sodium_runtime_has_aesni`·`_pclmul`·`_avx`가 x86 필드와, `sodium_runtime_has_armcrypto`가 `armAes`와 같아야 한다(libsodium은 HWCAP의 AES bit만 보고 PMULL은 보지 않는다). 다르면 빌드 정의 누락이므로 시험이 실패한다. libsodium이 하드웨어 경로를 오류 없이 끄는 문제를 이 시험이 잡는다.
- **실행 시점 판정.** 시험이 없는 기기(Android arm64 실기기)를 위해, libsodium 경로의 `available`은 `CpuFeatures`와 `sodium_runtime_has_*`가 모두 참일 때만 참이다. 빌드 정의가 빠져 libsodium이 AEGIS를 soft로 돌리는 기기에서 Rocks가 `available = true`를 보고하지 않게 하기 위해서다. 경량 계층 표는 `CpuFeatures`와 자가 시험(5.3절)으로 고르고, 공개 AES 계열의 `available`은 두 판정이 모두 참일 때만 참이다(5.3절 `IsAesAvailable`).
- **구현 보고.** `TryGetAeadInfo`는 선택된 구현의 이름을 돌려준다(예: `"libsodium/aesni"`, `"rocks-aes/arm64"`). 시작 진단 로그에도 이 이름을 한 번 남긴다.

### 6.3 HW AES가 없는 기기

ARMv8.0의 Crypto Extensions는 선택 기능이어서 저가 SoC에는 없을 수 있다. 그런 기기에서 AES·AEGIS는 `Unsupported`다. 소프트웨어 AES를 두지 않는 이유는 두 가지다. 테이블 구현은 cache-timing에 노출되고(libsodium이 AES-GCM 소프트웨어 구현을 두지 않는 이유와 같다), 상수 시간 비트슬라이스 구현은 외부 의존이나 자체 코드를 크게 늘린다.

- `TryGetAeadInfo`는 `available = false`를 돌려주고, `GetPreferredAead()`는 XChaCha20-Poly1305를 돌려준다. AES 명령이 있을 때 무엇을 돌려줄지는 결정 D2다.
- AES로 봉인한 데이터는 그런 기기에서 열 수 없다. 그래서 형식을 고르는 쪽(쿠커·저장 도구)은 대상 기기군에 HW가 없을 수 있으면 ChaCha20-Poly1305 계열(세그먼트 형식 포함)을 고른다.
- **ARM DIT.** 공개 연구(GoFetch, 2024)는 Apple M1·M2의 데이터 메모리 의존 prefetcher가 상수 시간 코드에서도 비밀을 흘릴 수 있음을 보였다. M3는 DIT(Data Independent Timing)를 켜면 이 prefetcher가 꺼진다(공개 확인은 M3뿐이고 M4 이후는 확인되지 않았다). macOS arm64가 대상이므로, FEAT_DIT가 있으면 암호 연산 구간에서 DIT를 켜는 것을 C1에 넣는다(결정 D12). M1·M2는 끌 방법이 없어 한계로 적는다. libsodium도 DIT 검토를 끝내지 않았다(로드맵). 구현(2026-11-15): PlatformBase `Platform/DataIndependentTiming.h`의 `DataIndependentTimingScope`가 FEAT_DIT가 있고 꺼져 있을 때만 PSTATE.DIT를 켜고, 범위가 끝나면 그 범위가 켠 경우에만 끈다(스레드 상태, 겹친 범위는 바깥 범위가 끝날 때까지 유지). 읽기는 범용 시스템 레지스터 이름(`S3_3_C4_C2_5`), 쓰기는 MSR(immediate) 인코딩이라 `+dit` 없이 모든 arm64 대상(macOS·Android·Windows)에서 어셈블된다. 켠 직후에는 Apple 지침("Writing ARM64 code for Apple platforms"의 DIT 절)대로 speculation barrier를 둔다. FEAT_SB가 있으면 `SB`, 없으면 `DSB NSH`·`ISB`다. Arm ARM은 PSTATE 쓰기에 barrier를 요구하지 않지만, 없으면 뒤따르는 비밀 의존 명령이 DIT가 꺼진 상태로 투기 실행될 수 있다(Go도 darwin/arm64에서 같은 barrier를 둔다). 끌 때는 barrier가 없다. Crypto는 비밀을 다루는 공개 진입점(Aead·Mac·Kdf·PasswordHash·Hash·Signature·KeyExchange·Kem·Bytes·KeyBlob의 익명 namespace 밖 함수와 `SecretBytes::TryGenerate`·`TryCopyFrom`, `TryFillRandom`·`TryRandomUniform`)에서 범위를 연다. 비밀 값을 읽어 계산하지 않는 함수에는 열지 않는다. 지우기·해제(소멸자, `Reset`·`Release`)는 값을 0으로 덮을 뿐이라 열지 않는다(공개 진입점인 `SecureZero`는 연다). 정보 조회·크기 계산(`TryGetAeadInfo`·`TryGetSealedBytes`·`GetPreferred*` 등)과 공개 데이터만 다루는 함수(공개키 `TryFromBytes`, `Ed25519phVerifier` 생성·Reset)도 열지 않는다. 서명 검증(`TryVerify`, `Ed25519phVerifier`의 `TryUpdate`·`TryFinish`)은 연다. 공개키·서명은 공개지만 메시지는 복호한 기밀 평문일 수 있고(서명 뒤 암호화), 서명 쪽·해시와 같은 기준이다. 범위마다 MSR 두 번과 barrier가 들므로 작은 조각을 반복하는 증분 API에서 비용이 커질 수 있다(6.4절 측정에서 따로 기록한다). 감지는 지금 macOS만이라 다른 대상에서는 아무 일도 하지 않는다. 실기기 확인은 macOS arm64 CI 몫이다(Windows 호스트에서는 cross 컴파일로 어셈블만 확인했다).

### 6.4 성능 gate

알고리즘별로 64 B·1 KiB·64 KiB·1 MiB 입력의 처리량(cycles/byte)을 측정해 기록한다. gate는 경량 계층 AES-256-GCM 처리량과 같은 기기 libsodium AES-256-GCM 처리량의 비율이다. 같은 구조에서 파생했으므로, 크게 떨어지면 파생 과정의 회귀로 보고 고친다. 결정 D4도 이 측정으로 확정한다. 측정 전에는 수치를 약속하지 않는다. macOS arm64에서는 DIT 범위(MSR 두 번과 barrier) 비용을 64 B 측정과 따로 기록한다. 증분 API(`TryUpdate`)의 작은 조각에서 비용이 크면, 지원 여부를 캐시하는 내부 경로를 검토한다.

측정 도구는 `Programs/CryptoBench`(Release, 공개 API만, 단일 스레드, AD 13 byte, 크기마다 64·128 MiB, 5·9회 중 최고값)다. 경량 계층 AES-256-GCM의 공개 경로는 멀티파트뿐이라 메시지마다 `AesGcmEncryptor` 생성(슬롯 할당 + 스케줄 복사)·`TryUpdate` 한 번·`TryFinish`로 잰다. 2026-11-15 Windows x64(i5-13600KF, clang-cl Release) 결과(MB/s, 두 번 잰 범위)는 다음과 같다. cycles/byte는 아직 재지 않았다(시간 기준 처리량만).

| 크기 | libsodium AES-256-GCM | 경량 AES-256(메시지마다 생성) | 경량 AES-256(조각 이어 넣기) | 경량 AES-128 일회성 | 비율(경량/libsodium) |
|---|---|---|---|---|---|
| 64 B | 1,486–1,523 | 542–544 | 3,580–3,762 | 1,144–1,260 | 0.36 |
| 1 KiB | 5,948–6,016 | 3,911–4,312 | 6,233–6,544 | 5,827–6,112 | 0.65–0.73 |
| 64 KiB | 7,281–7,380 | 6,555–6,630 | 6,635–6,698 | 7,688–8,113 | 0.90 |
| 1 MiB | 6,711–7,282 | 6,540–6,569 | 6,028–6,577 | 7,819–7,833 | 0.90–0.97 |

- 64 KiB 이상에서 경량 계층은 libsodium의 약 0.9배다. CTR·GHASH를 4032 byte 조각으로 번갈아 부르는 구조가 원본의 블록 단위 엮기보다 조금 느린 것으로 본다. 0.8배 아래가 아니어서 융합 GCM 커널(CTR·GHASH를 한 루프에서 엮는 원본 방식)은 넣지 않았다. 필요해지면 커널 표에 융합 함수를 더한다.
- 작은 메시지의 "메시지마다 생성"은 키 저장소 슬롯 할당과 512 byte 스케줄 복사가 지배한다(멀티파트의 고정 비용이지 GCM 계산 비용이 아니다). 조각 이어 넣기(같은 암호기에 64 B씩)는 3.6 GB/s 이상이라 증분 API의 조각당 비용은 작다(Windows는 DIT가 없어 DIT 비용은 0이다. macOS arm64 측정은 CI 몫으로 남긴다).
- D4 확정: AES-256-GCM 일회성·세그먼트 경로는 libsodium 원본을 유지한다. 경량 계층이 같은 기기에서 동등하지 않고(0.9배), 원본이 경량 계층 GCM의 차등 기준으로 남는다.

## 7. API 원칙: Span

- **byte 입력은 모두 `Span<const uint8>`이다.** 포인터와 길이를 따로 받지 않는다. C 배열은 Span으로 암시 변환되고, `Array`는 `View()`로 넘긴다.
- **출력은 호출자가 준 `Span<uint8>`이다.** 반환값은 쓴 byte 수(`Result<usize>`)다. 출력 길이를 호출자가 정하는 함수(해시·XOF·MAC tag·KDF처럼 `output.Size()`만큼 정확히 채우는 함수)는 `Result<void>`다. 필요한 크기는 `TryGetSealedBytes`·`TagBytes` 같은 함수와 constexpr 상수로 미리 구한다. `Array<uint8>&`를 받는 Append 변형은 편의 함수일 뿐이며 내부에서 Span 경로를 부른다.
- **조각 입력은 `Span<const Span<const uint8>>`이다.** 해시·MAC·서명처럼 라이브러리가 증분 입력을 받는 곳에만 둔다. 헤더와 본문을 복사하지 않고 넣을 수 있다. AEAD의 AD는 libsodium이 연속 버퍼만 받으므로 조각 입력을 두지 않는다(복사를 숨기지 않는다).
- **고정 길이 값은 값 타입이다.** digest·서명·공개키·nonce는 `Ed25519Signature`처럼 할당 없는 값 타입으로 돌려준다. 값 타입은 `View()`로 `Span<const uint8>`을 내준다. 받을 때는 Span에 길이 검사를 하며, 길이가 틀리면 `InvalidArgument`다(결정 D8).
- **비밀은 view로만 노출한다.** `SecretBytes::View()`는 `Span<const uint8>`이고 수명은 객체와 같다. 쓰기 view는 생성·가져오기 경로에만 있다.
- **제자리 처리는 함수마다 정한다.** nonce 지정·분리 형식은 입력과 출력이 같은 주소에서 시작할 때(`m == c`)만 허용한다. nonce를 앞에 붙이는 고수준 `TrySeal`은 `plaintext.Data() == output.Data() + NonceBytes`, `TryOpen`은 `plaintext.Data() == sealed.Data() + NonceBytes`일 때만 허용한다. sealed box는 임시 공개키 자리 뒤(`+ 32`)에서만 허용한다. 그 밖의 겹침은 쓰기 전에 `InvalidArgument`로 거부한다. 검사에는 Core `Overlaps`를 쓴다.

Core `Span`에는 위 원칙에 필요한 하위 범위·byte 변환·겹침 검사를 더했다(선행 P1, 2026-11-14 `ca4e2cb`와 리뷰 반영). Left·Right 계열은 기존 `StringView`처럼 범위를 넘으면 길이로 잘라 낸다. 범위 밖을 오류로 다뤄야 하는 파싱에는 `TrySlice`를 쓴다. `TrySlice`의 오류는 `InvalidArgument`이므로, 메시지에서 온 byte를 자르는 복호 경로 어댑터는 이 실패를 `AuthenticationFailed`로 바꿔 돌려준다(2·11절). `AsBytes`는 값마다 byte 표현이 하나뿐인 원소(패딩 없는 정수·구조체)만 받는다. 패딩 있는 구조체나 float를 해시에 넣으면 미정 byte·여러 표현 때문에 결과가 실행마다 달라질 수 있기 때문이다.

```cpp
template <typename T>
class Span {
public:
    constexpr bool IsEmpty() const noexcept;

    // 범위 밖이면 Fatal이다(operator[]와 같은 규칙). 길이를 이미 검사한 내부 경로에서 쓴다.
    constexpr Span Slice(usize index, usize count) const noexcept;

    // 범위를 넘으면 Size()로 잘라 낸다(StringView::TryLeftBytes 등과 같은 의미).
    constexpr Span Left(usize count) const noexcept;      // 앞 count개
    constexpr Span LeftChop(usize count) const noexcept;  // 뒤 count개를 뺀 나머지
    constexpr Span Right(usize count) const noexcept;     // 뒤 count개
    constexpr Span RightChop(usize count) const noexcept; // 앞 count개를 뺀 나머지

    // 범위 밖이면 InvalidArgument다(StringView::TrySliceBytes와 같은 규칙). 외부 입력을 파싱할 때 쓴다.
    Result<Span> TrySlice(usize index, usize count) const noexcept;
};

// T는 trivially copyable이어야 한다. byte view의 수명은 원본과 같다.
template <typename T>
Span<const uint8> AsBytes(Span<T> values) noexcept;

template <typename T>
Span<uint8> AsWritableBytes(Span<T> values) noexcept;   // T가 const면 컴파일 오류

// 두 view가 한 byte라도 겹치면 true다. 함수별 제자리 규칙(위)과 함께 쓴다.
template <typename T, typename U>
bool Overlaps(Span<T> a, Span<U> b) noexcept;   // 원소 타입이 달라도 byte 범위로 판정한다
```

## 8. 일회성 API

**Crypto/Bytes.h · Random.h**

```cpp
namespace rocks::crypto {

// 길이가 다르면 false다(길이는 비밀이 아니다). 같은 길이면 내용과 무관하게 같은 시간이 걸린다(sodium_memcmp).
[[nodiscard]] bool ConstantTimeEquals(Span<const uint8> left, Span<const uint8> right) noexcept;
[[nodiscard]] bool IsAllZero(Span<const uint8> bytes) noexcept;   // 상수 시간(sodium_is_zero)

// 컴파일러가 없애지 않는 소거다(sodium_memzero).
void SecureZero(Span<uint8> bytes) noexcept;

// OS CSPRNG로 채운다(PlatformBase SecureRandom, libsodium randombytes와 같은 경로).
// OS 난수 실패는 DependencyFailed이고 출력 전체를 지운다.
[[nodiscard]] Result<void> TryFillRandom(Span<uint8> output) noexcept;

// [0, upperBound) 균등 분포다. upperBound가 0이면 InvalidArgument.
[[nodiscard]] Result<uint32> TryRandomUniform(uint32 upperBound) noexcept;

// 비밀을 담은 byte의 상수 시간 hex·base64 변환(sodium_bin2hex·bin2base64·hex2bin·base642bin).
// Core Base64는 상수 시간이 아니므로 키·토큰에는 이 함수를 쓴다. text 출력은 문자 + NUL만 쓴다.
enum class Base64Variant : uint8 { Standard, StandardNoPadding, UrlSafe, UrlSafeNoPadding };
[[nodiscard]] Result<usize> TryGetHexChars(usize bytes) noexcept;                          // NUL 제외, Overflow
[[nodiscard]] Result<usize> TryGetBase64Chars(usize bytes, Base64Variant variant) noexcept;
[[nodiscard]] Result<usize> TryToHex(Span<const uint8> bytes, Span<char> text) noexcept;
[[nodiscard]] Result<usize> TryFromHex(StringView text, Span<uint8> bytes) noexcept;
[[nodiscard]] Result<usize> TryToBase64(Span<const uint8> bytes, Base64Variant variant, Span<char> text) noexcept;
[[nodiscard]] Result<usize> TryFromBase64(StringView text, Base64Variant variant, Span<uint8> bytes) noexcept;

} // namespace rocks::crypto
```

**Crypto/Hash.h**

```cpp
namespace rocks::crypto {

// BLAKE2b. digest.Size()가 출력 길이다(16–64 byte). 키를 주는 BLAKE2b keyed는 MAC이므로 Mac.h에 있다.
[[nodiscard]] Result<void> TryBlake2b(Span<const uint8> input, Span<uint8> digest) noexcept;

// 조각 입력. pieces를 이어 붙인 것과 같은 결과다(모든 해시·MAC에 같은 형태로 있다).
[[nodiscard]] Result<void> TryBlake2b(Span<const Span<const uint8>> pieces, Span<uint8> digest) noexcept;

// 알고리즘마다 Tag가 다른 고정 길이 값 타입이다(D8). Sha3_256Digest·Sha3_512Digest도 같은 형태다.
template <typename Tag, usize N>
struct FixedDigest {
    static constexpr usize Bytes = N;
    uint8 bytes[N] = {};
    Span<const uint8> View() const noexcept;
};

Sha512Digest Sha512(Span<const uint8> input) noexcept;
Sha3_256Digest Sha3_256(Span<const uint8> input) noexcept;
Sha3_512Digest Sha3_512(Span<const uint8> input) noexcept;

// XOF: output.Size()만큼 짜낸다. TurboSHAKE domain은 0x01–0x7F(RFC 9861, 기본 0x1F)다. 출력이 입력과 겹치면
// InvalidArgument다.
Result<void> TryShake128(Span<const uint8> input, Span<uint8> output) noexcept;           // TryShake256도 같다
Result<void> TryTurboShake128(Span<const uint8> input, Span<uint8> output, uint8 domain = 0x1F) noexcept;

} // namespace rocks::crypto
```

**Crypto/Aead.h**

```cpp
namespace rocks::crypto {

// 영속 형식에 기록할 수 있는 값이다. 값을 바꾸지 않고 끝에 덧붙인다.
enum class AeadAlgorithm : uint8 {
    XChaCha20Poly1305 = 1,     // nonce 24, tag 16
    ChaCha20Poly1305Ietf = 2,  // nonce 12, tag 16 (RFC 8439)
    Aes256Gcm = 3,             // nonce 12, tag 16
    Aes128Gcm = 4,             // nonce 12, tag 16 (AES 경량 계층)
    Aegis256 = 5,              // nonce 32, tag 32
    Aegis128L = 6,             // nonce 16, tag 32
};

struct AeadInfo {
    usize keyBytes;
    usize nonceBytes;
    usize tagBytes;
    uint64 maxMessageBytes;     // (key, nonce) 한 쌍의 평문 상한(11절 표)
    uint64 maxBlocksPerKey;     // 키당 16 byte 블록 상한(AES-GCM 2^36 − 1, 그 밖은 0 = 없음, 11절)
    bool randomNonceSafe;       // 무작위 nonce를 쓸 수 있는가(XChaCha20, AEGIS-256)
    bool available;             // 이 기기에서 쓸 수 있는가. AES·AEGIS는 HW 명령이 있을 때만 true
    StringView implementation;  // 진단용. 예: "libsodium/aesni", "rocks-aes/arm64"
};

[[nodiscard]] Result<AeadInfo> TryGetAeadInfo(AeadAlgorithm algorithm) noexcept;
AeadAlgorithm GetPreferredAead() noexcept;   // 6.3절, 결정 D2
[[nodiscard]] Result<usize> TryGetSealedBytes(AeadAlgorithm algorithm, usize plaintextBytes) noexcept; // Overflow

// 알고리즘에 묶인 키다. move-only, 키 저장소(10.2절), 소멸 시 소거.
// 생성 뒤에는 바뀌지 않으므로 여러 스레드가 동시에 Seal·Open에 써도 된다. AES-256-GCM 키는 만들 때
// libsodium 키 확장 상태(beforenm)를 한 번 계산해 키 저장소 슬롯에 함께 둔다.
// 이 기기에서 쓸 수 없는 알고리즘은 Unsupported, 열거값이 아니면 InvalidArgument다.
class AeadKey {
public:
    static Result<AeadKey> TryGenerate(AeadAlgorithm algorithm) noexcept;
    static Result<AeadKey> TryImport(AeadAlgorithm algorithm, Span<const uint8> keyBytes) noexcept;
    AeadAlgorithm Algorithm() const noexcept;
    bool IsEmpty() const noexcept;   // 기본 생성·이동 뒤. 모든 연산이 StaleHandle이다
};

Result<AeadKey> TryDeriveAeadKey(const KdfKey& master, uint64 subkeyId, KdfContext context,
                                 AeadAlgorithm algorithm) noexcept;   // 10.1절 타입 파생

// 고수준: nonce를 만들어 앞에 붙인다. 출력 = nonce ‖ 암호문 ‖ tag.
// randomNonceSafe가 아닌 알고리즘은 Unsupported다(96bit nonce는 9.5절 세그먼트나 hazmat 카운터로).
[[nodiscard]] Result<usize> TrySeal(const AeadKey& key, Span<const uint8> associatedData,
                                    Span<const uint8> plaintext, Span<uint8> output) noexcept;

// 실패하면 AuthenticationFailed이고 plaintext 전체를 0으로 지운다.
[[nodiscard]] Result<usize> TryOpen(const AeadKey& key, Span<const uint8> associatedData,
                                    Span<const uint8> sealed, Span<uint8> plaintext) noexcept;

} // namespace rocks::crypto

namespace rocks::crypto::hazmat {

Result<void> TryExportKey(const AeadKey& key, Span<uint8> output) noexcept;   // 10.4절

// nonce를 호출자가 책임진다. 결합 형식(암호문 ‖ tag)과 분리 형식이 있다. 모든 알고리즘을 받는다.
Result<usize> TrySealWithNonce(const AeadKey& key, Span<const uint8> nonce, Span<const uint8> associatedData,
                               Span<const uint8> plaintext, Span<uint8> sealed) noexcept;
Result<usize> TryOpenWithNonce(const AeadKey& key, Span<const uint8> nonce, Span<const uint8> associatedData,
                               Span<const uint8> sealed, Span<uint8> plaintext) noexcept;
Result<void> TrySealDetached(const AeadKey& key, Span<const uint8> nonce, Span<const uint8> associatedData,
                             Span<const uint8> plaintext, Span<uint8> ciphertext, Span<uint8> tag) noexcept;
Result<void> TryOpenDetached(const AeadKey& key, Span<const uint8> nonce, Span<const uint8> associatedData,
                             Span<const uint8> ciphertext, Span<const uint8> tag, Span<uint8> plaintext) noexcept;

// 순번 nonce 봉인기와 받는 쪽: 키를 소유하고 seq번째 nonce = iv XOR (0 ‖ BE64(seq))를 스스로 만든다
// (TLS 1.3 5.3절·QUIC·HPKE와 같은 구성). 출력은 암호문 ‖ tag이고 nonce는 싣지 않는다.
class AeadSequenceSealer {   // TryCreate(AeadKey&&, iv), TrySeal(ad, plaintext, sealed),
                             // NextSequence, IsExhausted, UsedBlocks
};
class AeadSequenceOpener {   // TryCreate(AeadKey&&, iv), TryOpen(ad, sealed, plaintext),
                             // NextSequence, IsExhausted, HasFailed
};

} // namespace rocks::crypto::hazmat
```

AEAD 규칙(2026-11-15 구현):

- 오류 순서는 빈 키 `StaleHandle` → 알고리즘(`Unsupported`) → 호출자 버퍼(길이·겹침 `InvalidArgument`, 출력 부족 `CapacityExceeded`) → 메시지 검사다. 메시지에서 온 길이(nonce + tag보다 짧은 입력, 분리 형식의 틀린 tag 길이, 상한을 넘는 암호문)와 tag 불일치는 모두 `AuthenticationFailed`이고 plaintext 전체를 0으로 지운다(결정 D5). 호출자 버퍼 오류는 쓰기 전에 돌려준다.
- 제자리(7절): 고수준은 `plaintext.Data() == output.Data() + nonceBytes`(봉인), `plaintext.Data() == sealed.Data() + nonceBytes`(열기)만, hazmat은 입력과 출력의 시작 주소가 같을 때만 겹침을 허용한다. AD·nonce·tag가 출력과 겹치면 `InvalidArgument`다.
- AES-256/128-GCM과 AEGIS는 6.1절의 HW 조건 하나를 함께 쓴다: `CpuFeatures`(x86-64 AES-NI·PCLMULQDQ·AVX, ARM64 AES·PMULL)와 libsodium 실행 감지가 모두 참이고 `crypto_aead_aes256gcm_is_available()`이 참이며, 경량 계층 커널 표가 자가 시험을 통과했을 때다(C2, 5.3절 `IsAesAvailable`). AES-128-GCM 일회성은 경량 계층이고 `implementation`은 커널 표 이름(`rocks-aes/x64`·`rocks-aes/arm64`)이다. AES-256-GCM 일회성은 libsodium이다(결정 D4, 6.4절). AES-GCM 키는 두 경로가 같은 상태를 미리 계산해 둔다(5.3절 키 객체). kx 세션 키는 32 byte 키 AEAD만 받으므로 AES-128-GCM은 기기와 관계없이 `InvalidArgument`다. 마지막 값은 ARM에서 `HAVE_ARMCRYPTO` 빌드 정의를 확인한다(ARM 실행 감지는 빌드 정의와 무관하다). 조건이 아니면 libsodium soft 경로로 내려가지 않고 `Unsupported`다(결정 D3). PMULL 없는 ARM64에서 AES-256-GCM 키를 만들면 libsodium `beforenm`이 PMULL 명령에서 죽으므로 이 확인이 필요하다(2026-11-15 리뷰 반영).
- hazmat의 GCM nonce는 96 bit만 받고 그 밖은 `Unsupported`, 다른 알고리즘의 nonce 길이 오류는 `InvalidArgument`다.
- AES-GCM·AEGIS 복호는 libsodium이 검증 전 평문을 호출자 버퍼에 잠시 쓴다. 실패하면 반환 전에 지우므로 "검증 전 평문을 내보내지 않는다"(2절)는 API 경계에서 성립한다. 호출 중 같은 버퍼를 다른 스레드가 읽지 않게 한다.
- 순번 봉인기(2026-11-15 설계 리뷰 반영). 신뢰할 수 있고 순서가 보장되는 전송에서 AES-GCM·AEGIS의 HW 처리량이 필요하거나 nonce를 싣지 않아야 할 때, 또는 TLS 1.3·QUIC·HPKE형 레코드 nonce와 맞출 때 쓴다. 그 밖의 레코드 채널은 secretstream(9.4절)이 낫다.
  - nonce 구성은 prefix ‖ 카운터가 아니라 nonce 길이 iv와의 XOR이다. prefix 구성은 iv = prefix ‖ 0⁸인 특수한 경우이고, XOR 구성은 키마다 다른 비밀 nonce 오프셋으로 다중 사용자 보강을 준다(aead-limits-13 7.1·7.2절의 GCM·ChaCha20-Poly1305 다중 사용자 bound가 nonce 무작위화를 전제한다. AEGIS는 RFC 10032 10.1.3절이 nonce의 남는 bit에 키 식별자를 넣기를 권하며 효과가 비슷하다). iv는 키와 같은 키 스케줄에서 나오고(`TryDerive`가 키와 다른 타입 tag로 함께 뽑는다) 전송하지 않으며 상수 라벨로 두지 않는다. 0이어도 nonce는 겹치지 않고 보강만 잃는다. kx로 얻은 키에는 iv를 0으로 둔다(kx는 키만 준다). iv는 키 저장소에 두고, 봉인·열기에 쓴 스택 nonce도 지운다.
  - 키 하나는 평생 sealer 하나(한 방향)만 쓴다. 방향을 prefix·라벨로 나누면 역할을 착각했을 때 양쪽이 같은 nonce로 봉인한다(fail-open). 두 방향은 kx의 rx·tx처럼 역할이 묶인 키로 나눈다(fail-closed). 같은 키 byte를 다시 가져오거나 같은 인자로 다시 파생하면 seq 0부터 같은 nonce가 나오므로 키는 세션마다 새것이어야 한다. kx 세션 키는 두 공개키에서 결정적으로 나오므로(BLAKE2b(X25519 ‖ client_pk ‖ server_pk)) 봉인하는 쪽 자기 키 쌍이 세션마다 새 ephemeral이어야 한다(2026-11-15 재검증 반영: 상대만 ephemeral이면 공격자가 상대의 이전 공개키와 레코드를 재전송해 장기 키 쪽이 같은 세션 키로 seq 0부터 다시 봉인하게 만든다). 장기 키 쪽은 무작위 nonce AEAD만 쓰고 상대의 첫 메시지는 재전송될 수 있다고 본다(0-RTT). 신원은 장기 Ed25519 키가 세션 공개키와 맥락(양쪽 공개키·역할)을 함께 서명해 확인한다. `TryDerive`의 subkeyId는 쓰기 전에 durable하게 올린 값이고 같은 master를 쓰는 다른 프로세스·기기와 카운터를 공유하지 않는다. 상태를 둘 수 없으면 128 bit 이상 무작위 salt의 HKDF로 세션 master를 먼저 만든다. 두 방향을 함께 만드는 편의 생성은 후속 "전송 채널 보호" 계층에 둔다.
  - 순번을 이어 가는 길(시작 seq 지정)은 두지 않는다. 봉인 뒤 저장 전에 죽으면 nonce가 다시 쓰이고(SP 800-38D 9.1절), 받는 쪽이 뒤처진 seq로 다시 만들면 재전송을 받아들인다. 재시작은 새 키(세션마다 새 ephemeral kx, 또는 쓰기 전에 durable하게 올린 epoch로 파생)로 한다. 경계 시험은 모듈 내부 접근점(`detail::SequenceTestAccess`, Private/AeadSequenceTestAccess.h)으로 seq·블록 집계를 옮긴다. 이 헤더는 사실상 이어 가기 API이므로 Tests 밖에서 include하지 않는다(정책 검사 `test-access-include`, 11절).
  - seq는 0부터 2^64 − 2까지 쓴다. `NextSequence() == 2^64 − 1`이 소진 표지이고 `IsExhausted()`가 이를 알린다. 봉인은 호출자 버퍼 오류(nonce·평문 상한·출력 길이·겹침)를 먼저 보고, 이 실패는 seq·블록 집계를 바꾸지 않는다. 그다음 AES-GCM은 정상 메시지가 키당 블록(메시지마다 평문 블록 + AD 블록 + 1) 한도 `maxBlocksPerKey`를 넘으면 소진으로 바꾼다(2026-11-15 재검증 반영: 너무 큰 메시지 하나가 키를 소진시키지 않는다). 소진 뒤 `TrySeal`은 `CapacityExceeded`이고, `IsExhausted()`가 false인 `CapacityExceeded`는 출력 부족이나 메시지 상한 초과다. 블록 한도는 마지막 방어선이고, 키 교체 메시지를 옛 키로 보낼 자리가 남도록 프로토콜은 그 전(예: 한도의 절반)에 재키잉한다.
  - opener는 순서를 고정하고 인증 실패(순서 바뀜·빠짐·재전송·변조·절단·다른 AD)와 소진 뒤 메시지를 `AuthenticationFailed`(plaintext 소거, sticky)로 받는다(TLS 1.3 5.2절 bad_record_mac과 같다). sticky라서 키당 위조 시도가 한 번이므로 받는 쪽 integrity limit을 셀 필요가 없다(같은 키로 opener를 다시 만들지 않을 때). 겹침은 실패 뒤에도 쓰기 전에 `InvalidArgument`다. sealed 길이로 정해지는 평문보다 plaintext가 작으면 인증 전에 `CapacityExceeded`(상태 불변)이고, 길이는 전송로에 보이는 값이라 새 정보가 아니지만 프레이밍 계층이 최대 레코드 길이를 먼저 막는다. 스트림 끝(절단) 인증은 프로토콜 몫이다(TLS close_notify처럼).
  - 명시적 순번 변형(DTLS·QUIC형)은 후속 "전송 채널 보호"에 둔다. 그 변형은 sticky가 아니고, 재전송 창(RFC 4303 3.4.3)을 두며, 인증 실패 수를 세어 integrity limit(RFC 9001 6.6절)을 지켜야 한다. 구현: `DatagramSealer`·`DatagramOpener`(Crypto/Channel.h, [SecureChannel](SecureChannel.ko.md) 3.2절, 2026-11-15).
- 시험: RFC 8439 2.8.2, XChaCha draft A.3.1 출력, AES-256-GCM과 빈 평문 tag(cryptography·pycryptodome로 계산), AEGIS draft 부록 TV1–TV3(tag256)과 libsodium 무작위 표 행(독립 Python 구현으로 재현). 모든 알고리즘의 hazmat 결합·분리 형식 변조(어댑터 소거가 관찰되는 AES-GCM 포함)·길이가 틀린 tag, 절단·다른 키·AD, 제자리와 시작 주소 ±1, 겹침 우선 순서, 출력 부족 시 무쓰기, GCM nonce 길이, `AeadInfo` 정확한 상한과 `CpuFeatures`로 정한 HW 사용 가능 여부·구현 이름, 타입 파생 KAT.

**Crypto/Signature.h · KeyExchange.h (요약)**

```cpp
enum class SignatureAlgorithm : uint8 { Ed25519 = 1, Ed25519ph = 2 };   // 영속 형식에 기록한다

struct Ed25519PublicKey {   // 일반 값 타입. TryFromBytes는 길이만 본다(점 검사는 검증 때)
    uint8 bytes[32] = {};
    static Result<Ed25519PublicKey> TryFromBytes(Span<const uint8> bytes) noexcept;   // InvalidArgument
    Span<const uint8> View() const noexcept;
};
struct Ed25519Signature { uint8 bytes[64] = {}; Span<const uint8> View() const noexcept; };

class Ed25519SigningKey {   // move-only, 키 저장소에 libsodium 비밀키(seed ‖ 공개키) 64 byte
public:
    static Result<Ed25519SigningKey> TryGenerate() noexcept;   // OS 난수 실패 DependencyFailed
    static Result<Ed25519SigningKey> TryFromSeed(Span<const uint8> seed) noexcept;
    Ed25519PublicKey PublicKey() const noexcept;
    bool IsEmpty() const noexcept;
};

Result<Ed25519Signature> TrySign(const Ed25519SigningKey& key, Span<const uint8> message) noexcept;
// 불일치·길이가 틀린 서명·올바르지 않은 공개키 점은 모두 AuthenticationFailed다.
Result<void> TryVerify(const Ed25519PublicKey& key, Span<const uint8> message,
                       Span<const uint8> signature) noexcept;
Result<void> hazmat::TryExportSeed(const Ed25519SigningKey& key, Span<uint8> output) noexcept;

// kx: 클라이언트·서버 역할에 따라 rx/tx 세션 키를 각각 AeadKey로 돌려준다(2026-11-15 구현·교차검증).
// algorithm은 키가 32 byte인 AEAD만 받고(16 byte는 기기와 관계없이 InvalidArgument), 키 byte는 crypto_kx와 같다.
// 상대 공개키가 small-order면 AuthenticationFailed다. 알고리즘은 키 byte에 묶이지 않는다(2절의 예외).
Result<KxSessionKeys> TryClientSessionKeys(const KxKeyPair& client, const KxPublicKey& server,
                                           AeadAlgorithm algorithm) noexcept;
Result<KxSessionKeys> TryServerSessionKeys(const KxKeyPair& server, const KxPublicKey& client,
                                           AeadAlgorithm algorithm) noexcept;

// sealed box: 수신자 공개키만으로 봉인한다. output.Size() >= message.Size() + 48(TryGetSealedBoxBytes).
// 수신자 키는 kx와 다른 타입이다(BoxKeyPair·BoxPublicKey). 제자리는 message가 output + 32에서 시작할 때만이다.
Result<usize> TrySealToRecipient(const BoxPublicKey& recipient, Span<const uint8> message, Span<uint8> output) noexcept;
Result<usize> TryOpenSealed(const BoxKeyPair& recipient, Span<const uint8> sealed, Span<uint8> plaintext) noexcept;
```

**Crypto/Kem.h (요약, 2026-11-15 구현·교차검증)**

```cpp
enum class KemAlgorithm : uint8 { XWing = 1, MlKem768 = 2 };   // 영속 형식에 기록한다. 기본은 X-Wing
class KemPublicKey {   // 알고리즘을 품은 값 타입(최대 1216 byte). TryFromBytes는 알고리즘·길이만 본다
public:
    static Result<KemPublicKey> TryFromBytes(KemAlgorithm algorithm, Span<const uint8> bytes) noexcept;
};
class KemKeyPair {     // move-only. 키 저장소에 seed만 둔다(X-Wing 32, ML-KEM d ‖ z 64, FIPS 203 seed 형식)
public:
    static Result<KemKeyPair> TryGenerate(KemAlgorithm algorithm) noexcept;
    static Result<KemKeyPair> TryFromSeed(KemAlgorithm algorithm, Span<const uint8> seed) noexcept;
    const KemPublicKey& PublicKey() const noexcept;
};
// 공유 비밀은 KdfKey로 준다(용도별 키는 타입 파생으로 뽑는다). 올바르지 않은 공개키(ML-KEM 계수 ≥ q, X-Wing X25519
// small-order)는 AuthenticationFailed와 암호문 소거. 캡슐 해제의 암호문 길이 오류와 X-Wing ct_X small-order(RFC 7748 6.1이
// 허용하는 엄격화, 공개 술어라 oracle 아님)는 AuthenticationFailed, ML-KEM 변조는 암묵적 거부(다른 공유 비밀).
Result<KdfKey> TryEncapsulate(const KemPublicKey& recipient, Span<uint8> ciphertext) noexcept;
Result<KdfKey> TryDecapsulate(const KemKeyPair& recipient, Span<const uint8> ciphertext) noexcept;
Result<void> hazmat::TryExportSeed(const KemKeyPair& key, Span<uint8> output) noexcept;
```

- KEM은 보낸 사람을 인증하지 않는다. 보내는 쪽은 수신자 공개키의 진위(서명·고정 키)를 먼저 확인한다.
- ML-KEM 캡슐 해제는 호출마다 seed를 확장 비밀키(2400 byte)로 펼쳐 키 저장소 임시 슬롯에 두었다가 지운다(libsodium X-Wing과 같은 방식). 확장 키를 들이거나 내보내는 경로가 없으므로 FIPS 203 7.3절 hash check가 구성상 성립한다. 키 쌍 하나는 작은 슬롯 하나만 쓴다(확장 키를 계속 두면 4 KiB 슬롯을 차지해 동시 키 쌍이 1000개 아래로 묶인다).

## 9. 스트리밍 API

### 9.1 공통 계약

- **객체 형태와 할당.** 스트림 객체는 move-only다. 키와 키에 의존하는 라이브러리 상태(키 확장 결과, keyed 해시·MAC 상태)는 SecretArena 슬롯에 고정한다(정렬 요구 5.1절, 10.2절). 키 없는 해시 상태와 세그먼트 버퍼·비트맵 같은 큰 버퍼는 `Allocator&` 인자로 받고, 기본값은 `GetSecretAllocator()`다(평문이 지나가는 버퍼이므로 해제 때 지운다. 키 저장소와는 따로다). 키 저장소가 한도까지 차거나 힙이 모자라면 `OutOfMemory`다. 이동된 뒤에 호출하면 `StaleHandle`이다. 한 객체는 한 번에 한 스레드만 쓴다.
- **소거.** Reset·소멸·실패 때 내부 상태와 버퍼를 `SecureZero`로 지운다.
- **sticky 오류.** 라이브러리 상태를 건드린 뒤의 실패는 Reset 전까지 같은 오류로 남는다. 라이브러리를 부르기 전에 거부한 경우(길이·겹침·잘못된 단계)는 상태를 바꾸지 않는다.
- **역압.** byte 스트림 형태는 `TryProcess(Span<const uint8> input, Span<uint8> output) → StreamProgress{consumed, produced, complete`}다. 출력이 모자라면 일부만 소비하고 돌아온다. `StreamProgress`는 Compression과 같은 타입이며 Core로 옮긴다(선행 P2).
- **단계.** 메시지 형태(secretstream)는 메시지 하나가 출력 하나다. 출력 Span이 `message + overhead`보다 작으면 `CapacityExceeded`이고 상태는 바뀌지 않는다.

### 9.2 증분 해시·MAC·서명

```cpp
namespace rocks::crypto {

// 키 없는 증분 해시(BLAKE2b·SHA-512·SHA3-256/512). 상태는 allocator에서 할당한다(9.1절). 키를 쓰는
// BLAKE2b keyed는 MacState다(상태가 키 저장소에 있다).
class HashState {
public:
    static Result<HashState> TryCreate(HashAlgorithm algorithm, usize digestBytes = 0,
                                       Allocator& allocator = GetSecretAllocator()) noexcept;
    Result<void> TryUpdate(Span<const uint8> input) noexcept;
    Result<void> TryUpdate(Span<const Span<const uint8>> pieces) noexcept;
    Result<void> TryFinish(Span<uint8> digest) noexcept;   // 뒤에는 Reset 전까지 Update가 InvalidArgument
    void Reset() noexcept;                                 // 같은 알고리즘·길이로 처음 상태
};

// 증분 XOF(SHAKE128/256, TurboSHAKE128/256). TrySqueeze를 여러 번 불러 이어지는 출력을 얻는다. 첫 Squeeze
// 뒤에는 Reset 전까지 Update가 InvalidArgument다.
class XofState { /* TryCreate(algorithm, domain, allocator), TryUpdate, TrySqueeze, Reset */ };

// HMAC-SHA-256/512, BLAKE2b keyed를 같은 형태로 둔다. 알고리즘은 키(MacKey)가 정한다. 상태는 키에서 파생된
// 비밀이라 키 저장소 슬롯에 둔다. 검증은 상수 시간이고 결과와 관계없이 상태를 끝낸다.
class MacState {
public:
    static Result<MacState> TryCreate(const MacKey& key) noexcept;
    Result<void> TryUpdate(Span<const uint8> input) noexcept;
    Result<void> TryFinish(Span<uint8> tag) noexcept;
    Result<void> TryFinishVerify(Span<const uint8> expectedTag) noexcept;   // 불일치 AuthenticationFailed
};

// Ed25519ph(RFC 8032, context 없음). 순수 Ed25519(m) 서명과 호환되지 않는다(알고리즘 ID가 다르다).
// SHA-512 상태는 키가 없으므로 allocator에서 할당한다. Finish 뒤에는 Reset 전까지 InvalidArgument다.
class Ed25519phSigner {
public:
    static Result<Ed25519phSigner> TryCreate(Allocator& allocator = GetSecretAllocator()) noexcept;
    Result<void> TryUpdate(Span<const uint8> input) noexcept;        // 조각 overload도 있다
    Result<Ed25519Signature> TryFinish(const Ed25519SigningKey& key) noexcept;   // 빈 키는 상태 불변
    void Reset() noexcept;
};

// 검증 실패·길이가 틀린 서명은 AuthenticationFailed이고, 결과와 관계없이 상태를 끝낸다. 실패는 sticky다(Reset 전까지
// TryUpdate·TryFinish가 AuthenticationFailed, MacState와 같다). TryFinishAny는 신뢰 공개키 여러 개(키 회전) 중 맞는 키의
// 순번을 돌려준다. 상태를 키마다 복사해 확인하므로 스트림을 다시 해시하지 않는다.
class Ed25519phVerifier { /* TryCreate, TryUpdate, TryFinish(const Ed25519PublicKey&, Span<const uint8> signature),
                             TryFinishAny(Span<const Ed25519PublicKey> trusted, Span<const uint8> signature), Reset */ };

} // namespace rocks::crypto
```

HKDF는 증분 extract를 두지 않는다(IKM은 공유 비밀처럼 짧다). extract·expand는 일회성이다. SHA-2·SHA-3·SHAKE 증분 객체는 같은 형태다. C3 점검(2026-11-15): 위 객체는 C1·C2에서 조각 overload(`Span<const Span<const uint8>>`)까지 모두 구현되어 있어 바꾸지 않았다.

### 9.3 AES-GCM 멀티파트와 AES 모드 (hazmat)

상호운용 형식이 GCM 하나로 큰 메시지를 요구할 때만 쓴다. 구현은 경량 계층이다. 암호화는 안전하게 증분할 수 있다. 복호화는 tag를 확인하기 전에 평문이 나가므로 이름에 그 사실을 적는다. CTR과 블록 암호 키도 같은 공개 헤더(`Crypto/AesModes.h`, 2026-11-15 구현)에 둔다. AES-CMAC은 MAC이므로 `Mac.h`의 `MacAlgorithm::AesCmac`이다.

```cpp
namespace rocks::crypto::hazmat {

// 블록 암호 모드용 키(16·32 byte). AeadKey와 섞지 않는다. 길이 오류 InvalidArgument(HW 판정보다 먼저), HW 없음 Unsupported.
class AesKey { /* TryGenerate(keyBytes), TryImport(keyBytes), KeyBytes, IsEmpty. move-only, 스케줄만 둔다 */ };

enum class AesCtrCounterWidth : uint8 { Bits32 = 32, Bits64 = 64, Bits128 = 128 };   // 카운터 블록의 끝 폭

class AesCtr {   // move-only, 키 스케줄 사본·남은 keystream을 키 저장소 슬롯에 둔다
public:
    static Result<AesCtr> TryCreate(const AesKey& key, Span<const uint8> initialCounterBlock,   // 16 byte
                                    AesCtrCounterWidth width = AesCtrCounterWidth::Bits128) noexcept;
    Result<void> TryProcess(Span<const uint8> input, Span<uint8> output) noexcept;   // 같은 길이, 제자리 허용
    void Reset() noexcept;                                                          // 지우고 빈 객체
};
Result<void> TryAesCtrXor(const AesKey& key, Span<const uint8> initialCounterBlock, Span<const uint8> input,
                          Span<uint8> output, AesCtrCounterWidth width = AesCtrCounterWidth::Bits128) noexcept;

class AesGcmEncryptor {
public:
    static Result<AesGcmEncryptor> TryCreate(const AeadKey& key, Span<const uint8> nonce) noexcept;
    Result<void> TryUpdateAssociatedData(Span<const uint8> associatedData) noexcept;   // 평문 Update 전에만
    Result<usize> TryUpdate(Span<const uint8> plaintext, Span<uint8> ciphertext) noexcept;
    Result<void> TryFinish(Span<uint8> tag) noexcept;
    void Reset() noexcept;
};

class AesGcmDecryptor {
public:
    static Result<AesGcmDecryptor> TryCreate(const AeadKey& key, Span<const uint8> nonce) noexcept;
    Result<void> TryUpdateAssociatedData(Span<const uint8> associatedData) noexcept;
    // 나온 평문은 TryFinishVerify가 성공하기 전까지 신뢰하지 않는다. 실패하면 호출자가 지금까지 받은 평문을 버린다.
    Result<usize> TryUpdateUnverified(Span<const uint8> ciphertext, Span<uint8> plaintext) noexcept;
    Result<void> TryFinishVerify(Span<const uint8> tag) noexcept;   // AuthenticationFailed(sticky)
    void Reset() noexcept;
    bool HasFailed() const noexcept;
};

} // namespace rocks::crypto::hazmat
```

- GCM 멀티파트: 키는 AES-128/256-GCM `AeadKey`만 받는다(다른 알고리즘 `InvalidArgument`, 빈 키 `StaleHandle`). nonce는 96 bit만 받는다(그 밖은 `Unsupported`). 상태는 키 스케줄을 복사하므로 키 객체를 먼저 버려도 된다(5.3절). (key, nonce)당 평문 상한 16 × (2^32 − 2) byte(NIST SP 800-38D)와 AD 상한 2^61 − 1 byte를 누적 길이로 세다가 넘는 호출은 읽기 전에 `CapacityExceeded`(상태 불변)다. 부분 블록의 keystream과 GHASH 입력을 상태에 보관하므로 `TryUpdate`는 입력과 같은 byte 수를 바로 출력한다(출력이 작으면 `CapacityExceeded`, 제자리가 아닌 겹침은 `InvalidArgument`). `TryUpdate`를 한 번이라도 부른 뒤(빈 입력 포함)의 AD와 끝난 뒤의 호출은 `InvalidArgument`(상태 불변)다. `TryFinish`의 tag 길이 오류는 `InvalidArgument`이고 다시 부를 수 있다. `TryFinishVerify`는 결과와 관계없이 끝내고, 불일치·잘린 tag는 `AuthenticationFailed`이며 Reset 전까지 모든 호출이 같은 오류다. 끝·Reset·소멸 때 상태를 지운다. Reset은 같은 nonce로 다시 시작하지 않도록 빈 객체로 만든다. 일회성(AES-128은 경량 계층, AES-256은 libsodium)과 byte가 같다(차등 시험).
- CTR: 카운터 블록의 끝 32·64·128 bit만 big-endian으로 올리고 그 폭 안에서만 올림한다. 이번 호출에 필요한 새 keystream 블록 수가 폭 안에 남은 값(2^폭 − c, 마지막 값을 쓴 뒤 0)보다 많으면 쓰기 전에 `CapacityExceeded`(상태 불변)다. 마지막 값으로 만든 블록의 남은 keystream은 계속 쓴다. 길이 불일치·제자리가 아닌 겹침·16 byte가 아닌 카운터 블록·열거값이 아닌 폭은 `InvalidArgument`다. CTR은 인증이 없으므로 MAC을 따로 붙인다.
- AES-CMAC(RFC 4493·SP 800-38B): `MacAlgorithm::AesCmac`(값 4, tag 16 byte). 키는 16·32 byte만(AES-192와 다른 길이, hazmat 짧은 키 경로도 `InvalidArgument`), 생성·파생 기본은 32 byte다. 오류 순서는 열거값(`InvalidArgument`) → HW(`Unsupported`) → 길이(`InvalidArgument`)로 `AeadKey`와 같다. 키가 스케줄을 품고, 일회성은 키 스케줄을, `MacState`는 사본(1 KiB 슬롯)을 쓴다. 마지막 1–16 byte를 서브키를 섞을 때까지 보류한다. KeyBlob은 AES-CMAC 키를 16·32 byte로만 담는다(10.4절).

### 9.4 메시지 스트림: secretstream

메시지 경계가 있고 순서대로 소비하는 흐름에 쓴다(로그·파일 전송·채널 위의 레코드). libsodium `crypto_secretstream_xchacha20poly1305` wire 형식을 그대로 쓴다.

```cpp
namespace rocks::crypto {

enum class SecretStreamTag : uint8 { Message = 0, Push = 1, Rekey = 2, Final = 3 };

class SecretStreamWriter {
public:
    static constexpr usize HeaderBytes = 24;
    static constexpr usize OverheadBytes = 17;   // 암호화된 tag 1 + MAC 16

    // header를 채운다. 수신 측은 첫 메시지보다 먼저 header를 받아야 한다.
    static Result<SecretStreamWriter> TryCreate(const SecretStreamKey& key, Span<uint8> header) noexcept;
    // output.Size() >= message.Size() + OverheadBytes. 반환은 쓴 byte 수.
    Result<usize> TryPush(Span<const uint8> message, Span<const uint8> associatedData, SecretStreamTag tag,
                          Span<uint8> output) noexcept;
    void Rekey() noexcept;   // 흔적이 남지 않으므로 수신 측도 같은 위치에서 Rekey해야 한다
    // Final을 보낸 뒤의 TryPush는 InvalidArgument다(libsodium은 막지 않는다. Final 뒤에 이어 쓴 메시지도 검증을 통과한다).
};

struct SecretStreamMessage {
    usize bytes;
    SecretStreamTag tag;
};

class SecretStreamReader {
public:
    static Result<SecretStreamReader> TryCreate(const SecretStreamKey& key, Span<const uint8> header) noexcept;
    Result<SecretStreamMessage> TryPull(Span<const uint8> sealed, Span<const uint8> associatedData, Span<uint8> output) noexcept;
    void Rekey() noexcept;              // 송신 측 Rekey와 같은 위치에서 부른다
    bool IsFinished() const noexcept;   // Final을 받았는가
    Result<void> TryFinish() noexcept;  // Final 없이 끝났으면 AuthenticationFailed(절단)
};

} // namespace rocks::crypto
```

- 메시지 하나는 최대 약 256 GiB(64 × (2^32 − 2))이고, 스트림 길이와 메시지 수에는 제한이 없다. 내부 카운터가 넘치기 전에 자동으로 rekey한다.
- `Rekey` 태그는 이전 키를 잊는다(전방 비밀성). `Final`은 암묵적으로 rekey한다. Final 뒤에 오는 입력은 `AuthenticationFailed`다.
- Reader는 `IsFinished()`를 확인하지 않은 채 스트림을 완료로 취급하지 않는다. `TryFinish`가 이것을 강제한다.
- pull이 돌려주는 tag byte는 0–255 어느 값이든 될 수 있다. 0–3 밖의 값은 Rocks writer가 만들지 않으므로 `AuthenticationFailed`로 다룬다.
- 메시지 길이가 `MESSAGEBYTES_MAX`를 넘는 입력은 libsodium에 넘기기 전에 거부한다(misuse handler 도달 금지, 5.1절).
- 구현(2026-11-15, `Crypto/SecretStream.h`·`Private/SecretStream.cpp`): `SecretStreamKey`(32 byte, `TryGenerate`·`TryImport`·`TryDeriveSecretStreamKey`(타입 tag `"RkStream"`)·`hazmat::TryExportKey`). Writer는 header를 `TryFillRandom`으로 만든 뒤 `init_pull`로 상태를 만든다(`init_push`와 같은 계산이고, OS 난수 실패가 Fatal 대신 `DependencyFailed`가 된다). 상태(52 byte)는 키 저장소 슬롯이다. 오류: Writer는 빈 객체 `StaleHandle` → Final 뒤·열거값 밖 tag·출력과 입력·AD 겹침 `InvalidArgument` → 메시지 상한 초과·출력 부족 `CapacityExceeded`(모두 쓰기 전, 상태 불변). Reader는 빈 객체 `StaleHandle` → 겹침 `InvalidArgument` → 실패 뒤·Final 뒤·길이 위반(17 byte 미만, 상한 초과) `AuthenticationFailed` → 출력 부족 `CapacityExceeded`(인증 전, 상태 불변) → MAC 불일치·0–3 밖 tag `AuthenticationFailed`다. `AuthenticationFailed`는 출력 전체와 상태를 지우고 Reset 전까지 같다. header 길이가 틀리면 Reader `TryCreate`가 `AuthenticationFailed`, Writer `TryCreate`가 `InvalidArgument`다. 제자리 처리는 받지 않는다(겹침은 모두 `InvalidArgument`). 두 객체 모두 `Reset`(빈 객체)을 두고, Reader는 `HasFailed`, Writer는 `IsFinished`를 둔다.
- 검증: Wycheproof 상태 벡터(5.4절), PyNaCl 1.6.2가 만든 스트림(Message·Push·Rekey·Final, tag 0x04 거부) 열기, bit 뒤집기·잘린 메시지·다른 AD·순서 바꾸기·다른 키·Final 없는 끝·Final 뒤 입력, 길이만 큰 Span으로 상한 + 1 거부(`SecretStreamTests.cpp`).

### 9.5 세그먼트 스트림과 임의 접근

파일처럼 길이가 크고, 일부만 읽거나 병렬로 처리해야 하는 데이터에 쓴다. 구성은 Hoang·Reyhanitabar·Rogaway·Vizár의 online AE(STREAM)이고, Tink AES-GCM-HKDF·age와 같은 골격(스트림별 HKDF 키 + 카운터 nonce + last 표시)을 쓴다. 다만 Tink 형식을 그대로 쓰지 않고 헤더를 더한다. Tink 형식에는 key commitment가 없고 알고리즘이 AES-GCM으로 고정되며, 알고리즘·세그먼트 크기·키 ID가 헤더에 없어 키 객체가 따로 실어 날라야 하기 때문이다(결정 D11).

| 구역 | 필드 | 크기 |
|---|---|---|
| 헤더 | magic `"RSEG"` | 4 B |
| 헤더 | 버전 | 1 B |
| 헤더 | 알고리즘(1 = ChaCha20-Poly1305-IETF, 2 = AES-256-GCM) | 1 B |
| 헤더 | log2 S(12–24, 4 KiB–16 MiB) | 1 B |
| 헤더 | flags(0) | 1 B |
| 헤더 | keyId(RFC 9562 byte 순서, 비밀 아님) | 16 B |
| 헤더 | salt | 32 B |
| 헤더 | commitment | 32 B |
| 세그먼트 i(마지막 제외) | 암호문 ‖ tag | S − 16 B ‖ 16 B |
| 마지막 세그먼트 | 암호문 ‖ tag(last = 1로 봉인) | 1 ~ S − 16 B ‖ 16 B(빈 평문이면 0 ‖ 16) |
| 세그먼트 i의 nonce | 0x00 × 3 ‖ BE64(i) ‖ last | 12 B |

헤더는 88 B다. S = 2^log2 S = 세그먼트 암호문 크기(tag 포함).

- **키 파생.** `K ‖ C = HKDF-SHA256(ikm = 마스터 키, salt = salt, info = "rocks.segment.v1" ‖ 헤더[0, 56) ‖ SHA-256(AD), L = 64)`. 앞 32 byte는 세그먼트 키, 뒤 32 byte는 commitment다. 헤더의 알고리즘·세그먼트 크기·키 ID가 info로 키에 묶이므로, 헤더를 바꾸면 commitment가 맞지 않는다. 세그먼트 AEAD의 AAD는 비어 있다.
- **commitment.** 복호 쪽은 세그먼트를 하나라도 열기 전에 commitment를 상수 시간으로 비교한다. 잘못된 키, 헤더 변조, 알고리즘·크기 downgrade를 처음에 거부한다. 기본은 키 ID로 키를 찾고 시도 복호를 하지 않지만, 비밀번호 후보처럼 피할 수 없는 경우에도 commitment가 partitioning oracle을 막는다. commitment는 info 밖에 있지만 문제가 없다. 앞 56 byte는 키 파생으로, commitment 32 byte는 동등 비교로 88 byte 전체가 인증된다.
- **nonce.** 스트림마다 키가 새로 파생되므로 무작위 prefix 없이 `0³ ‖ BE64(i) ‖ last`로 충분하다. 세그먼트 수는 2^32로 제한한다.
- **정규 인코딩.** 마지막을 뺀 모든 세그먼트는 S − 16 byte로 꽉 채운다. 빈 평문은 빈 마지막 세그먼트 하나로 표현한다. 세그먼트가 둘 이상이면 마지막 세그먼트는 비어 있을 수 없다. 그래서 같은 평문의 인코딩은 하나이고, 복호 쪽은 비정규 인코딩을 거부한다.
- **크기 산술.** 전체 암호문 크기 T는 헤더를 포함한다. body = T − 88, n = ⌈body / S⌉, 마지막 세그먼트 크기 r = body − (n − 1)·S다. body < 16, n > 2^32, n ≥ 2인데 r ≤ 16이면 `AuthenticationFailed`다. 임의 접근 `TryOpen`에 넘긴 `lastSegment.Size()`는 r과 같아야 한다. flags의 모르는 bit, 범위 밖 log2 S도 거부한다. keyId는 RFC 9562 byte 순서로 기록한다.
- **마지막 판정에는 시도가 없다.** 순차 복호기는 꽉 찬 세그먼트 뒤에 byte가 더 오면 last = 0으로, 입력이 끝나 `TryFinish`가 오면 last = 1로 연다. 같은 세그먼트를 last = 0으로 실패한 뒤 last = 1로 다시 열지 않는다. 그래서 마지막 세그먼트의 평문은 `TryFinish`에서 나온다.
- **AES 스트림 상한.** AES-256-GCM 형식은 스트림당 세그먼트 암호문 합(헤더 제외, tag 포함)을 2^40 − 16 byte로 제한한다(C3 확정, 2026-11-15). 세그먼트 AD가 비어 세그먼트마다 GCM 블록이 ⌈암호문 / 16⌉개이므로 블록 2^36 − 1개이고, 11절 순번 봉인기와 같은 기밀성 목표 (σ + 1)²/2^129 ≤ 2^−57이다(스트림마다 키가 새로 파생되므로 키당 한도 = 스트림당 한도). 평문으로는 2^40 − 2^44/S − 16 byte다(64 KiB 세그먼트면 약 1 TiB − 256 MiB). 처음 설계의 평문 2^48 byte(약 2^−41)는 같은 목표로 다시 정해 바꿨다. 암호기·병렬 writer는 넘는 평문을 `CapacityExceeded`로, 복호 쪽은 넘는 크기를 형식 위반(`AuthenticationFailed`)으로 거부한다. ChaCha20-Poly1305 형식은 세그먼트 수 상한(평문 2^32 × (S − 16))만 둔다.
- **세그먼트 연산.** ChaCha20-Poly1305는 libsodium `crypto_aead_chacha20poly1305_ietf_*_detached`가 맡는다. AES-256-GCM은 libsodium `crypto_aead_aes256gcm_*_detached`가 맡고, 파생 키는 `beforenm`으로 한 번만 확장한다. HW가 없으면 AES-256-GCM 형식은 `Unsupported`다. HKDF는 libsodium이다.
- **Tink와의 관계.** 세그먼트 단위 구성은 같지만 wire 형식은 호환되지 않는다. Tink 상호운용이 필요해지면 별도 모드로 더한다.
- **형식 명세.** byte 배치·파생·검증 순서·시험 벡터의 규범 명세는 [세그먼트 스트림 형식 v1](CryptoSegmentFormat.ko.md)이다. 벡터 V1–V3은 C++ golden 시험과 독립 Python 구현(pyca/cryptography, OpenSSL)이 같은 값을 낸다.
- **구현(2026-11-15).** `Crypto/SegmentStream.h`, `Private/SegmentFormat.cpp`(헤더·크기 산술·파생·세그먼트 연산·키), `SegmentStreamSequential.cpp`(순차 암·복호), `SegmentStreamRandomAccess.cpp`(임의 접근·병렬 writer). 세그먼트 키는 파생한 32 byte로 만든 `AeadKey`(ChaCha20-Poly1305-IETF 또는 AES-256-GCM, libsodium `beforenm` 상태 포함)이고 키 저장소에 있다. SHA-256(AD)는 libsodium `crypto_hash_sha256`이다(D7의 "HMAC·HKDF 같은 파생 내부" 사용). 마지막 세그먼트 검증 전용 경로는 libsodium 복호 함수에 `m = NULL`을 넘기는 tag 확인이다(ChaCha20-Poly1305-IETF·AES-256-GCM `afternm` 모두 평문을 쓰지 않는다). salt는 공개 `TryCreate`가 OS 난수로 만들고, 고정 salt 생성은 모듈 내부 `detail::SegmentStreamFactory`(`Private/SegmentStreamInternal.h`, 11절 결정성)가 맡는다. 위치 이동 시험(세그먼트 2^32·AES 한도 경계)은 `Private/SegmentStreamTestAccess.h`로만 한다.
- **설계에 더한 API.** `SegmentLayout`(n·S·마지막 세그먼트 위치·크기·평문 크기)과 `TryGetSegmentLayout(params, 전체 크기)`(크기 산술, 위반 `AuthenticationFailed`), `TryGetSegmentStreamCiphertextBytes`·`TryGetSegmentStreamMaxPlaintextBytes`, `SegmentHeaderInfo::Params()`, 임의 접근·병렬 writer의 `Layout()`, `SegmentRange{firstSegment, segmentCount, ciphertextOffset, ciphertextBytes, firstSegmentOffset}`, 순차 객체의 `Reset`(빈 객체), `TryDeriveSegmentStreamKey`(타입 tag `"RkSegKey"`)·`hazmat::TryExportKey`. 임의 접근 사용자는 Peek → `TryGetSegmentLayout`으로 마지막 세그먼트 구간을 알아낸 뒤 `TryOpen`을 부른다.
- **구현 규칙.** 순차 객체의 `StreamProgress.complete`는 `TryProcess`에서 "입력을 다 소비했고 내보낼 출력이 남지 않음", `TryFinish`에서 "스트림 끝까지 다 내보냄"이다(끝난 뒤 `TryFinish`는 `{0, 0, true}`, Compression과 같다). 암호기는 헤더를 첫 출력으로 내고, 평문 한 세그먼트와 봉인 한 세그먼트 버퍼(2S − 16 byte)를 둔다. 복호기는 헤더 88 byte가 모이는 즉시 commitment까지 확인하고, 세그먼트 버퍼(S byte)에서 제자리로 연 뒤 내보낸다. 복호기의 실패(`AuthenticationFailed`·헤더의 `Unsupported`·헤더 처리 중 `OutOfMemory`)는 그 호출의 출력 전체를 지우고 Reset 전까지 같다. 임의 접근 `TryOpen`은 헤더를 지역 사본으로 한 번만 읽고, `lastSegment` 길이가 r과 다르면 `AuthenticationFailed`다. `TryOpenSegment`·`TrySealSegment`는 같은 시작 주소의 제자리만 허용한다. 병렬 writer의 claim은 비트맵 word의 원자적 OR이고(세그먼트 2^32개면 비트맵 512 MiB), 길이 오류·출력 부족은 claim 전에 돌아온다.

```cpp
namespace rocks::crypto {

enum class SegmentStreamAlgorithm : uint8 {
    ChaCha20Poly1305 = 1,   // 모든 기기
    Aes256Gcm = 2,          // HW 기기군(없으면 Unsupported)
};

struct SegmentStreamParams {
    SegmentStreamAlgorithm algorithm = SegmentStreamAlgorithm::ChaCha20Poly1305;
    uint8 segmentSizeLog2 = 16;   // 64 KiB, tag 포함(결정 D6). 12–24
};

// 마스터 키 32 byte와 키 ID다. 헤더의 keyId는 key.Id()에서 온다(따로 지정하지 않으므로 어긋날 수 없다).
// 매개변수는 헤더에 기록되고 키 파생에 묶이므로, 같은 마스터 키를 다른 매개변수로 써도 파생 키가 다르다.
// Nil KeyId는 InvalidArgument다.
class SegmentStreamKey { /* TryGenerate(KeyId), TryImport(KeyId, Span<const uint8>), Id(), IsEmpty() */ };

struct SegmentHeaderInfo {
    SegmentStreamAlgorithm algorithm;
    uint8 segmentSizeLog2;
    KeyId keyId;
};

// 헤더를 읽어 키 ID와 매개변수를 돌려준다. 이 값은 commitment 확인 전이라 아직 인증되지 않았다.
// 키 ID로 KeyRing에서 키를 찾는 데만 쓰고, 복호 객체를 만들 때 commitment로 확인된다.
// magic 불일치·범위 밖 크기·모르는 flags bit는 AuthenticationFailed, 모르는 버전·알고리즘은 Unsupported다.
Result<SegmentHeaderInfo> TryPeekSegmentHeader(Span<const uint8> header) noexcept;

class SegmentStreamEncryptor {
public:
    // 헤더(salt 무작위, commitment)를 만든다. associatedData는 HKDF info로 스트림 전체에 한 번 묶인다.
    // 마지막 세그먼트인지는 다음 입력이나 TryFinish가 와야 알 수 있으므로, 출력은 한 세그먼트 늦게 나온다.
    static Result<SegmentStreamEncryptor> TryCreate(const SegmentStreamKey& key, const SegmentStreamParams& params,
                                                    Span<const uint8> associatedData,
                                                    Allocator& bufferAllocator = GetSecretAllocator()) noexcept;
    Result<StreamProgress> TryProcess(Span<const uint8> plaintext, Span<uint8> output) noexcept;
    Result<StreamProgress> TryFinish(Span<uint8> output) noexcept;   // 마지막 세그먼트(last = 1)를 닫는다
};

class SegmentStreamDecryptor {
public:
    static Result<SegmentStreamDecryptor> TryCreate(const SegmentStreamKey& key, Span<const uint8> associatedData,
                                                    Allocator& bufferAllocator = GetSecretAllocator()) noexcept;
    // 세그먼트 하나를 다 받아 검증한 뒤에만 그 평문을 내보낸다. 버퍼는 세그먼트 하나 크기다.
    // 내보낸 평문은 세그먼트 단위로 인증됐지만, 스트림이 완결됐는지는 TryFinish가 성공해야 확정된다.
    Result<StreamProgress> TryProcess(Span<const uint8> ciphertext, Span<uint8> plaintext) noexcept;
    // 입력 끝을 알린다. 남은 세그먼트를 last = 1로 열어 그 평문을 내보낸다(출력이 모자라면 반복 호출).
    // 마지막 세그먼트가 없거나 검증에 실패하면 AuthenticationFailed(절단)다.
    Result<StreamProgress> TryFinish(Span<uint8> plaintext) noexcept;
};

// 전체 암호문 크기를 알 때 쓴다. 세그먼트마다 독립적으로 검증·복호한다. const 함수는 여러 스레드가 동시에 불러도 된다.
class SegmentStreamRandomAccess {
public:
    // 열 때 commitment를 확인하고 마지막 세그먼트를 한 번 검증한다(절단 검출, 정확한 평문 크기).
    // 마지막 세그먼트 검증은 평문을 쓰지 않는 검증 전용 경로를 쓰므로 scratch가 필요 없다.
    // ciphertextBytes는 헤더를 포함한 전체 크기다.
    static Result<SegmentStreamRandomAccess> TryOpen(const SegmentStreamKey& key, Span<const uint8> associatedData,
                                                     Span<const uint8> header, Span<const uint8> lastSegment,
                                                     uint64 ciphertextBytes) noexcept;
    uint64 PlaintextBytes() const noexcept;
    uint64 SegmentCount() const noexcept;

    // 평문 [offset, offset + bytes)를 읽는 데 필요한 세그먼트 번호 범위와 암호문 byte 범위다.
    // bytes == 0이면 빈 범위다. offset + bytes가 overflow하거나 평문 크기를 넘으면 InvalidArgument다.
    Result<SegmentRange> TryMapPlaintextRange(uint64 offset, uint64 bytes) const noexcept;

    // 세그먼트 index의 암호문을 검증·복호한다. 마지막 세그먼트인지는 ciphertextBytes로 정해진다.
    Result<usize> TryOpenSegment(uint64 index, Span<const uint8> ciphertextSegment, Span<uint8> plaintext) const noexcept;
};

// 전체 크기를 아는 병렬 봉인용이다. 같은 index를 두 번 봉인하면 AlreadyExists다(nonce 재사용 차단).
// 순서: 길이 검사 → index 원자적 claim → 봉인. claim 뒤에는 실패 경로가 없다. 성공 뒤에 표시하면
// 두 스레드가 같은 index를 서로 다른 평문으로 동시에 봉인할 수 있기 때문이다. 비트맵은 bufferAllocator에서 할당한다.
class SegmentStreamParallelWriter {
public:
    static Result<SegmentStreamParallelWriter> TryCreate(const SegmentStreamKey& key, const SegmentStreamParams& params,
                                                         Span<const uint8> associatedData, uint64 plaintextBytes,
                                                         Span<uint8> header,
                                                         Allocator& bufferAllocator = GetSecretAllocator()) noexcept;
    Result<usize> TrySealSegment(uint64 index, Span<const uint8> plaintext, Span<uint8> sealed) noexcept; // 스레드 안전
};

} // namespace rocks::crypto
```

| 위협 | 막는 장치 |
|---|---|
| nonce 재사용 | 스트림마다 256 bit 무작위 salt로 키를 새로 파생하고, 그 안에서 세그먼트 번호를 nonce로 쓴다. 병렬 writer는 같은 index 재봉인을 거부한다. 제자리 수정은 지원하지 않고, 바꾸려면 새 salt로 전체를 다시 쓴다. |
| 절단 | 마지막 세그먼트만 last = 1로 봉인한다. 경계에서 잘라내면 새로 끝이 된 세그먼트의 검증이 실패한다. |
| 재정렬·중복·이식 | 세그먼트 번호가 nonce에 들어간다. 다른 스트림의 세그먼트는 키가 달라 검증에 실패한다. |
| 뒤에 덧붙이기 | last 세그먼트 뒤의 byte는 거부한다. |
| AD·헤더 바꿔치기, downgrade | AD 해시와 헤더(알고리즘·크기·키 ID·salt)가 HKDF info로 키 파생에 들어가고, commitment가 맞지 않으면 세그먼트를 열기 전에 거부한다. |
| 미검증·미완결 평문 | 세그먼트 단위로 검증한 뒤에 내보낸다. 스트림 완결은 `TryFinish`가 확정한다. 전체가 원자적이어야 하는 소비자(세이브 파싱)는 Finish 뒤에 쓰거나 일회성 API를 쓴다. |
| 여러 키 시도(partitioning oracle) | commitment로 키를 먼저 확인한다. 기본은 헤더 키 ID로 KeyRing에서 찾는다. |

### 9.6 압축과의 결합

순서는 압축 다음 암호화다. 두 스트림은 같은 `StreamProgress` 역압 규약을 쓰므로 호출자가 중간 버퍼 하나로 잇는다. 공격자가 영향을 주는 데이터와 비밀이 한 압축 문맥에 섞이면 길이가 정보를 흘린다(CRIME·BREACH와 같은 원리). 그래서 전송 채널에서는 둘을 같은 압축 문맥에 넣지 않는다는 규칙을 API 주석에 적는다.

```cpp
// 입력 조각 하나를 압축 → 세그먼트 암호화로 흘린다. 양쪽 모두 consumed가 입력보다 작을 수 있다(역압).
Span<const uint8> pending = input;
while (!pending.IsEmpty()) {
    auto packed = compress.TryProcess(pending, Span<uint8>(stage));
    if (!packed) {
        return packed.Error();
    }

    pending = pending.RightChop(packed.Value().consumed);
    Span<const uint8> sealing = Span<const uint8>(stage).Left(packed.Value().produced);

    while (!sealing.IsEmpty()) {
        auto sealed = encryptor.TryProcess(sealing, sink.Reserve());
        if (!sealed) {
            return sealed.Error();
        }

        sink.Commit(sealed.Value().produced);
        sealing = sealing.RightChop(sealed.Value().consumed);
    }
}
```

구현(2026-11-15): Crypto는 Compression에 의존하지 않는다. `CryptoSegmentStreamWithCompression` 시험이 위 형태(Lz4Frame 압축 → 세그먼트 암호화, 중간 버퍼 300 byte, 암호화 출력 창 97 byte)로 왕복하고 복호 → 해제로 원래 입력을 되찾는다(시험 실행 파일만 두 모듈을 함께 링크한다). 압축 문맥 규칙은 `SegmentStream.h`·`SecretStream.h` 주석에 적었다.

### 9.7 비동기·병렬

- `AsyncCrypto.h`: `TrySubmitSegmentEncrypt`·`TrySubmitSegmentDecrypt`는 큰 버퍼를 세그먼트 단위로 쪼개 WorkerScheduler에서 병렬로 처리한다. 봉인은 `SegmentStreamParallelWriter`, 복호는 `SegmentStreamRandomAccess`의 const 함수를 쓴다. 세그먼트들은 헤더를 공유할 뿐 서로 독립이므로, salt를 고정한 시험에서 결과 byte는 동기 API와 같다.
- `AsyncFile` 읽기 완료 → 세그먼트 복호를 의존 작업으로 잇는다(Compression 5절과 같은 형태). 취소는 세그먼트 경계에서 확인한다. 실패·취소된 작업은 2절 규칙대로 출력 전체를 지운다.
- 출력 버퍼의 할당기·수명·취소·용량 계약은 `AsyncCompression/AsyncCompression.h`의 규약을 따른다. 키 의존 상태는 9.1절대로 SecretArena에 있다.

구현(C4, 2026-11-15):

- **작업 구성.** 한 제출은 worker 작업을 `SegmentCryptoOptions::maxTasks`개(0이면 worker 수, 상한 `SegmentCryptoMaxTasks` = 8, 세그먼트 수 이하) 만들고, 작업들은 다음 세그먼트 번호를 원자적으로 가져가 처리한다. 결과 보관소 `SegmentCryptoTask`는 `CompressionTask`와 같은 틀이다(이동 불가, 한 스레드, `TryGetResult`·`TryWaitResult`·`RequestCancel`, 소멸자는 진행 중이면 취소하고 기다린다). 첫 작업 제출이 거부되면 제출 전체가 실패하고(task·출력 불변), 그 뒤의 거부는 작업 수만 줄인다. 작업 상태·body는 `GetSecretAllocator()`다. 입출력이 겹치면 `InvalidArgument`다(제자리 없음).
- **동기와 같은 결과.** 암호화는 제출 때 writer를 만든다(헤더·키 파생, 제출 오류가 `ParallelWriter::TryCreate`와 같다. 헤더는 성공하면 마지막에 출력 앞에 쓴다). 복호는 처음 시작한 작업 하나가 동기 사용자와 같은 순서(헤더 지역 사본 → Peek → `TryGetSegmentLayout` → `TryOpen` → 출력 크기 `CapacityExceeded`)로 열고, 다른 작업은 그 결과를 이벤트로 기다린다(여는 작업이 실행 중이므로 기다림은 그 처리 시간으로 묶인다). 세그먼트 하나라도 실패하면 결과는 그 오류다(처음 기록한 이유가 남는다).
- **의존과 이어 붙이기.** dependencies는 모든 작업에 걸린다. 헤더는 작업 시작 때 읽으므로 앞선 읽기 작업이 암호문을 채울 수 있다. 이어 붙일 작업은 `Handles()` 전체를 dependency로 준다. 실패를 본 작업은 `Fail`로, 자기 취소 요청으로 멈춘 작업은 `Cancelled`로 끝나므로 성공하지 못한 제출 뒤의 작업은 실행되지 않는다.
- **결과 기록과 소거.** 작업 capture마다 공유 상태의 표(참조) 하나를 들고, 마지막 표가 놓일 때 결과 slot을 쓰고 성공이 아니면 출력 전체를 지운다. capture는 body가 실행되지 않아도(실행 전 취소·의존 실패·Shutdown) terminal 게시 전에 정리되므로, 모든 handle이 terminal이면 결과와 소거가 끝나 있다. 별도 join 작업을 두지 않은 이유: 대기 중인 작업은 취소 요청을 받으면 의존 작업이 끝나기 전에 terminal이 되므로, join이 끝났다고 다른 작업이 출력을 다 썼다는 보장이 없다.
- **취소.** 작업 시작 전과 세그먼트 경계마다 확인한다(세그먼트 하나, 최대 16 MiB는 끊지 않는다). 한 작업이 멈추면 다른 작업도 다음 경계에서 멈춘다. 모든 세그먼트를 처리한 뒤 늦게 걸린 취소는 결과를 바꾸지 않는다(`CompressionTask`와 같다).
- **수명.** 키는 제출 때 필요한 만큼 복사한다(암호화는 파생 세그먼트 키를 품은 writer, 복호는 마스터 키 사본). 그래서 제출 뒤 키를 버리거나 KeyRing에서 빼도 된다. 입력·출력 Span과 복호의 associatedData는 결과가 나올 때까지 유효해야 한다.
- **결정성(11절).** 고정 salt와 세그먼트 경계 probe(호출마다 넘기는 값)를 받는 `detail::TrySubmitSegmentEncryptWithSalt`·`TrySubmitSegmentDecryptWithProbe`(`Private/AsyncCryptoInternal.h`)로 시험한다.

## 10. 키와 비밀 메모리

### 10.1 타입

```cpp
namespace rocks::crypto {

// 비밀 byte다. 복사할 수 없고 이동만 된다. 소멸·Reset 때 0으로 지운다. 저장소는 크기로 정한다:
// 4 KiB 이하는 키 저장소(SecretArena) 슬롯, 그보다 크면 소거하는 버퍼 할당기(GetSecretAllocator).
// 키는 모두 4 KiB 이하다(가장 큰 ML-KEM-768 비밀키가 2400 byte).
class SecretBytes {
public:
    static Result<SecretBytes> TryCreate(usize bytes) noexcept;                 // 0으로 채운다
    static Result<SecretBytes> TryGenerate(usize bytes) noexcept;               // OS 난수
    static Result<SecretBytes> TryCopyFrom(Span<const uint8> source) noexcept;  // source는 호출자가 지운다
    Span<const uint8> View() const noexcept;   // 수명은 이 객체와 같다
    Span<uint8> MutableView() noexcept;        // 생성 직후 채우는 경로(파생·가져오기)에서만
    usize Size() const noexcept;
    bool IsEmpty() const noexcept;
    void Reset() noexcept;                     // 지우고 빈 값이 된다
};

// 알고리즘에 묶인 키 타입은 SecretBytes를 품는다: AeadKey, MacKey, SecretStreamKey, SegmentStreamKey, BoxKeyPair, KemKeyPair,
// Ed25519SigningKey, KxKeyPair, ChannelServerKey(보안 채널 서버 정적 키, Id를 품는다). 공개 값(Ed25519PublicKey, KxPublicKey,
// ChannelPublicKey)은 일반 값 타입이다. hazmat::AesKey(CTR)는
// 키 byte 대신 AES 키 스케줄 슬롯만 둔다(9.3절).

// 마스터 키에서 용도별 키를 타입으로 파생한다(BLAKE2b KDF). 결과가 일반 Span에 쓰이지 않는다.
// context는 컴파일 시간 8 문자 상수(KdfContext, consteval 생성)만 받는다. 런타임·외부 입력으로 context를
// 고르지 못하게 하려는 것이다. 엔진이 쓰는 context는 Crypto/Labels.h에 모아 중복을 시험으로 막는다("Rk" 접두는 엔진 예약, 첫 등록은 KeyBlob의 "RkKeyBlb"; 첫 엔진
// context와 함께 둔다). 타입 파생(TryDeriveMacKey·TryDeriveKdfKey·TryDeriveAeadKey)은 BLAKE2b personal 뒤 8 byte에
// 키 타입·알고리즘 tag("RkKdfKey", "RkMacK" 0 alg, "RkAead" 0 alg; 모듈 내부 KdfInternal.h 한 곳에 모은다)를 넣어,
// 같은 (id, context)라도 타입·알고리즘마다 독립인 키를 낸다(2026-11-15 리뷰 반영). TryDeriveSecret은 libsodium
// crypto_kdf와 같은 byte다. 구현(Kdf.h): KdfKey, TryDeriveSecret(16–64 byte)·TryDeriveMacKey·TryDeriveKdfKey(키 계층)·TryDeriveBoundKdfKey(transcript 조각을 길이 접두로 묶은 하위 마스터, KEM 공유 비밀을 맥락에 묶을 때),
// HKDF-SHA-256/512(TryHkdf·TryHkdfExtract·TryHkdfExpand, 상호운용용 Span 출력). TryDeriveAeadKey는 Aead.h에 있다.
// 스트림 키 파생(TryDeriveSegmentStreamKey 타입 tag "RkSegKey", TryDeriveSecretStreamKey "RkStream")은 C3에서
// SegmentStream.h·SecretStream.h에 더했다.
Result<AeadKey> TryDeriveAeadKey(const KdfKey& master, uint64 subkeyId, KdfContext context,
                                 AeadAlgorithm algorithm) noexcept;
Result<SegmentStreamKey> TryDeriveSegmentStreamKey(const KdfKey& master, uint64 subkeyId, KdfContext context,
                                                   KeyId id) noexcept;

} // namespace rocks::crypto
```

### 10.2 SecretArena

`sodium_malloc`은 쓰지 않는다. 할당마다 페이지를 3–4개 더 쓰고, 정렬을 보장하지 않으며, Rocks 할당 추적 밖에 있기 때문이다. 키처럼 작은 비밀이 많은 엔진에는 맞지 않는다. 대신 PlatformBase `VirtualMemoryRegion`(예약·부분 commit) 위에 슬롯 할당기를 둔다. 계획했던 PlatformBase `SecureMemory`(선행 P5)는 기존 `VirtualMemoryRegion`과 `sodium_mlock`으로 충족되어 두지 않는다(2026-11-14 구현).

저장소는 둘로 나눈다(2026-11-14 리뷰 반영). 하나로 두면 평문 버퍼가 arena를 채운 뒤에 만든 키가 잠금·guard page 없는 힙으로 조용히 밀려나기 때문이다.

- **키 저장소(SecretArena, 모듈 내부 API).** 키·키 상태 전용이다. 배치는 [guard page][arena 64 KiB][guard page]이고, guard page는 예약만 하고 commit하지 않는다. arena는 4 KiB chunk 16개이고, chunk는 처음 쓸 때 크기 등급(64 B–4 KiB, 2의 거듭제곱) 하나를 맡아 슬롯 사용을 64 bit 마스크로 기록한다. 슬롯은 등급 크기로 정렬된다(최소 64 byte). 해제하면 슬롯 전체를 즉시 지운다. 모든 arena가 차면 새 arena를 덧붙이고(최대 64개, 4 MiB), 그래도 없으면 `OutOfMemory`다. 힙으로 넘기지 않는다. libsodium 상태, AES 라운드 키·H 거듭제곱, 키, 4 KiB 이하 `SecretBytes`가 여기에 있다. 남의 주소나 이중 해제는 Fatal이다(키 저장소 손상은 결함이다). 단 그 사이 같은 슬롯이 다시 할당되었다면 두 번째 해제는 새 소유자의 슬롯을 지우며 구분할 수 없다(주소만으로는 세대를 모른다).
- **버퍼 할당기(`GetSecretAllocator()`).** 평문이 지나가는 버퍼용 `Allocator`다. 키 저장소를 쓰지 않는다. 일반 힙에서 할당하고(최소 64 byte 정렬) 해제 때 요청 크기 전체를 지운다. 잠금·덤프 제외는 없다. 블록 크기는 블록 밖 표(주소 → 크기, Core `HashMap`)에 둔다. 그래서 버퍼 앞을 넘겨 쓰는 결함이 할당기 자료 구조를 망가뜨리지 않고, 이 할당기 것이 아닌 주소는 역참조하지 않고 `NotFound`다. `Array<uint8>`에 주면 성장 재할당 때 버려지는 이전 버퍼도 지워진다. 4 KiB를 넘는 `SecretBytes`도 여기에 있다.
- 두 저장소는 프로세스 끝까지 파괴하지 않는다(정적 객체 소멸 순서와 무관하게 마지막 해제까지 쓸 수 있게).
- 잠금과 덤프 제외는 `sodium_mlock`/`sodium_munlock`에 맡긴다. mlock·VirtualLock에 더해 `MADV_DONTDUMP`까지 이미 처리하므로, 직접 짜는 OS 호출을 줄인다. 잠금 한도가 작은 기기가 있으므로 arena는 작게 잡고, 한도에 걸린 arena는 잠그지 않은 채 쓴다. 상태는 `SecretMemoryStatus{arenaCount, arenaCapacityBytes, arenaLockedBytes, arenaUsedBytes, dumpExclusionRequested, bufferAllocations, bufferBytes}`로 보고한다(best-effort). libsodium은 `madvise` 결과를 확인하지 않으므로 덤프 제외는 "요청함"까지만 보고한다.
- guard page는 arena 양 끝의 넘침만 잡고, 같은 arena 안의 슬롯끼리 넘치는 것은 잡지 못한다. Asan 구성은 빈 슬롯과 요청 크기 뒤 꼬리를 poison해(Core `AsanPoisonMemory`) 해제 뒤 접근과 꼬리 넘침을 잡는다. guard page 접근 위반은 자식 프로세스 음성 대조(`secret-arena-guard-before`·`-after`)가 고정한다.
- 비밀 타입은 이동할 때 원본을 지운다. 레지스터, 라이브러리 내부 스택 잔재, 하이버네이트 파일은 막을 수 없다는 한계를 문서에 적는다.
- macOS에는 `MADV_DONTDUMP`에 해당하는 것이 없다. Windows minidump 제외는 crash reporter의 콜백 몫이다(후속, crash reporter가 아직 없다).
- 프로세스 메모리를 직접 읽는 공격자는 막지 못한다. 이 장치는 스왑·덤프·해제 뒤 잔존 데이터를 줄이는 것이 목적이다.

### 10.3 KeyRing과 공급자

```cpp
namespace rocks::crypto {

struct KeyId {   // Crypto/KeyId.h(KeyBlob이 C1에서 먼저 쓴다). 형식에는 RFC 9562 순서, Nil은 받지 않는다
    Guid value;
};

// KeyId → 키. TKey는 AeadKey, SegmentStreamKey 같은 키 타입이다. Id()를 품는 키(SegmentStreamKey)는
// 그 값이 등록 ID와 같아야 한다. 항목은 등록 뒤 바뀌지 않는다. 모든 함수는 여러 스레드가 동시에 불러도 된다.
template <typename TKey>
class KeyRing {
public:
    explicit KeyRing(Allocator& allocator = GetSecretAllocator()) noexcept;
    Result<void> TryAdd(KeyId id, TKey&& key) noexcept;           // 이미 있으면 AlreadyExists, 실패하면 key는 호출자에게 남는다
    Result<KeyLease<TKey>> TryAcquire(KeyId id) noexcept;         // 없으면 공급자를 부르고, 그래도 없으면 NotFound
    Result<KeyLease<TKey>> TryAcquireCurrent() noexcept;          // 현재 키(없으면 NotFound, 공급자를 부르지 않는다)
    Result<void> TryRemove(KeyId id) noexcept;                    // 마지막 lease가 끝나면 소거. 현재 키였으면 현재 키도 없앤다
    Result<void> TrySetCurrent(KeyId id) noexcept;                // 암호화에 쓸 현재 키. 등록된 키만(NotFound)
    void ClearCurrent() noexcept;
    KeyId CurrentId() const noexcept;
    usize Count() const noexcept;

    // 없는 키를 가져오는 곳이다(서버, OS 키 저장소, 빌드 시 주입 표). 같은 ID의 동시 요청은 한 번만
    // 부른다(single-flight). 잠금 밖에서 부르며, 공급자는 키를 돌려주고 KeyRing이 등록한다.
    void SetProvider(Delegate<Result<TKey>(KeyId)> provider) noexcept;
};

// KeyBlob 표 공급자(KeyId → Key 방식 blob). 요청 때 wrapKeyId로 KEK를 빌려 풀고 id를 비교한다.
template <KeyBlobKey TKey>
class KeyBlobProvider {
public:
    explicit KeyBlobProvider(KeyRing<KdfKey>& kekRing) noexcept;
    Result<void> TryAddBlob(Span<const uint8> blob) noexcept;     // blob을 복사해 둔다
    Result<void> TryRemoveBlob(KeyId id) noexcept;
    Result<TKey> TryFetch(KeyId id) noexcept;
    typename KeyRing<TKey>::Provider AsProvider() noexcept;       // SetProvider에 넘긴다(수명 추적 없음)
};

} // namespace rocks::crypto
```

- 공급자가 KeyBlob에서 키를 꺼낼 때는 TryPeekKeyBlob로 wrapKeyId를 읽어 KEK(`KeyRing<KdfKey>`)를 lease하고, `TryUnwrapKey<TKey>`가 돌려준 id가 요청한 id와 같은지 확인한다(다르면 AuthenticationFailed, 같은 KEK 아래 blob 통째 교체 방어).
- 형식은 키 ID를 기록하고, 복호는 ID로 키를 고른다. 키 회전은 "암호화는 현재 키, 복호는 등록된 키 전체(ID로)"다. 제거한 키라도 이미 받은 lease는 끝날 때까지 유효하다.
- KeyRing은 키 타입별 템플릿이다. 하나의 ID가 두 알고리즘에 쓰이는 일을 타입으로 막는다.
- **구현(C4, 2026-11-15).** `Crypto/KeyRing.h`(header-only 템플릿). 표는 잠금 하나로 지키는 `HashMap<Guid, SharedPtr<항목>>`이고 잠근 구간은 조회·삽입·lease 복사뿐이다. `KeyLease<TKey>`는 항목을 공유 소유하므로(복사 가능, 키 byte는 복사하지 않는다) 키를 제거하거나 KeyRing이 먼저 소멸해도 lease가 끝날 때까지 유효하고, 마지막 참조가 놓이는 스레드에서 키 소멸자가 지운다. 할당기는 마지막 lease보다 오래 살아야 하므로 기본값은 프로세스 끝까지 사는 `GetSecretAllocator()`다.
- **설계에서 바꾼 점.** `SetCurrent`는 등록된 키만 받는 `TrySetCurrent`로 바꾸고(Nil `InvalidArgument`, 없으면 `NotFound`) `TryAcquireCurrent`·`ClearCurrent`·`CurrentId`·`Count`를 더했다. "키 클래스는 Id를 품지 않는다"는 Id를 품는 `SegmentStreamKey`와 맞지 않아, Id를 품는 키는 `TryAdd`에서 id와 같아야 하고(`InvalidArgument`) 공급자 결과가 다르면 `AuthenticationFailed`다. 오류 순서는 빈 키 `StaleHandle` → Nil·Id 불일치 `InvalidArgument` → `AlreadyExists`·`NotFound` → `OutOfMemory`다.
- **공급자 계약.** 같은 ID의 동시 요청은 공급자를 한 번 부르고 성공·실패를 함께 받는다(늦게 온 요청은 표에서 찾거나, 실패였으면 다시 부른다. 실패는 기억하지 않는다). Core `Delegate`는 동시 실행을 허용하지 않으므로 KeyRing마다 공급자 호출은 한 번에 하나다(공급자 잠금, 잠금 순서 공급자 → 표). 표 잠금 밖에서 부르므로 공급자가 다른 KeyRing(KEK)이나 같은 KeyRing의 등록된 키를 빌릴 수 있다. 같은 KeyRing의 없는 키를 공급자 안에서 요청하면 교착 대신 `Busy`이고, 두 KeyRing의 공급자가 서로의 없는 키를 요청하는 순환은 만들지 않는다(KEK 계층처럼 한 방향). 공급자 안의 `SetProvider`와 공급자 호출 중 KeyRing 소멸은 Fatal이다. 공급자가 `NotFound`면 `NotFound`, 빈 키면 `StaleHandle`, 다른 오류는 그대로다. 둘이 같은 ID를 동시에 등록하면 먼저 등록된 항목이 남는다.
- **KeyBlob 표 공급자.** `KeyBlobProvider<TKey>`는 Key 방식 blob을 복사해 두고 요청 때 위 첫 항목 절차(Peek → KEK lease → `TryUnwrapKey<TKey>` → id 비교)로 키를 낸다. 넣을 때 형식·타입·길이를 보고(`AuthenticationFailed`), 비밀번호 방식은 `Unsupported`다. OS 키 저장소 공급자는 후속이다(1절).
- **Core 보강.** `Delegate`의 `CallIfAlive`(void 반환 전용 `ExecuteIfBound` 경로지만 모든 반환 타입에 만들어진다)가 반환값을 버려 `[[nodiscard]]` `Result` 반환 대상이 컴파일되지 않았다. 반환값을 명시적으로 버리게 고쳤다.
- 비밀번호에서 키를 얻을 때는 Argon2id(salt 16 byte)를 쓴다. 입력 매개변수는 상한으로 검사해 자원 고갈을 막는다. 구현(PasswordHash.h): TryDeriveKdfKeyFromPassword·TryDerivePasswordSecret, 저장 문자열 TryHashPassword·TryVerifyPassword·TryPasswordNeedsRehash. 비용 상한은 반복 32회·메모리 1 GiB·반복 × 메모리 4 GiB(SENSITIVE)이고 넘으면 계산하지 않고 `CapacityExceeded`다. "$<id>$" 꼴의 다른 형식과 libsodium 형식보다 긴 문자열은 `Unsupported`, 형식 오류·불일치는 `AuthenticationFailed`, Argon2 작업 메모리 부족은 `OutOfMemory`다(errno ENOMEM으로 불일치와 구분한다, 2026-11-15 리뷰 반영). libsodium은 작업 메모리를 해제 때 지우지 않는다(동작을 바꾸는 패치는 하지 않는다).

### 10.4 키 직렬화

키를 저장하거나 옮기는 형식은 하나로 정한다. 키를 그대로 내보내면 알고리즘·ID·매개변수를 잃기 때문이다.

KeyBlob v1(Crypto/KeyBlob.h, 2026-11-15 구현, 설계 교차검증 반영)의 배치는 다음과 같다(정수는 little-endian, ID는 RFC 9562 순서 BE64(high) ‖ BE64(low)).

| off | 길이 | 필드 | 규칙 |
|---|---|---|---|
| 0 | 4 | magic "RKEY" | 다르면 AuthenticationFailed |
| 4 | 1 | version = 1 | 모르면 Unsupported(magic과 이 byte만 보고 판정하며 뒤를 해석하지 않는다) |
| 5 | 1 | KeyBlobType | Aead 1·Mac 2·Kdf 3·Ed25519 4·Kx 5·Box 6·Kem 7·SecretStream 8·SegmentStream 9·ChannelServer 10(8·9는 C4, 10은 보안 채널에서 덧붙였다). 모르면 Unsupported |
| 6 | 1 | algorithm | 클래스가 품는 열거값(Aead·Mac·Kem), 없으면 0(Ed25519 서명 키는 Ed25519·ph 공용이라 0). 표 밖이면 Unsupported |
| 7 | 1 | method | 1 = Key(KdfKey KEK), 2 = Password(Argon2id v1.3, lane 1, salt 16, 출력 32). 모르면 Unsupported |
| 8 | 16 | keyId | Nil이면 AuthenticationFailed |
| 24 | 2 | L | (type, algorithm)의 허용 길이(AEAD 키 길이, HMAC 16–4096, BLAKE2b keyed 16–64, AES-CMAC 16·32, 32, KEM seed) |
| 26 | P | 방식 매개변수 | Key: wrapKeyId 16(Nil 거부). Password: ops LE32 ‖ memKiB LE32 ‖ salt 16(P = 24, 하한 위반은 AuthenticationFailed) |
| 26+P | 24 | nonce | OS 난수. AEAD nonce이면서 파생 transcript에도 들어간다 |
| 50+P | 32 | commitment C | AEAD 전에 상수 시간 비교 |
| 82+P | L+16 | 암호문 ‖ tag | XChaCha20-Poly1305, AD = T |

- 길이는 98 + P + L(Key 114 + L, Password 122 + L), 상한 `KeyBlobMaxBytes` = 4218이다. blob이 자기 길이를 담고, unwrap은 정확한 길이만 받는다.
- 파생: M = KEK(Key) 또는 TryDeriveKdfKeyFromPassword(pw, salt, {ops, memKiB × 1024})(Password), T = blob[0, 50 + P), B = TryDeriveBoundKdfKey(M, 0, labels::KeyBlob, {T}), K = TryDeriveAeadKey(B, 0, labels::KeyBlob, XChaCha20Poly1305), C = TryDeriveSecret(B, 1, labels::KeyBlob, 32). 다른 KEK·비밀번호와 헤더 변조는 AEAD 전에 C 비교에서 걸린다. C는 키에 대한 commitment(CMT-1)라 비밀번호 후보를 여러 개 시도하는 흐름(2절)에도 partitioning oracle이 없다. 키 wrap에는 메시지 commitment가 필요 없다((K, nonce, AD)가 정해지면 평문이 하나다).
- wrap 키를 AeadKey가 아니라 KdfKey로 받는다: 2절 "같은 키 byte를 다른 용도에 쓰지 않는다", 무작위 nonce XChaCha20만 쓰므로 이식성, commitment·blob별 파생에 PRF 키가 필요하다. 결정적 wrap(AES-KW·SIV)은 쓰지 않는다(HW 전용·자체 구성·같은 키 재wrap이 드러남). KEK 하나로 2^48개를 wrap해도 nonce 충돌은 약 2^-97이다.
- 담는 비밀: AEAD·MAC·KDF 키 byte, secretstream 키·세그먼트 스트림 마스터 키(32 byte, algorithm 0), Ed25519 seed(sk ‖ pk를 담지 않아 pk 불일치로 비밀키가 새는 double-public-key 문제를 구조적으로 피한다), X25519 비밀키(Kx·Box·ChannelServer), KEM seed(FIPS 203 형식). hazmat 짧은 MAC 키(16 byte 미만)는 wrap하지 않는다. 순번 봉인기 상태와 세션 비밀(kx 세션 키, KEM 공유 비밀)은 담지 않고, blob에서 푼 AeadKey로 순번 봉인기를 만들지 않는다(복원할 때마다 seq 0부터 같은 nonce).
- 비밀번호 방식: 비용은 명시 인자(도구·데스크톱 MODERATE, 모바일 INTERACTIVE 이상 권장), 기록은 KiB 단위(정규 인코딩), 하한 위반은 AuthenticationFailed, 상한(PasswordHash.h와 같은 검사, 내부 PasswordInternal.h 공유) 초과는 Argon2id 전에 CapacityExceeded다. 기기별 정책은 호출자가 TryPeekKeyBlob의 passwordCost로 먼저 정한다. 비밀번호 하나로 키 여러 개를 보관하면 비밀번호로 KdfKey blob 하나(KEK)를 만들고 나머지는 그 KEK 아래 Key 방식으로 둔다.
- API: TryPeekKeyBlob(인증 전 헤더, 헤더만 있으면 된다), 닫힌 타입 목록(`KeyBlobTraits`, 10개 타입)의 템플릿 TryGetKeyBlobBytes·TryWrapKey(wrapKey, wrapKeyId, key, keyId, blob)·TryWrapKeyWithPassword·`TryUnwrapKey<TKey>`·`TryUnwrapKeyWithPassword<TKey>`(결과 `UnwrappedKey<TKey>{id, key}`). 정의는 KeyBlob.cpp에서 10개 타입으로 명시 인스턴스화한다. `SegmentStreamKey`·`ChannelServerKey`는 Id를 품으므로 wrap의 keyId가 `key.Id()`와 다르면 `InvalidArgument`(쓰기 전)이고, unwrap하면 blob의 keyId를 Id로 가진 키가 나온다(헤더 keyId와 어긋날 수 없다).
- 오류(wrap): 빈 키 StaleHandle → 호출자 인자 InvalidArgument(Nil ID, 짧은 MAC 키, cost 범위, 빈 password, Argon2id 상한보다 긴 password, password·blob 겹침) → cost 상한·출력 부족 CapacityExceeded → 쓰기 시작 → 계산(DependencyFailed·OutOfMemory). 쓰기 시작한 뒤 실패하면 blob 전체를 지운다. TryGetKeyBlobBytes도 같은 비밀 길이 규칙을 쓴다(짧은 MAC 키는 InvalidArgument).
- 오류(unwrap): 빈 KEK StaleHandle → 긴 password InvalidArgument → 헤더(byte 순서대로 판정한다: magic·version 5 byte 미만이나 magic 불일치 AuthenticationFailed → 모르는 version Unsupported → 앞 26 byte 부족 AuthenticationFailed → 모르는 타입·(타입, 알고리즘)·방식 Unsupported → Nil keyId·L 범위 밖·방식 매개변수 길이 부족·Nil wrapKeyId·비용 하한 위반 AuthenticationFailed) → 길이·타입·방식이 기대와 다름 AuthenticationFailed → 비용 상한 CapacityExceeded(Password) → 이 기기에 없는 AEAD 알고리즘 Unsupported → 계산(OutOfMemory) → commitment·tag AuthenticationFailed. 공격자 입력에서는 InvalidArgument가 나오지 않는다.
- 한 번 읽기: Peek·unwrap은 blob을 지역 버퍼(Peek 50 byte, unwrap `KeyBlobMaxBytes`)로 한 번 복사하고 그 사본으로만 검사·파생·복호한다. blob이 공유 메모리나 쓰기 가능한 파일 매핑이라 호출 중 바뀌어도 헤더를 두 번 읽어 생기는 틈(첫 읽기에서 다른 타입으로 보이고 파생 때 원래 헤더로 돌아오면 KdfKey byte가 AeadKey로 나온다)이 없다. Peek와 unwrap 사이의 변경은 호출자 몫이다(같은 자기 버퍼를 넘긴다).
- 막지 않는 것: 같은 KEK 아래 롤백(비밀번호 변경은 키 회전이 아니다)·통째 교체·삭제(저장소 몫, 통째 교체는 공급자가 id 비교로 막는다), 출처, 메타데이터 기밀성. 생성 시각·용도 flag·만료와 공개키 동봉은 미룬다(필요하면 v2의 인증되는 영역). 수신자 공개키로 wrap하는 method(X-Wing 공유 비밀을 M으로)는 후속이다.
- 평문 키 내보내기(`hazmat::TryExportKey`, 키 타입마다 overload)는 hazmat이고, 시험·상호운용 도구에서만 쓴다. 정본이 seed인 키(Ed25519 서명 키, KEM 키 쌍)는 `hazmat::TryExportSeed`로 seed를 내보낸다.

## 11. 오류·한도·스레드

| 상황 | ErrorCode | 출력·상태 |
|---|---|---|
| 복호·검증 경로의 신뢰할 수 없는 입력: tag·MAC·서명·commitment 불일치, 절단·순서 위반, 잘못된 키, 헤더 형식 위반, 기대한 타입·방식과 다른 blob(KeyBlob). 상대에게서 받은 공개키(kx 상대, 봉인·캡슐화 수신자)가 올바른 점이 아닌 경우도 같다(봉인 쪽은 libsodium이 쓴 자리만 지운다) | `AuthenticationFailed` [신설] | 출력 전체 소거, 스트림 sticky |
| 호출자의 API 오용: 키·nonce·context·출력 버퍼 길이 오류, 함수 규칙 밖의 겹침, 잘못된 단계 호출 | `InvalidArgument` | 입력·출력·상태 불변 |
| 출력 Span 부족 | `CapacityExceeded` | 상태 불변(메시지형), 역압(스트림형) |
| hex·base64 디코딩: 길이·끝 `'='`만으로 알 수 있는 형식 오류 / 디코딩 중에 찾은 형식 오류(알파벳·끝 bit·중간 패딩) | `InvalidArgument` | 앞은 출력 불변, 뒤는 출력 전체 소거. 크기 검사가 내용 검사보다 먼저다 |
| `TryFillRandom`의 OS 난수 실패 | `DependencyFailed` | 출력 전체 소거 |
| (key, nonce)당 상한 초과, nonce 카운터 소진, 세그먼트 수 2^32 초과 | `CapacityExceeded` | 그 키·스트림으로는 더 봉인하지 않음 |
| HW 명령이 없는 기기의 AES·AEGIS, 무작위 nonce가 안전하지 않은 알고리즘의 고수준 Seal, 96 bit가 아닌 GCM nonce, 헤더의 모르는 버전·알고리즘 | `Unsupported` | 입력·출력·상태 불변 |
| 키 저장소 한도(arena 64개, 4 MiB) 소진·OS 예약 실패, 버퍼 할당기의 힙 부족 | `OutOfMemory` | 상태 불변 |
| 크기 계산 overflow | `Overflow` | — |
| libsodium 초기화 실패·Crypto 밖 초기화, libsodium 내부 난수 경로의 OS 난수 실패(`randombytes_buf`는 실패를 돌려줄 수 없다), misuse handler | Fatal | 암호 기능 없이 계속하지 않음 |

`AuthenticationFailed`를 새로 두는 이유가 있다. 변조·잘못된 키와 API 오용(`InvalidArgument`)을 호출자가 구분해야 하기 때문이다. 다만 어느 검사가 실패했는지(tag인지 길이 표시인지)는 오류에 담지 않는다(결정 D5).

| 알고리즘 | key | nonce | tag | (key, nonce)당 평문 상한 |
|---|---|---|---|---|
| XChaCha20-Poly1305 | 32 | 24 | 16 | SIZE_MAX − 16(사실상 무제한) |
| ChaCha20-Poly1305-IETF | 32 | 12 | 16 | 64 × (2^32 − 1) ≈ 256 GiB |
| AES-256-GCM / AES-128-GCM | 32 / 16 | 12 | 16 | 2^36 − 32 B ≈ 64 GiB(NIST). libsodium AES-256-GCM은 구현이 한 블록 적게 받아 16 × (2^32 − 3) byte를 쓴다(헤더 상수 16 × (2^32 − 2)) |
| AEGIS-256 / AEGIS-128L | 32 / 16 | 32 / 16 | 32 | 2^61 − 1. 무작위 nonce는 256이 사실상 무제한, 128L은 2^48개 |
| 순번 봉인기(키당) | — | — | — | seq 2^64 − 1개. AES-GCM은 키당 2^36 − 1 블록(약 1 TiB): draft-irtf-cfrg-aead-limits-13 6.2절의 기밀성 이점 (s + q + 1)^2 / 2^129를 2^-57 아래로 둔다(QUIC RFC 9001 B.1.1의 기밀성 목표와 같은 값. TLS 1.3 레코드 한도는 이 식으로 약 2^-60). ChaCha20-Poly1305·AEGIS는 nonce를 지키면 키당 데이터 한도가 없다(aead-limits-13 6.3절, RFC 10032 10절) |
| secretstream | 32 | header 24 | 17 | 메시지당 ≈ 256 GiB, 스트림은 무제한 |
| 세그먼트 스트림 | 32 | header 88 | 16/세그먼트 | 2^32 세그먼트(64 KiB 세그먼트면 약 256 TiB). AES-256-GCM은 세그먼트 암호문 합 2^40 − 16 B(평문 2^40 − 2^44/S − 16 B) |

키당 데이터 한도의 목표는 기밀성 이점 2^-57(키마다)이다. 키가 여러 개면 대략 키 수만큼 더해진다(TLS·QUIC도 연결당 목표다). 9.5절 AES 세그먼트 스트림은 C3(2026-11-15)에서 같은 목표로 다시 정해 "세그먼트 암호문 합(tag 포함) ≤ 2^40 − 16 B"(블록 2^36 − 1개)로 확정했다. 처음 설계의 평문 2^48 B(약 2^-41)를 대신한다.

- **스레드.** 자유 함수는 재진입 가능하다. 키 객체는 생성 뒤 불변이라 여러 스레드가 동시에 읽어도 된다. 연산 상태는 호출마다(일회성) 또는 스트림 객체마다 따로 있다. AES 라운드 키와 H 거듭제곱은 키를 만들 때 한 번 계산해 키 객체에 두고, 이후 바꾸지 않는다(5.3절). KeyRing은 모든 함수가 스레드 안전하고 공급자 호출은 KeyRing마다 한 번에 하나다(10.3절). `SegmentCryptoTask`는 제출·조회·소멸을 한 스레드에서 하고, 그 작업들은 worker에서 같은 writer·임의 접근 객체를 함께 쓴다(9.7절).
- **결정성.** 고수준 API는 무작위 nonce·salt를 쓰므로 출력이 실행마다 다르다. 시험은 hazmat의 nonce 지정 API와, 고정 salt·`CpuFeatures`를 값으로 받는 모듈 내부 함수로 재현한다. 모듈 오브젝트는 시험 실행 파일과 제품이 같이 쓰므로(모듈을 시험용으로 따로 컴파일하지 않고, `ROCKS_TESTING`은 시험 실행 파일 소스에만 정의된다) "시험 빌드에서만 컴파일되는 hook"은 만들 수 없다. 그래서 전역 hook(바꿔 둘 수 있는 함수 포인터·고정 salt 플래그)을 두지 않는다. 바꿔 둔 상태가 없으므로 출하 코드가 고정 salt로 (키, nonce)를 재사용할 길도 없다. 공개 API는 늘 OS 난수·감지 결과를 넘긴다. 시험이 닿는 길은 둘로 나눈다.
  - 값을 받는 내부 함수(고정 salt·`CpuFeatures`·세그먼트 경계 probe 등)는 모듈 구현과 시험이 함께 include하는 일반 Private 내부 헤더(예: `KdfInternal.h`, `AsyncCryptoInternal.h`)에 선언한다. 정의하는 .cpp가 같은 선언을 봐야 선언·정의가 어긋나지 않는다.
  - 공개 계약이 막은 상태 이동(순번 이어 가기 등)은 모듈 Private/의 header-only friend 접근점 `*TestAccess.h`로만 한다.
  - "바꿔 둘 수 있는 전역 hook을 두지 않는다"는 설계·리뷰 규칙이다. 정책 검사(P7, `PolicyChecks.TestAccess`)는 그 가운데 lexical하게 볼 수 있는 부분을 확인한다.
  - `test-access-include`: 시험 소스(모듈 `Tests/`·`Programs/FoundationTests/`) 밖의 소스(모듈 구현·모듈 `Private/Tests/`·다른 모듈·시험 지원 코드)는 `*TestAccess.h`를 include하지 않는다(`#include_next`·`#import` 포함, 접근점끼리는 예외). 모듈 `Private/Tests/`는 모듈 소스로 컴파일되어 출하되므로 시험 소스가 아니다.
  - `test-access-location`: `*TestAccess.h`는 모듈 Private/에만 둔다(공개 include 경로와 그 안의 Private/에 두지 않는다).
  - `testing-macro`: `ROCKS_TESTING`은 시험 소스와 TestSupport에서만 쓴다. 모듈 .cpp에서는 늘 정의되지 않으므로 그 매크로로 감싼 hook은 시험에서도 꺼지고 반대 분기는 출하된다. 모듈 헤더에서는 시험 번역 단위만 정의되므로 클래스 배치·inline 정의가 번역 단위마다 달라진다(ODR 위반).
  - `global-hook`: libsodium 전역 설정 함수(`randombytes_set_implementation`·`sodium_set_misuse_handler`)는 Crypto 초기화(`Private/Sodium/SodiumRuntime.cpp`)만 부른다. 시험이 난수 구현을 바꿔 두면 다른 시험·스레드의 키·nonce가 결정적이 된다.
  - lexical 검사의 한계: 매크로로 만든 include 경로, 공개 헤더가 friend로 선언한 접근점 이름을 제품 코드가 직접 정의하는 형태, 일반 내부 헤더에 둔 가변 전역은 잡지 않는다(리뷰가 맡는다).

## 12. 검증 계획

- **알려진 답.** Wycheproof 사상표에 따른 전 항목(96 bit가 아닌 GCM IV·짧은 tag는 `Unsupported`·`AuthenticationFailed`, Ed25519 "acceptable" 항목은 libsodium 결과를 기대값으로 둔다: 비정규 encoding·small-order는 거부, 혼합 차수 키는 cofactorless 식이 맞으면 수용). C1 사상표: AEAD는 valid가 봉인 byte와 열기 결과 일치, invalid는 AuthenticationFailed이고 평문 출력을 0으로 지운다. AES-GCM은 128·256 bit 키를 검증하고(128 bit는 C2부터, 5.4절) 192 bit 키는 열거값이 없어 센다. AES HW가 없는 기기에서는 128·256 bit 키 213건을 Unsupported로 센다(HW 판정이 조용히 꺼지는 회귀는 CPU 일치 시험이 잡는다). AES-CMAC은 5.4절 사상이다. 들인 데이터에는 짧은 tag 사례가 없어(AES-GCM은 모두 16 byte, ChaCha 계열 0 byte tag는 nonce 길이 오류가 먼저다) 짧은 tag 거부는 AeadTests의 잘린 입력·tag 시험이 맡는다. ChaCha 계열의 nonce 길이 오류는 InvalidArgument다. HMAC의 잘린 tag는 전체 tag 앞부분과 비교하고, 검증 함수가 잘린 tag를 AuthenticationFailed로 거부하는지도 본다. HKDF 출력 상한 초과는 InvalidArgument다. X25519는 crypto_kx 클라이언트·서버 양쪽으로 돌려 공유 비밀 0(small-order 점 31건)을 AuthenticationFailed로, 나머지(twist 점·비정규 u 포함)를 BLAKE2b-512(q ‖ client_pk ‖ server_pk)와 비교한다. ML-KEM-768은 seed 길이 오류 InvalidArgument, 암호문 길이 오류 AuthenticationFailed, 그 밖은 공개키와 공유 비밀(암묵적 거부 포함) 일치다. RFC 8439(ChaCha20-Poly1305), XChaCha20 초안 벡터, RFC 8032(Ed25519·Ed25519ph), RFC 7748(X25519), RFC 5869(HKDF), RFC 4231(HMAC), RFC 7693(BLAKE2b), RFC 4493(CMAC), NIST SP 800-38A(CTR)·CAVP GCM, X-Wing·ML-KEM768 표준 벡터, 세그먼트 형식 golden(salt 고정, [형식 명세](CryptoSegmentFormat.ko.md) 6절 V1–V3, 독립 Python 구현과 일치), secretstream Wycheproof 상태 벡터·PyNaCl 상호운용.
- **차등.** 같은 기기에서 libsodium AES-256-GCM(원본)과 경량 계층 AES-256-GCM을 무작위 입력(길이 0–64 KiB, AD 0–1 KiB, 정렬·비정렬, 제자리)과 무작위 조각 나누기(멀티파트)로 대조한다. 원본이 파생 코드의 기준이다. x64와 arm64 결과는 같은 벡터 파일로 고정한다.
- **스칼라 참조.** 시험 전용 스칼라 참조(블록 암호화 + 32·64·128 bit 카운터 증가)로 CTR·CMAC·AES-128을 대조한다. 시작 카운터를 2^32−k, 2^64−k, 2^128−k에 두어 carry 전파와 wrap(`CapacityExceeded`)을 확인한다. libsodium 원본에 없는 경로라 원본이 기준이 되지 못하기 때문이다.
- **경로 범위.** GitHub CI(Windows x64 AES-NI·AVX, macOS arm64 AES·PMULL)와 로컬 Android x86_64 에뮬레이터(adb 실행)에서 돈다. 행마다 `AeadInfo.implementation`이 기대값과 같은지 단언한다. HW 없는 경로는 `CpuFeatures`를 값으로 받는 내부 판정 함수(`detail::HasAesHardware`, `SelectAesKernels`·`CheckAesKernels`)로 `Unsupported` 반환과 자가 시험 거부를 확인한다(11절 결정성. C2에서 값을 받는 함수와 감지 결과로 부르는 공개 경로로 나눴다). Android arm64 실기기는 아직 실행 경로가 없으므로 결과에서 구분해 적는다.
- **일치 시험.** 라이브러리 CPU 감지 = `CpuFeatures`(6.2절).
- **음성 시험.** misuse handler 도달 조건(secretstream 최대 길이 초과 등)이 Fatal 없이 오류로 돌아오는지, 제자리 규칙 밖 겹침의 쓰기 전 거부, tag의 bit 하나 뒤집기, 잘린 tag, 모든 세그먼트 경계와 중간에서의 절단, 세그먼트 교환·중복·삭제, 다른 스트림 세그먼트 이식, AD·헤더 bit flip(commitment 거부), last 표시 위조, 잘못된 키, Final 뒤 입력, 병렬 writer 재봉인, 크기 0·1·S−17·S−16·S−15·2(S−16), 1 byte 출력 창 역압, 최대 길이 경계(상한 −1·상한·상한 +1을 상태 조작으로 확인). C3(2026-11-15): `SecretStreamTests.cpp`·`SegmentStreamTests.cpp`·`SegmentStreamNegativeTests.cpp`가 위 항목을 모두 덮는다. secretstream 상한은 길이만 큰 Span(내용은 읽히지 않는다)으로, 세그먼트 2^32개·AES 2^40 − 16 B 경계는 `SegmentStreamTestAccess`로 위치를 옮겨 확인한다. 병렬 writer는 네 스레드가 같은 index 64개를 다투어 index마다 한 번만 봉인되는지 본다.
- **소거·수명.** 소멸 뒤 슬롯이 0인지, 실패 경로에서 출력이 0인지, 이동 뒤 `StaleHandle`인지 확인한다. ASan 구성(libsodium 포함 여부는 결정 D9).
- **비동기·KeyRing(C4, 2026-11-15).** `AsyncCryptoTests.cpp`: 고정 salt 비동기 암호문 = 동기 순차 암호기(ChaCha20-Poly1305, HW가 있으면 AES-256-GCM, 세그먼트 경계 크기와 `--seed` 무작위 크기, 작업 수 1·3·8·기본), 비동기 복호 왕복, 공개 경로(OS 난수 salt) → 동기 임의 접근. 변조·다른 키 ID·같은 ID 다른 키·AD·절단·짧은 입력·버전·작은 출력에서 동기 경로와 같은 오류이고 출력 전체가 0. 제출 오류가 task·출력을 바꾸지 않음, 진행 중 `Busy`. 취소: 시작 전(의존 대기)·의존 실패·진행 중(probe 확인 횟수, 실제 `RequestCancel`) 모두 출력 0. 수명: 제출 뒤 키 소멸·KeyRing 제거·lease 반환, 의존 작업이 암호문을 채운 뒤 복호, 이어 붙인 작업은 성공일 때만 실행, 진행 중 task 소멸. `KeyRingTests.cpp`: 오류 순서, 제거·KeyRing 소멸 뒤 lease, 마지막 lease에서 키 저장소 반환, 공급자 성공·실패·빈 키·ID 불일치·재진입 `Busy`, single-flight(스레드 8개, 성공 한 번 호출), 스레드 8개 등록·조회·제거 혼합(`--seed`), KeyBlob 표 공급자(KEK 없음·변조 blob·타입·방식). `KeyBlobTests.cpp`: 타입 8·9 왕복·Peek·Id 불일치 거부(보안 채널에서 타입 10도 같은 시험과 kx·box 키로 열기 거부를 더했다).
- **fuzz.** 세그먼트 header·배치 계산·임의 접근 범위·세그먼트 복호(순차·임의 접근)·secretstream reader·보안 채널 handshake 메시지 읽기·datagram 열기·Wycheproof JSON 파서. 저장소에 coverage 기반 fuzzer(libFuzzer) 빌드 기반이 없어(모듈 규칙이 sanitizer 인자를 받지 않는다), 고정 seed 변형 fuzz를 시험 묶음에 둔다(`Tests/CryptoFuzzTests.cpp`, 2026-11-15). bit 뒤집기·byte 바꾸기·자르기·늘리기·구간 복제로 입력을 변형하고, 정해진 오류로만 거부되는지(Fatal·InvalidArgument 없음), 실패 출력이 지워지는지, 배치·범위 계산이 형식 명세의 식과 같은지 본다. Asan 구성에서도 돈다. libFuzzer 하네스는 빌드 기반이 생기면 같은 대상으로 옮긴다.
- **성능.** 6.4절 gate.

## 13. 선행 작업과 단계

| 코드 | 내용 | 위치 |
|---|---|---|
| P1 | Span 보강(IsEmpty·Slice·Left·LeftChop·Right·RightChop·TrySlice·AsBytes·AsWritableBytes·Overlaps) | Core |
| P2 | `StreamProgress`를 Core로 옮기고 Compression이 그 타입을 쓰게 바꾼다 | Core, Compression |
| P3 | `ErrorCode::AuthenticationFailed` | Core |
| P4 | `CpuFeatures` 확장(6.2절). Rockpile R0과 공유한다 | PlatformBase |
| P5 | `SecureMemory`(guard page 예약·잠금 연결 지점). 구현 결과 기존 `VirtualMemoryRegion`과 `sodium_mlock`으로 충족해 새 API를 두지 않았다(10.2절) | PlatformBase |
| P6 | ThirdParty Sodium·Wycheproof manifest, 컴파일 인자 허용 목록, target pragma 기록, 라이선스 고지 | Build, Tools |
| P7 | 함수 단위 target 매크로(`ROCKS_TARGET_X64_AES`·`ROCKS_TARGET_ARM64_AES`)와, 이 매크로·AES·carry-less·SHA intrinsics를 디렉터리 허용 목록에서만 쓰게 하는 정책 검사. 시험 hook의 출하 빌드 제외 검사와 모듈 내부 시험 접근점(`Private/AeadSequenceTestAccess.h` 등)을 Tests 밖에서 include하지 않는지의 검사. 구현 결과(2026-11-15): 모듈 오브젝트가 시험·출하 공용이라 hook 제외는 "전역 hook을 두지 않고 값을 받는 내부 함수와 `*TestAccess.h` 접근점으로 시험한다"로 바꾸고 `ROCKS_TESTING`·libsodium 전역 설정 사용처를 검사한다. ARM64 매크로는 감지 조건과 같게 `crypto`를 뺐고, 정책의 시험 소스 판정을 시험 실행 파일 구성과 같게 좁혔다(모듈 `Private/Tests/`는 시험 소스가 아니다). 모듈 규칙의 `ROCKS_TARGET_*` 정의를 빌드 입력 검증이 거부한다(5.3·11절) | Base, Tools, Build |
| P8 | libsodium 빌드 정의의 경고 정책 승인(Compression 3.4·3.6절 선례), 대상별 configure 결과 고정, 파생 파일 출처 SHA-256 기록. 파생 기록은 2026-11-15에 일반 검사로 구현했다(`Derivation.json`, `PolicyChecks.DerivedSources`, 규칙 `derived-source`, 5.2절) | Docs, Tools |

| 단계 | 내용 | 완료 조건 |
|---|---|---|
| C1 | 기반과 libsodium 일회성: SecretArena, 키 타입·키 직렬화, label 등록부, 난수·상수 시간(hex·base64 포함), macOS arm64 DIT, 해시·MAC·KDF·Argon2id, AEAD(ChaCha·XChaCha·AEGIS·AES-256-GCM HW), Ed25519, kx, sealed box, X-Wing·ML-KEM768 | Wycheproof·RFC 통과, CPU 일치 시험, 3개 플랫폼 경로 단언 |
| C2 | AES 경량 계층: x64·arm64 커널, GCM(AES-128/256, 일회성·멀티파트), CTR, 이어서 CMAC | libsodium 대비 차등 시험, Wycheproof·NIST, 성능 gate(결정 D4 확정). 2026-11-15 구현: AES-128-GCM 공개, `Crypto/AesModes.h`(AesKey·AesCtr·GCM 멀티파트), AES-CMAC, Wycheproof aes_cmac, `Programs/CryptoBench`로 D4 확정(6.4절). Windows x64 Debug 시험 통과, arm64는 NDK·macOS 대상 컴파일만 확인(실행은 macOS arm64 CI) |
| C3 | 스트리밍: 증분 객체, secretstream, 세그먼트 형식(순차·임의 접근·병렬 writer), 형식 명세 문서 | golden·음성 시험 전부, 형식 명세와 golden 일치. 2026-11-15 구현: `Crypto/SecretStream.h`·`Crypto/SegmentStream.h`, [형식 명세](CryptoSegmentFormat.ko.md)(벡터를 독립 Python 구현으로 대조), Wycheproof secretstream, AES 세그먼트 상한 2^40 − 16 B 확정(9.5·11절). Windows x64 Debug 시험 통과 |
| C4 | KeyRing·공급자, AsyncCrypto | 동기·비동기 결과 byte 일치, 취소·수명 시험. 2026-11-15 구현: `Crypto/KeyRing.h`(KeyRing·KeyLease·KeyBlobProvider, 10.3절), `Crypto/AsyncCrypto.h`(9.7절), KeyBlob 타입 SecretStream 8·SegmentStream 9(10.4절), Core `Delegate` 보강. Windows x64 Debug 시험 통과(동시성 시험 여러 seed 반복) |
| 후속 | Crypto 단독 후속은 두지 않는다(2026-11-15 사용자 판단: 게임 엔진에는 과하다). 전송 채널 보호는 [SecureChannel](SecureChannel.ko.md)(채택: Crypto에 wire 형식 없는 Noise NK handshake·명시 counter datagram 봉인·열기·서버 키 타입, 패킷 형식·DoS는 NetCore), 서명된 청크 표는 [ContentSigning](ContentSigning.ko.md)(채택: 서명된 쿠킹 매니페스트 봉투, Crypto 추가 없음, AssetRuntime A1과 함께 구현). OS 키 저장소·AES-CBC·CCM·KW·P-256·RSA 검증은 구체적 요구가 생길 때만 더한다 | SecureChannel·ContentSigning |

착수 시점은 기존 작업 순서 안에서 사용자가 정한다(결정 D10). 이 문서는 스펙 반영 전 설계안이다.

## 14. 결정

결정은 설계안의 추천안을 채택해 착수한다(사용자가 이의를 제기하면 바꾼다). D10(착수 시점)은 2026-11-14 사용자 지시("별도 워크 트리에서 작업시작")로 정해졌다. libsodium 경고 처리는 2026-11-14 사용자가 "추적 가능한 수정"을 골랐다.

| 코드 | 질문 | 채택(2026-11-14, 추천안) | 대안 |
|---|---|---|---|
| D1 | AES 경량 계층의 출처 | libsodium aesni·armcrypto 구현에서 파생(ISC 고지 유지). 검증된 구조를 쓰고, 바꾸는 부분은 AES-128 키 확장·증분 상태·CTR·CMAC로 한정 | 처음부터 작성(Intel·Arm 공개 기법) / TF-PSA-Crypto에서 aes·aesni·aesce·gcm·cmac만 발췌(검증된 코드지만 1.x 비공개 내부라 업그레이드마다 맞춤 작업, Apache-2.0 의존 추가) |
| D2 | AES HW가 있을 때 `GetPreferredAead()` | XChaCha20-Poly1305 유지(모든 기기에서 같은 결과, 무작위 nonce 안전) | AEGIS-256(최고속, 무작위 nonce 안전) / AES-256-GCM(상호운용) |
| D3 | HW 없는 기기의 AEGIS | `Unsupported`(AES와 같은 규칙, 모든 경로 상수 시간) | libsodium soft 허용(부채널 보호 없음을 문서로 경고) |
| D4 | AES-256-GCM 일회성·세그먼트 경로 | libsodium 원본(검증된 코드, 경량 계층의 차등 기준). 2026-11-15 확정: 같은 기기에서 경량 계층이 64 KiB 이상 약 0.9배라 동등하지 않다(6.4절) | 경량 계층으로 단일화(경로 하나, 측정 결과가 같으면) |
| D5 | 인증 실패 오류 | `ErrorCode::AuthenticationFailed` 신설(이름 후보 `VerificationFailed`) | `InvalidArgument` 재사용(Compression과 같은 분류) |
| D6 | 세그먼트 크기 기본값 | 64 KiB(임의 접근 단위와 병렬 분할의 균형). 프리셋 4 KiB·64 KiB·1 MiB | 1 MiB(순차 처리량 우선) |
| D7 | SHA-256 공개 API | Core `Sha256` 하나만 둔다. libsodium SHA-256은 HMAC·HKDF 내부에서만 쓴다 | Crypto에도 SHA-256 공개(중복) |
| D8 | 고정 길이 인자 | 값 타입 + `View()`, 입력은 Span 길이 검사 | 정적 extent Span(`Span<const uint8, 32>`) 도입 |
| D9 | sanitizer 구성의 libsodium | libsodium 번역 단위는 sanitizer 계측에서 뺀다(설치 문서가 sanitizer 전체를 금지하고 upstream CI에도 sanitizer 작업이 없다). 어댑터·경량 계층·형식 코드는 ASan으로 돈다. 구현: manifest `sanitizerInstrumentation: false` → 모듈의 Asan 계측 인자만 뺀다(2026-11-14) | 포함해서 돌리고 결과를 기록 |
| D10 | 착수 시점 | 2026-11-14 착수(별도 워크트리, 브랜치 `feature/crypto`) |  |
| D11 | 세그먼트 형식 | Rocks 헤더(알고리즘·크기·키 ID·commitment를 키 파생에 결합) + STREAM 세그먼트 | Tink AES-GCM-HKDF wire 호환(commitment 없음, AES-GCM 고정, HW 없는 기기에서 못 읽음) |
| D12 | ARM DIT 적용 시점 | C1에 포함(macOS arm64, FEAT_DIT가 있을 때 암호 구간에서 켬) | 후속 검토(libsodium 로드맵을 기다림) |
| D13 | libsodium 고정 대상 | 1.0.22-stable commit(XGETBV 감지 정의·misuse handler 잠금 수정 포함, 수정만 들어가는 브랜치) | point release 1.0.22 + 빌드 도구의 `.S` 지원과 `HAVE_AVX_ASM`(Windows는 별도 경로) |

## 15. 교차검증 기록

2026-11-14에 세 갈래로 병렬 검증했다. 첫째는 초안을 모르는 독립 설계, 둘째는 초안에 대한 반례 검토, 셋째는 공개 자료 사실 확인이다(libsodium 1.0.22 tarball·태그·stable 원본, 저장소 원본). 사용자 지시에 따른 방향 변경(UE 비교 제외, 외부 AES 라이브러리 대신 HW 전용 경량 계층, 큰 외부 라이브러리 제외)은 검증 전에 반영했다. 차단 3건, 중요 18건을 모두 조치했다.

| 출처 | 등급 | 지적 | 조치 |
|---|---|---|---|
| 사실 | [차단] | 1.0.22에는 `HAVE_XGETBV_ASM`이 없어 asm 없이 빌드하면 Windows 밖 x86-64에서 AVX 감지가 실패하고 AES-GCM·AEGIS가 꺼진다 | 1.0.22-stable commit 고정(5.1, D13) |
| 반례 | [차단] | 세그먼트 복호기 `TryFinish`에 출력이 없어 마지막 세그먼트 평문을 받을 수 없다 | `TryFinish(Span) → StreamProgress`, 시도 없는 last 판정(9.5) |
| 반례 | [차단] | "같은 주소면 제자리 허용"이 nonce‖암호문‖tag 형식과 모순되고, 사전 거부 때도 평문을 지운다 | 함수별 제자리 규칙(7), 쓰기 시작 뒤에만 소거(2·11) |
| 독립 | [중요] | Tink 형식에는 key commitment·키 ID·매개변수 결합이 없다 | 자체 헤더 + STREAM + commitment(9.5, D11) |
| 독립 | [중요] | 세그먼트 인증과 스트림 완결은 다르다. 빈 평문·빈 last의 정규 인코딩이 필요하다 | `TryFinish` 확정 규칙, 정규 인코딩·크기 산술(9.5) |
| 독립·반례 | [중요] | 병렬 writer가 같은 index를 동시에 봉인할 수 있다 | 길이 검사 → 원자적 claim → 봉인, AlreadyExists(9.5) |
| 반례 | [중요] | 오류 분류가 절마다 다르고, 짧은 tag 위조가 가능하다 | 출처별 분류, tag 길이 정확히 일치(2·11) |
| 반례 | [중요] | 키 ID로 키를 찾으려면 헤더를 읽어야 하는데 수단이 없고, KeyRing 타입·공급자 경합이 정해지지 않았다 | `TryPeekSegmentHeader`, `KeyRing<TKey>`, single-flight(9.5·10.3) |
| 반례 | [중요] | 큰 버퍼와 키 없는 상태까지 작은 잠금 arena에 둔다 | 키 의존 상태만 arena, 버퍼는 Allocator(기본 `GetSecretAllocator`), `OutOfMemory`(9.1) |
| 반례 | [중요] | CTR carry·CMAC·AES-128은 libsodium 원본이 기준이 되지 못한다 | 시험 전용 스칼라 참조, 경계 카운터 시험(5.2·12) |
| 반례·사실 | [중요] | Android arm64에서 HW 경로가 조용히 꺼질 수 있고, libsodium ARM 판정은 AES bit만 본다 | 실행 시점 판정(`CpuFeatures` ∧ `sodium_runtime_has_*`), 경량 계층 자가 시험(5.3·6.1·6.2) |
| 반례 | [중요] | RNG 구조체 NULL 호출, init 재귀, 대기 없는 once, 원격 misuse Fatal, 실패 출력 `0xd0` | 5.1 초기화·난수·misuse·복호 실패 출력 규칙 |
| 반례 | [중요] | secretstream Reader에 Rekey가 없고, Final 뒤 Push를 막지 않으며, tag byte 매핑이 없다 | 9.4 계약 보강 |
| 반례 | [중요] | `arm_neon.h` 기존 사용·SHA HW 경로와 정책이 충돌하고, 비공식 정의 승인·`HAVE_TI_MODE`·파생 파일 추적이 빠졌다 | 디렉터리 허용 목록(5.3, P7), 경고 정책 승인·configure 결과 고정(5.1, P8), `HAVE_TI_MODE`, 원본 SHA-256 기록 |
| 사실 | [중요] | 설치 문서는 sanitizer 전체를 금지하고 upstream CI에는 sanitizer 작업이 없다 | D9 추천을 "libsodium 번역 단위 계측 제외"로 바꿈 |
| 반례 | 개선 | "모든 경로가 상수 시간"은 과장이다(Argon2id, Apple M1·M2 prefetcher) | 2절 문구 수정, DIT를 C1로(6.3, D12) |
| 반례 | 개선 | AES 스트림 생일 한계, flags·Guid 순서·범위 산술 | AES 스트림 2^48 byte 상한, 형식 규칙(9.5) |
| 반례·독립 | 개선 | 키 직렬화, 타입 KDF, 상수 시간 hex·base64, label 등록부, 시험 hook 출하 제외, 키를 소유하는 순번 봉인기, 비밀 Array 재할당 잔재, 배포 키의 비밀성 | 10.4, 10.1, 8, 11, 10.2, 2절 |
| 사실 | 정정 | GHASH는 Karatsuba가 아니라 교과서식, ARM 병렬은 6블록, HMAC 일회성 키 32 byte 고정, ipcrypt의 AES, Span 반복자·`Array::View`, StringView 이름, zstd target 속성, CI 구성, version.h 복사 방식, zig 인자, `ED25519_COMPAT` | 각 절 정정 |
| 사실 | 확인 | libsodium 상수·한도·상태 크기, KEM·SHA-3·XOF 추가 버전, secretstream 규칙, Argon2id 프리셋, CPU 조건, common.h 동작, 저장소 사실 등 약 30건 | 변경 없음 |

반례 검토에서 "공개 필드 검사는 oracle이 아니다"는 지적은 받아들여, 모르는 버전·알고리즘만 `Unsupported`로 구분했다. 나머지 형식 위반은 `AuthenticationFailed`로 둔다.

## 16. 출처

- libsodium 문서: [doc.libsodium.org](https://doc.libsodium.org/). AEAD 개요·AES-256-GCM·AEGIS-256·secretstream·HKDF·HMAC-SHA-2·SHA-3·XOF·custom RNG·helpers·memory management·installation·internals·roadmap 항목.
- libsodium 원본: [github.com/jedisct1/libsodium](https://github.com/jedisct1/libsodium). stable 브랜치 `ChangeLog`, `src/libsodium/include/sodium/private/common.h`, `sodium/runtime.c`, `sodium/core.c`, `crypto_aead/aes256gcm/*`, `crypto_aead/aegis256/*`, `build.zig`, `builds/msvc/*`. 이슈 #1090, #648. 릴리스 1.0.22(2026-04-09) tarball·`1.0.22-RELEASE` 태그와 stable 브랜치를 대조했다(`runtime.c`의 XGETBV 분기, `build.zig`).
- libsodium AES-GCM 원본(경량 계층의 출처): `src/libsodium/crypto_aead/aes256gcm/aesni/aead_aes256gcm_aesni.c`(1,020줄), `.../armcrypto/aead_aes256gcm_armcrypto.c`(1,072줄, stable. point release 1.0.22는 1,071줄), `crypto_ipcrypt/*`(AES-128 키 확장). ChangeLog 1.0.22(ML-KEM768·X-Wing·SHA-3).
- 검토 후 제외한 AES 라이브러리(5.2절): TF-PSA-Crypto 1.1.1·1.2.0, Mbed TLS 3.6.7, AWS-LC 5.14.0, BoringSSL, OpenSSL 4.1, SymCrypt 103.13, BearSSL 0.6. 각 저장소의 README·BUILDING·BRANCHES·SECURITY·migration guide.
- Tink Streaming AEAD(세그먼트 구성 비교): [developers.google.com/tink/streaming-aead](https://developers.google.com/tink/streaming-aead). STREAM: Hoang·Reyhanitabar·Rogaway·Vizár, "Online Authenticated-Encryption and its Nonce-Reuse Misuse-Resistance"(CRYPTO 2015).
- Wycheproof: [github.com/C2SP/wycheproof](https://github.com/C2SP/wycheproof).
- NIST SP 800-38A·38B·38D, RFC 8439·8032·7748·5869·4231·7693·4493.
- Rocks: `Docs/Design/Compression.ko.md`(외부 라이브러리 통합·스트리밍 규약), `Docs/Design/Rockpile.ko.md`(R0 SHA-256 HW 경로), `Docs/Design/AssetSystem`, `Core/Source/Runtime/PlatformBase/Public/Platform/Cpu.h`·`SecureRandom.h`·`VirtualMemory.h`, `Core/Public/Core/Containers/Span.h`·`Encoding/Sha256.h`.

사실 구분: libsodium·Tink 항목과 AES 라이브러리 비교는 1차 자료(공식 문서·원본·릴리스)로 확인했다. libsodium의 clang-cl(Windows MSVC ABI) 빌드는 공식 시험 대상이 아니어서 미확인이며, Rocks native 빌드로 확인할 위험 항목이다. 경량 계층 규모는 원본 줄 수에서 잡은 추정이고, 성능 수치는 측정 전이라 적지 않았다. STREAM 논문 서지, NIST·RFC 번호, VAES·SHA CPUID bit, macOS FEAT_* sysctl 이름, Goldmont의 AVX 부재는 원문을 다시 열람하지 않은 지식 기반(2차)이다.
