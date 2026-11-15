# Rocks 세그먼트 스트림 형식 v1

기준일: 2026-11-15. 이 문서는 세그먼트 스트림 wire 형식의 규범 명세다. 설계 배경과 위협 분석은
[Crypto 설계](Crypto.ko.md) 9.5절, API는 `Crypto/SegmentStream.h`, 구현은
`Core/Source/Runtime/Crypto/Private/SegmentFormat.cpp`가 맡는다. 6절의 시험 벡터는 C++ 구현의 golden
시험(`CryptoSegmentStreamGolden`)과 독립 Python 구현(7절)이 같은 값을 낸 것이다.

## 1. 기호

- 정수는 big-endian이다. BE64(x)는 x의 8 byte big-endian 표현이다.
- S = 2^log2S는 세그먼트 암호문 크기(tag 포함)다. log2S는 12–24(4 KiB–16 MiB)다.
- n은 세그먼트 수, r은 마지막 세그먼트 크기(tag 포함), T는 헤더를 포함한 전체 암호문 크기다.
- `‖`는 byte 이어 붙이기, `[a, b)`는 반열린 byte 구간이다.

## 2. 배치

T = 88 + (n − 1)·S + r이다. 헤더는 88 byte이고 그 뒤에 세그먼트 0 … n − 1이 이어진다.

| off | 길이 | 필드 | 값·규칙 |
|---|---|---|---|
| 0 | 4 | magic | `52 53 45 47`("RSEG") |
| 4 | 1 | version | 1 |
| 5 | 1 | algorithm | 1 = ChaCha20-Poly1305(RFC 8439, 96 bit nonce), 2 = AES-256-GCM |
| 6 | 1 | log2S | 12–24 |
| 7 | 1 | flags | 0(정의된 bit 없음) |
| 8 | 16 | keyId | BE64(high) ‖ BE64(low)(RFC 9562 byte 순서), Nil 금지 |
| 24 | 32 | salt | 스트림마다 OS 난수 |
| 56 | 32 | commitment C | 3절 |
| 88 | … | 세그먼트 i | 암호문 ‖ tag 16. i < n − 1이면 S byte, 마지막은 r byte |

## 3. 키 파생과 세그먼트 연산

```text
H    = header[0, 56)                                   // magic … salt
info = "rocks.segment.v1"(16 byte ASCII) ‖ H ‖ SHA-256(AD)   // 104 byte
K ‖ C = HKDF-SHA-256(IKM = 마스터 키(32 byte), salt = header[24, 56), info, L = 64)
nonce_i = 00 00 00 ‖ BE64(i) ‖ last_i                  // 12 byte, last_i = 01(마지막) 또는 00
segment_i = AEAD-Encrypt(key = K, nonce = nonce_i, AAD = 빈 값, plaintext_i) = 암호문 ‖ tag
```

- AD는 스트림 전체에 한 번 묶이며 SHA-256으로만 들어간다. 세그먼트 AAD는 비어 있다.
- K는 세그먼트 AEAD 키, C는 header[56, 88)에 기록하는 key commitment다. 헤더의 algorithm·log2S·keyId·
  salt가 info로 K와 C에 묶이므로 헤더 88 byte 전체가 인증된다(앞 56 byte는 파생으로, C는 동등 비교로).
- AEAD는 algorithm 값의 알고리즘이다(tag 16 byte, 분리 형식을 이어 붙인다).

## 4. 정규 인코딩과 한도

- 평문 P를 (S − 16) byte씩 나눈다. 마지막을 뺀 세그먼트는 꽉 찬다(평문 S − 16, 암호문 S).
- 빈 평문은 빈 마지막 세그먼트 하나(r = 16)다. n ≥ 2이면 마지막 세그먼트는 비어 있을 수 없다(r ≥ 17).
  그래서 n = max(1, ⌈P / (S − 16)⌉)이고 같은 평문의 인코딩은 하나다.
- n ≤ 2^32이다. ChaCha20-Poly1305의 평문 상한은 2^32 × (S − 16) byte다.
- AES-256-GCM은 세그먼트 암호문 합 T − 88을 2^40 − 16 byte(GCM 블록 2^36 − 1개) 이하로 둔다. 평문 상한은
  2^40 − 2^44/S − 16 byte다(S = 64 KiB이면 2^40 − 2^28 − 16).
- 봉인 쪽은 상한을 넘는 평문을 받지 않는다. 같은 (키, salt)로 세그먼트를 다시 봉인하지 않는다(내용을
  바꾸려면 새 salt로 전체를 다시 쓴다).

## 5. 복호 검증 순서

복호 쪽은 아래 순서로 판정한다. 메시지에서 온 byte의 위반은 모두 `AuthenticationFailed`이고, 공개
필드의 모르는 버전·알고리즘만 `Unsupported`다.

1. 헤더(byte 순서): 5 byte 미만이나 magic 불일치 → AuthenticationFailed. version ≠ 1 → Unsupported.
   헤더가 88 byte가 아님 → AuthenticationFailed. algorithm이 1·2가 아님 → Unsupported. log2S 범위 밖,
   flags ≠ 0, keyId가 Nil → AuthenticationFailed.
2. keyId가 쓰려는 키의 ID와 다름 → AuthenticationFailed. 이 기기에서 AES-256-GCM을 쓸 수 없음 →
   Unsupported.
3. K ‖ C를 파생하고 C를 header[56, 88)과 상수 시간으로 비교한다. 다르면 세그먼트를 하나도 열기 전에
   AuthenticationFailed다(잘못된 키, 헤더·AD 변조, 알고리즘·크기 downgrade).
4. 크기: 전체 크기를 알면 body = T − 88, n = ⌈body / S⌉, r = body − (n − 1)·S다. body < 16, n > 2^32,
   n ≥ 2인데 r ≤ 16, AES인데 body > 2^40 − 16 → AuthenticationFailed. 순차 복호는 크기를 미리 모르므로
   꽉 찬 세그먼트 뒤에 byte가 더 오면 last = 0으로, 입력이 끝나면 남은 byte를 last = 1로 연다(같은
   세그먼트를 다른 last로 다시 시도하지 않는다). 남은 byte가 위 규칙을 어기면 AuthenticationFailed다.
5. 세그먼트 i를 nonce_i로 검증한다. 실패하면 AuthenticationFailed이고, 검증 전 평문은 내보내지 않는다.
6. 마지막 세그먼트(last = 1)가 검증되어야 스트림이 완결된다. 그 뒤의 byte는 받지 않는다.

## 6. 시험 벡터

공통 입력: 마스터 키 `00 01 02 … 1f`, keyId `0123456789abcdef fedcba9876543210`, salt `40 41 … 5f`,
log2S = 12(S = 4096). 평문 byte i = (31·i + 7) mod 256. AD는 V1이 빈 값, V2·V3이
`"rocks segment golden"`(20 byte ASCII)이다.

| 벡터 | algorithm | 평문 | n | T |
|---|---|---|---|---|
| V1 | 1 ChaCha20-Poly1305 | 0 byte | 1(r = 16) | 104 |
| V2 | 1 ChaCha20-Poly1305 | 5000 byte | 2(r = 936) | 5120 |
| V3 | 2 AES-256-GCM | 8160 byte | 2(r = 4096) | 8280 |

V1 전체 암호문(104 byte):

```text
5253454701010c000123456789abcdeffedcba9876543210404142434445464748494a4b4c4d4e4f5051525354
55565758595a5b5c5d5e5f77a63a977eca576fd9d3d645a4358d41d89667114daab63804817cbe09eae763ba16
bc44eff93960faf1d5a7cd419495
```

| 값 | V1 | V2 | V3 |
|---|---|---|---|
| SHA-256(AD) | `e3b0c442…7852b855` | `4cfb2b02…cf8e1923` | V2와 같다 |
| K | `b48aaf7e26dbcdc9fa8ad0f2cc05939d70624b5bbdfacfd0414fae549ae57b02` | `231b43931d0491aa4de811c9b93b630b8b6e1adfa8d1f1b585f80471e64d4498` | `6f0b994cce778479d3e61d83b422523ae023b97a30061a81583928ef83bf799f` |
| C | `77a63a977eca576fd9d3d645a4358d41d89667114daab63804817cbe09eae763` | `d01846f14ffce5928c0ef95bfb3cf0835ef96a5cdd3a74ab4d6800dafc495169` | `990f2f666b31ef3ea56b4f8b25288841702bfb0a560875007f0c702d516debab` |
| tag 0 | `ba16bc44eff93960faf1d5a7cd419495` | `976de92e42577bf775997f8de9f4751b` | `c0b7dacaff2aff41012a04e47dc6ea4d` |
| tag 1 | — | `e5821fbfbd3ef3e1aa7974d395f64a4a` | `830ec24514f984190a62bbf6e2cc4a7d` |
| SHA-256(전체 암호문) | `879f9ed25bf61c0dd6c5db7efc59ef4dbf0ea2e8c0fa8389b3c467a8843e61b8` | `4f32a3cbc5f77ac29b5916bb568f75778e15467c67469532eb0f670c47304150` | `bbd90ee3e25d07749c611c00f2ebbc46167bfb378d2698d984d62c549ae51141` |

SHA-256(AD) 전체 값: 빈 AD `e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855`,
`"rocks segment golden"` `4cfb2b026383889be803e823180ce1c3a2588c00d2e1e0f706ac37a0cf8e1923`. 헤더는 V1의
앞 88 byte에서 V2는 C만, V3은 algorithm(byte 5 = 02)과 C만 다르다.

## 7. 독립 참조 구현

아래 Python(pyca/cryptography 50.0.2, OpenSSL 백엔드, hashlib)이 6절 값을 낸다. libsodium을 쓰지 않으므로
Rocks 구현과 독립이다.

```python
import hashlib, struct
from cryptography.hazmat.primitives import hashes
from cryptography.hazmat.primitives.ciphers.aead import AESGCM, ChaCha20Poly1305
from cryptography.hazmat.primitives.kdf.hkdf import HKDF

def seal(master, key_id, algorithm, log2, salt, ad, plaintext):
    h = b"RSEG" + bytes([1, algorithm, log2, 0]) + key_id + salt
    okm = HKDF(algorithm=hashes.SHA256(), length=64, salt=salt,
               info=b"rocks.segment.v1" + h + hashlib.sha256(ad).digest()).derive(master)
    aead = (ChaCha20Poly1305 if algorithm == 1 else AESGCM)(okm[:32])
    cap = (1 << log2) - 16
    n = max(1, -(-len(plaintext) // cap))
    out = h + okm[32:]
    for i in range(n):
        nonce = b"\0\0\0" + struct.pack(">Q", i) + bytes([i == n - 1])
        out += aead.encrypt(nonce, plaintext[i * cap:(i + 1) * cap], b"")
    return out
```
