// AES 경량 계층의 아키텍처와 무관한 부분이다(Docs/Design/Crypto.ko.md 5.2·5.3·9.3절): 커널 표
// 선택과 자가 시험, 키 스케줄 수명, GCM 일회성(분리 tag)·멀티파트, CTR(32·64·128 bit 카운터),
// CMAC. 커널은 AesX64.cpp·AesArm64.cpp에 있다.
//
// 출처: GCM 흐름은 libsodium 1.0.22-stable(ISC)의
// aead_aes256gcm_aesni.c·aead_aes256gcm_armcrypto.c를 따른다(원본 SHA-256은 Derivation.json).
// 대응하는 원본 함수:
//   crypto_aead_aes256gcm_encrypt_detached_afternm·aes_gcm_encrypt_generic
//     → AesGcmSealDetached(J0 = nonce ‖ 1, 카운터 2부터 CTR, AD → 암호문 → 길이 블록 GHASH,
//       tag = E_K(J0) xor GHASH)
//   crypto_aead_aes256gcm_decrypt_detached_afternm·aes_gcm_decrypt_generic
//     → AesGcmOpenDetached(암호문을 GHASH에 넣은 뒤 복호, 상수 시간 tag 비교, 실패하면 출력 소거)
// 원본에서 바꾼 점: 한 함수 안에서 블록마다 엮던 CTR·GHASH를 커널 표의 버퍼 함수 호출로 바꾸고,
// 조각(GcmChunkBytes) 단위로 번갈아 부른다. 실패한 열기의 출력은 0xd0 대신 0으로 지운다(11절).
// GCM 멀티파트(부분 블록 keystream·GHASH 버퍼, AD에서 데이터로 넘어가는 단계), CTR 카운터 폭과
// 용량 검사, CMAC(RFC 4493)은 원본에 없는 새 코드다.
//
// libsodium 고지(ISC License):
//   Copyright (c) 2013-2026 Frank Denis <j at pureftpd dot org>
//
//   Permission to use, copy, modify, and/or distribute this software for any purpose with or
//   without fee is hereby granted, provided that the above copyright notice and this permission
//   notice appear in all copies.
//
//   THE SOFTWARE IS PROVIDED "AS IS" AND THE AUTHOR DISCLAIMS ALL WARRANTIES WITH REGARD TO THIS
//   SOFTWARE INCLUDING ALL IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS. IN NO EVENT SHALL THE
//   AUTHOR BE LIABLE FOR ANY SPECIAL, DIRECT, INDIRECT, OR CONSEQUENTIAL DAMAGES OR ANY DAMAGES
//   WHATSOEVER RESULTING FROM LOSS OF USE, DATA OR PROFITS, WHETHER IN AN ACTION OF CONTRACT,
//   NEGLIGENCE OR OTHER TORTIOUS ACTION, ARISING OUT OF OR IN CONNECTION WITH THE USE OR
//   PERFORMANCE OF THIS SOFTWARE.

#include "AesKernels.h"
#include "AesModesInternal.h"

#include "../SecretArena.h"

#include <Base/Compiler.h>
#include <Base/Endian.h>
#include <Core/Diagnostics/Logger.h>
#include <Core/Move.h>
#include <Platform/Fatal.h>
#include <Platform/Memory.h>

#include <sodium.h>

namespace rocks::crypto::detail {
namespace {

// 이 파일의 진단 카테고리다. 출력에 `[Error][Crypto.Aes] ...`로 나간다.
constexpr LogCategory LogCryptoAes{"Crypto.Aes"};

// GCM 한 번에서 CTR과 GHASH를 번갈아 부르는 조각 크기다. 조각이 L1 캐시에 남아 있는 동안 두 커널이
// 같은 byte를 읽게 한다. x64 7블록·arm64 6블록 병렬 폭의 공배수(252블록, 4032 byte)라 조각 안에서는
// 병렬 경로만 돈다.
constexpr usize GcmChunkBytes = AesBlockBytes * 252;

// counter = nonce ‖ BE32(value)다(GCM J0는 value 1, 평문 첫 블록은 2).
void MakeCounterBlock(Span<const uint8> nonce, uint32 value, AesBlock& block) noexcept {
    platform::MemoryCopy(block.bytes, nonce.Data(), AesGcmNonceBytes);
    StoreBigEndian<uint32>(block.bytes + AesGcmNonceBytes, value);
}

// 호출자가 지켰어야 할 GCM 인자 규칙이다. 어기면 범위 밖 쓰기나 카운터 반복으로 이어지므로 결함으로
// 멈춘다.
void RequireGcmArguments(const AesKernels& kernels, Span<const uint8> nonce,
                         Span<const uint8> associatedData, usize inputBytes, usize outputBytes,
                         usize tagBytes) noexcept {
    if (!kernels.IsComplete() || nonce.Size() != AesGcmNonceBytes || tagBytes != AesGcmTagBytes ||
        inputBytes != outputBytes || static_cast<uint64>(inputBytes) > AesGcmMaxMessageBytes ||
        static_cast<uint64>(associatedData.Size()) > AesGcmMaxAssociatedBytes) {
        platform::Fatal("Crypto: AES-GCM precondition violated");
    }
}

// 길이 블록(BE64(AD bit 수) ‖ BE64(암호문 bit 수))을 GHASH에 넣고 tag = E_K(J0) xor GHASH를 tag에
// 쓴다(원본 aes_gcm_encrypt_generic의 final_block·mac 계산). GHASH 누적값과 E_K(J0)는 tag를
// 위조하는 데 쓰일 수 있으므로 지운다.
void FinishTag(const AesKernels& kernels, const AesSchedule& schedule, const AesBlock& j0,
               uint64 associatedBytes, uint64 messageBytes, AesBlock& state, uint8* tag) noexcept {
    AesBlock lengths;
    StoreBigEndian<uint64>(lengths.bytes, associatedBytes * 8);
    StoreBigEndian<uint64>(lengths.bytes + 8, messageBytes * 8);
    kernels.ghashUpdate(schedule, state, lengths.bytes, AesBlockBytes);

    AesBlock mask;
    kernels.encryptBlock(schedule, j0, mask);
    for (usize i = 0; i < AesGcmTagBytes; ++i) {
        tag[i] = static_cast<uint8>(mask.bytes[i] ^ state.bytes[i]);
    }

    sodium_memzero(&mask, sizeof(mask));
    sodium_memzero(&state, sizeof(state));
}

// ---- 자가 시험 ----

// 자가 시험 입력이다: 키 40 41 42 ...(16 또는 32 byte), 아래 nonce, AD 33 byte(29i + 3), 평문 245
// byte(13i + 7). 평문은 15블록 + 5 byte라 CTR 병렬 경로(7·6블록), GHASH 14블록 묶음, 부분 블록을
// 모두 지난다. 기대값은 Python cryptography 50.0.2(OpenSSL)와 pycryptodome 4.4.0이 같은 값을 낸
// 것을 확인했다(2026-11-15). CBC-MAC은 평문 앞 15블록, 시작 상태 0이다.
constexpr usize SelfTestAssociatedBytes = 33;
constexpr usize SelfTestMessageBytes = 245;
constexpr usize SelfTestCbcMacBlocks = 15;
constexpr uint8 SelfTestNonce[AesGcmNonceBytes] = {0xf0, 0xe1, 0xd2, 0xc3, 0xb4, 0xa5,
                                                   0x96, 0x87, 0x78, 0x69, 0x5a, 0x4b};

struct SelfTestVector {
    usize keyBytes;
    uint8 tag[AesGcmTagBytes];
    uint8 cbcMac[AesBlockBytes];
};

constexpr SelfTestVector SelfTestVectors[] = {
    {16,
     {0x7a, 0xd2, 0x41, 0x7f, 0x81, 0x3e, 0x49, 0xa0, 0xfe, 0x8e, 0x08, 0x5c, 0x78, 0x6f, 0x4c,
      0x60},
     {0x0a, 0x34, 0xa6, 0xe0, 0x25, 0x53, 0xda, 0x34, 0xfa, 0x6a, 0x2c, 0xf5, 0x89, 0xf4, 0x88,
      0xa5}},
    {32,
     {0x4d, 0xca, 0x0d, 0xad, 0x10, 0x84, 0xc7, 0x08, 0x04, 0x67, 0xc5, 0x7e, 0x61, 0x4f, 0x2a,
      0x7d},
     {0xaa, 0xa0, 0x1f, 0xa1, 0x90, 0x43, 0xc5, 0x70, 0x66, 0x14, 0x54, 0xb7, 0x2c, 0x1a, 0xb3,
      0x78}},
};

// kernels로 봉인 tag, 제자리 열기 왕복, CBC-MAC을 확인한다. 키는 공개 값이지만 스케줄은 지운다.
bool PassesSelfTest(const AesKernels& kernels, const SelfTestVector& vector) noexcept {
    uint8 key[32] = {};
    for (usize i = 0; i < vector.keyBytes; ++i) {
        key[i] = static_cast<uint8>(0x40 + i);
    }

    uint8 associatedData[SelfTestAssociatedBytes];
    for (usize i = 0; i < SelfTestAssociatedBytes; ++i) {
        associatedData[i] = static_cast<uint8>(i * 29 + 3);
    }

    uint8 message[SelfTestMessageBytes];
    for (usize i = 0; i < SelfTestMessageBytes; ++i) {
        message[i] = static_cast<uint8>(i * 13 + 7);
    }

    AesSchedule schedule;
    kernels.expandKey(key, vector.keyBytes, schedule);
    const Span<const uint8> nonce(SelfTestNonce);
    uint8 sealed[SelfTestMessageBytes];
    AesBlock tag;
    AesGcmSealDetached(kernels, schedule, nonce, Span<const uint8>(associatedData),
                       Span<const uint8>(message), Span<uint8>(sealed), Span<uint8>(tag.bytes));
    bool passed = platform::MemoryEquals(tag.bytes, vector.tag, AesGcmTagBytes);

    // 제자리 열기다. 암호문을 덮기 전에 GHASH에 넣는 순서까지 확인한다.
    const bool opened = AesGcmOpenDetached(
        kernels, schedule, nonce, Span<const uint8>(associatedData), Span<const uint8>(sealed),
        Span<const uint8>(tag.bytes), Span<uint8>(sealed));
    passed = passed && opened && platform::MemoryEquals(sealed, message, SelfTestMessageBytes);

    AesBlock mac = {};
    kernels.cbcMacUpdate(schedule, mac, message, SelfTestCbcMacBlocks);
    passed = passed && platform::MemoryEquals(mac.bytes, vector.cbcMac, AesBlockBytes);
    sodium_memzero(&schedule, sizeof(schedule));
    return passed;
}

// GetAesKernels의 첫 호출이 한 번 부른다. HW가 없으면 조용히 빈 표, 자가 시험이 실패하면 빈 표와
// stderr 한 줄이다(커널 결함이나 잘못된 CPU 보고이므로 진단에 남긴다).
AesKernels InitializeAesKernels() noexcept {
    const AesKernels candidate = SelectAesKernels(platform::QueryCpuFeatures());
    if (!candidate.IsComplete()) {
        return candidate;
    }

    const AesKernels checked = CheckAesKernels(candidate);
    if (!checked.IsComplete()) {
        StderrLogSink sink;
        LogEvent event;
        event.level = LogLevel::Error;
        event.category = LogCryptoAes.Name();
        event.message = "AES lightweight layer self-test failed; AES is disabled";
        event.location = SourceLocation::Current();
        (void)sink.Write(event);
    }

    return checked;
}

} // namespace

// ---- 커널 표 ----

bool HasAesInstructions(const platform::CpuFeatures& cpu) noexcept {
    if constexpr (IsX64) {
        return cpu.aes && cpu.pclmulqdq && cpu.avx;
    } else {
        return cpu.armAes && cpu.armPmull;
    }
}

AesKernels SelectAesKernels(const platform::CpuFeatures& cpu) noexcept {
    if (!HasAesInstructions(cpu)) {
        return AesKernels{};
    }

    return GetNativeAesKernels();
}

AesKernels CheckAesKernels(const AesKernels& candidate) noexcept {
    if (!candidate.IsComplete()) {
        return AesKernels{};
    }

    for (const SelfTestVector& vector : SelfTestVectors) {
        if (!PassesSelfTest(candidate, vector)) {
            return AesKernels{};
        }
    }

    return candidate;
}

const AesKernels& GetAesKernels() noexcept {
    static const AesKernels kernels = InitializeAesKernels();
    return kernels;
}

// ---- 키 스케줄 ----

Result<AesSchedule*> TryCreateAesSchedule(const AesKernels& kernels,
                                          Span<const uint8> key) noexcept {
    if (!kernels.IsComplete()) {
        return Result<AesSchedule*>::Failure(ErrorCode::Unsupported);
    }

    if (key.Size() != 16 && key.Size() != 32) {
        return Result<AesSchedule*>::Failure(ErrorCode::InvalidArgument);
    }

    auto slot = TryAllocateSecretSlot(sizeof(AesSchedule), alignof(AesSchedule));
    if (!slot) {
        return Result<AesSchedule*>::Failure(slot.Error());
    }

    AesSchedule* schedule = ConstructAt<AesSchedule>(slot.Value());
    kernels.expandKey(key.Data(), key.Size(), *schedule);
    return Result<AesSchedule*>::Success(schedule);
}

void DestroyAesSchedule(AesSchedule* schedule) noexcept {
    // 슬롯은 해제 때 전체가 지워진다(SecretArena). AesSchedule은 소멸자가 하는 일이 없다.
    FreeSecretSlot(schedule);
}

// ---- GCM ----

void AesGcmSealDetached(const AesKernels& kernels, const AesSchedule& schedule,
                        Span<const uint8> nonce, Span<const uint8> associatedData,
                        Span<const uint8> plaintext, Span<uint8> ciphertext,
                        Span<uint8> tag) noexcept {
    RequireGcmArguments(kernels, nonce, associatedData, plaintext.Size(), ciphertext.Size(),
                        tag.Size());
    AesBlock state = {};
    AesBlock counter;
    MakeCounterBlock(nonce, 2, counter);
    kernels.ghashUpdate(schedule, state, associatedData.Data(), associatedData.Size());
    for (usize offset = 0; offset < plaintext.Size(); offset += GcmChunkBytes) {
        const usize remaining = plaintext.Size() - offset;
        const usize bytes = remaining < GcmChunkBytes ? remaining : GcmChunkBytes;
        kernels.ctrXor(schedule, AesCounterWidth::Bits32, counter, plaintext.Data() + offset,
                       ciphertext.Data() + offset, bytes);
        kernels.ghashUpdate(schedule, state, ciphertext.Data() + offset, bytes);
    }

    AesBlock j0;
    MakeCounterBlock(nonce, 1, j0);
    FinishTag(kernels, schedule, j0, associatedData.Size(), plaintext.Size(), state, tag.Data());
}

bool AesGcmOpenDetached(const AesKernels& kernels, const AesSchedule& schedule,
                        Span<const uint8> nonce, Span<const uint8> associatedData,
                        Span<const uint8> ciphertext, Span<const uint8> tag,
                        Span<uint8> plaintext) noexcept {
    RequireGcmArguments(kernels, nonce, associatedData, ciphertext.Size(), plaintext.Size(),
                        tag.Size());
    AesBlock state = {};
    AesBlock counter;
    MakeCounterBlock(nonce, 2, counter);
    kernels.ghashUpdate(schedule, state, associatedData.Data(), associatedData.Size());
    for (usize offset = 0; offset < ciphertext.Size(); offset += GcmChunkBytes) {
        const usize remaining = ciphertext.Size() - offset;
        const usize bytes = remaining < GcmChunkBytes ? remaining : GcmChunkBytes;
        // 제자리 복호에서 암호문이 덮이기 전에 GHASH에 넣는다(원본 aes_gcm_decrypt_generic 순서).
        kernels.ghashUpdate(schedule, state, ciphertext.Data() + offset, bytes);
        kernels.ctrXor(schedule, AesCounterWidth::Bits32, counter, ciphertext.Data() + offset,
                       plaintext.Data() + offset, bytes);
    }

    AesBlock j0;
    MakeCounterBlock(nonce, 1, j0);
    AesBlock computed;
    FinishTag(kernels, schedule, j0, associatedData.Size(), ciphertext.Size(), state,
              computed.bytes);
    const bool matched = crypto_verify_16(computed.bytes, tag.Data()) == 0;
    sodium_memzero(&computed, sizeof(computed));
    if (!matched) {
        if (!plaintext.IsEmpty()) {
            sodium_memzero(plaintext.Data(), plaintext.Size());
        }

        return false;
    }

    return true;
}

// ---- GCM 멀티파트 ----

namespace {

// 부분 블록이 차면 GHASH에 넣는다.
void FlushFullPending(AesGcmStream& stream) noexcept {
    if (stream.pendingBytes == AesBlockBytes) {
        stream.kernels->ghashUpdate(stream.schedule, stream.ghash, stream.pending.bytes,
                                    AesBlockBytes);
        stream.pendingBytes = 0;
    }
}

// AD 단계를 끝낸다. 남은 AD 부분 블록은 0으로 채워 GHASH에 넣는다(SP 800-38D의 AD 0 채우기).
void BeginGcmData(AesGcmStream& stream) noexcept {
    if (stream.dataStarted) {
        return;
    }

    if (stream.pendingBytes != 0) {
        stream.kernels->ghashUpdate(stream.schedule, stream.ghash, stream.pending.bytes,
                                    stream.pendingBytes);
        stream.pendingBytes = 0;
    }

    stream.dataStarted = true;
}

// 현재 블록의 남은 keystream(keystream[pendingBytes..])으로 count byte를 처리하고 GHASH에 넣을
// 암호문 byte를 pending에 모은다. 제자리에서도 입력 byte를 먼저 읽는다.
void XorPartialBlock(AesGcmStream& stream, bool encrypt, const uint8* input, uint8* output,
                     usize count) noexcept {
    for (usize i = 0; i < count; ++i) {
        const uint8 in = input[i];
        const uint8 out = static_cast<uint8>(in ^ stream.keystream.bytes[stream.pendingBytes + i]);
        output[i] = out;
        stream.pending.bytes[stream.pendingBytes + i] = encrypt ? out : in;
    }

    stream.pendingBytes += static_cast<uint32>(count);
}

} // namespace

void AesGcmStreamStart(const AesKernels& kernels, const AesSchedule& source,
                       Span<const uint8> nonce, AesGcmStream& stream) noexcept {
    if (!kernels.IsComplete() || nonce.Size() != AesGcmNonceBytes) {
        platform::Fatal("Crypto: AES-GCM stream precondition violated");
    }

    platform::MemoryZero(&stream, sizeof(stream));
    platform::MemoryCopy(&stream.schedule, &source, sizeof(AesSchedule));
    MakeCounterBlock(nonce, 1, stream.j0);
    MakeCounterBlock(nonce, 2, stream.counter);
    stream.kernels = &kernels;
}

void AesGcmStreamAbsorbAssociated(AesGcmStream& stream, Span<const uint8> associatedData) noexcept {
    const usize size = associatedData.Size();
    if (stream.dataStarted ||
        static_cast<uint64>(size) > AesGcmMaxAssociatedBytes - stream.associatedBytes) {
        platform::Fatal("Crypto: AES-GCM stream associated data precondition violated");
    }

    if (size == 0) {
        return;
    }

    const uint8* data = associatedData.Data();
    usize offset = 0;
    if (stream.pendingBytes != 0) {
        const usize room = AesBlockBytes - stream.pendingBytes;
        offset = size < room ? size : room;
        platform::MemoryCopy(stream.pending.bytes + stream.pendingBytes, data, offset);
        stream.pendingBytes += static_cast<uint32>(offset);
        FlushFullPending(stream);
    }

    const usize whole = (size - offset) & ~(AesBlockBytes - 1);
    if (whole != 0) {
        stream.kernels->ghashUpdate(stream.schedule, stream.ghash, data + offset, whole);
        offset += whole;
    }

    if (offset < size) {
        platform::MemoryCopy(stream.pending.bytes, data + offset, size - offset);
        stream.pendingBytes = static_cast<uint32>(size - offset);
    }

    stream.associatedBytes += size;
}

void AesGcmStreamProcess(AesGcmStream& stream, bool encrypt, Span<const uint8> input,
                         Span<uint8> output) noexcept {
    const usize size = input.Size();
    if (output.Size() != size ||
        static_cast<uint64>(size) > AesGcmMaxMessageBytes - stream.messageBytes) {
        platform::Fatal("Crypto: AES-GCM stream data precondition violated");
    }

    BeginGcmData(stream);
    if (size == 0) {
        return;
    }

    const AesKernels& kernels = *stream.kernels;
    const uint8* in = input.Data();
    uint8* out = output.Data();
    usize offset = 0;
    if (stream.pendingBytes != 0) {
        const usize room = AesBlockBytes - stream.pendingBytes;
        offset = size < room ? size : room;
        XorPartialBlock(stream, encrypt, in, out, offset);
        FlushFullPending(stream);
    }

    // 블록 경계에 맞은 부분이다. 일회성과 같이 조각마다 CTR과 GHASH를 번갈아 부른다.
    const usize whole = (size - offset) & ~(AesBlockBytes - 1);
    for (usize done = 0; done < whole; done += GcmChunkBytes) {
        const usize bytes = whole - done < GcmChunkBytes ? whole - done : GcmChunkBytes;
        const uint8* source = in + offset + done;
        uint8* target = out + offset + done;
        if (encrypt) {
            kernels.ctrXor(stream.schedule, AesCounterWidth::Bits32, stream.counter, source, target,
                           bytes);
            kernels.ghashUpdate(stream.schedule, stream.ghash, target, bytes);
        } else {
            kernels.ghashUpdate(stream.schedule, stream.ghash, source, bytes);
            kernels.ctrXor(stream.schedule, AesCounterWidth::Bits32, stream.counter, source, target,
                           bytes);
        }
    }

    offset += whole;
    if (offset < size) {
        // 새 keystream 블록 하나를 만들어 앞부분만 쓰고 나머지는 다음 호출에 남긴다.
        platform::MemoryZero(stream.keystream.bytes, AesBlockBytes);
        kernels.ctrXor(stream.schedule, AesCounterWidth::Bits32, stream.counter,
                       stream.keystream.bytes, stream.keystream.bytes, AesBlockBytes);
        XorPartialBlock(stream, encrypt, in + offset, out + offset, size - offset);
    }

    stream.messageBytes += size;
}

void AesGcmStreamFinish(AesGcmStream& stream, Span<uint8> tag) noexcept {
    if (tag.Size() != AesGcmTagBytes) {
        platform::Fatal("Crypto: AES-GCM stream tag precondition violated");
    }

    BeginGcmData(stream);
    if (stream.pendingBytes != 0) {
        stream.kernels->ghashUpdate(stream.schedule, stream.ghash, stream.pending.bytes,
                                    stream.pendingBytes);
        stream.pendingBytes = 0;
    }

    FinishTag(*stream.kernels, stream.schedule, stream.j0, stream.associatedBytes,
              stream.messageBytes, stream.ghash, tag.Data());
    sodium_memzero(&stream, sizeof(stream));
}

// ---- CTR ----

namespace {

constexpr uint32 AllOnes32 = NumericLimits<uint32>::Max();
constexpr uint64 AllOnes64 = NumericLimits<uint64>::Max();

// 폭 안에서 현재 값 뒤에 남은 카운터 값 수(= 폭의 모든 bit가 1인 값 - c)다. 128 bit에서 앞 64 bit가
// 모두 1이 아니면 2^64개 이상 남았으므로 AllOnes64로 포화한다.
uint64 RemainingAfterCurrent(const AesBlock& counter, AesCounterWidth width) noexcept {
    switch (width) {
    case AesCounterWidth::Bits32:
        return static_cast<uint64>(AllOnes32 - LoadBigEndian<uint32>(counter.bytes + 12));
    case AesCounterWidth::Bits64:
        return AllOnes64 - LoadBigEndian<uint64>(counter.bytes + 8);
    case AesCounterWidth::Bits128:
        if (LoadBigEndian<uint64>(counter.bytes) != AllOnes64) {
            return AllOnes64;
        }

        return AllOnes64 - LoadBigEndian<uint64>(counter.bytes + 8);
    }

    platform::Fatal("Crypto: AES CTR got an invalid counter width");
}

} // namespace

bool AesCtrHasCapacity(const AesBlock& counter, AesCounterWidth width, bool exhausted,
                       uint64 blocks) noexcept {
    if (blocks == 0) {
        return true;
    }

    return !exhausted && blocks - 1 <= RemainingAfterCurrent(counter, width);
}

bool AesCtrUsesLastCounter(const AesBlock& counter, AesCounterWidth width, uint64 blocks) noexcept {
    if (blocks == 0) {
        return false;
    }

    // 128 bit에서 앞 64 bit가 모두 1이 아니면 포화값이라 blocks - 1(2^60 미만)과 같을 수 없다.
    return blocks - 1 == RemainingAfterCurrent(counter, width);
}

void AesCtrStreamStart(const AesKernels& kernels, const AesSchedule& source,
                       const AesBlock& initialCounter, AesCounterWidth width,
                       AesCtrStream& stream) noexcept {
    if (!kernels.IsComplete()) {
        platform::Fatal("Crypto: AES CTR stream precondition violated");
    }

    platform::MemoryZero(&stream, sizeof(stream));
    platform::MemoryCopy(&stream.schedule, &source, sizeof(AesSchedule));
    stream.counter = initialCounter;
    stream.width = width;
    stream.kernels = &kernels;
}

uint64 AesCtrStreamBlocksFor(const AesCtrStream& stream, usize bytes) noexcept {
    if (bytes <= stream.keystreamBytes) {
        return 0;
    }

    const usize rest = bytes - stream.keystreamBytes;
    return static_cast<uint64>(rest / AesBlockBytes) + (rest % AesBlockBytes != 0 ? 1 : 0);
}

void AesCtrStreamProcess(AesCtrStream& stream, Span<const uint8> input,
                         Span<uint8> output) noexcept {
    const usize size = input.Size();
    const uint64 blocks = AesCtrStreamBlocksFor(stream, size);
    if (output.Size() != size ||
        !AesCtrHasCapacity(stream.counter, stream.width, stream.exhausted, blocks)) {
        platform::Fatal("Crypto: AES CTR stream precondition violated");
    }

    const bool usesLast = AesCtrUsesLastCounter(stream.counter, stream.width, blocks);
    const AesKernels& kernels = *stream.kernels;
    const uint8* in = input.Data();
    uint8* out = output.Data();
    usize offset = 0;
    if (stream.keystreamBytes != 0 && size != 0) {
        offset = size < stream.keystreamBytes ? size : stream.keystreamBytes;
        const usize start = AesBlockBytes - stream.keystreamBytes;
        for (usize i = 0; i < offset; ++i) {
            out[i] = static_cast<uint8>(in[i] ^ stream.keystream.bytes[start + i]);
        }

        stream.keystreamBytes -= static_cast<uint32>(offset);
    }

    const usize whole = (size - offset) & ~(AesBlockBytes - 1);
    if (whole != 0) {
        kernels.ctrXor(stream.schedule, stream.width, stream.counter, in + offset, out + offset,
                       whole);
        offset += whole;
    }

    if (offset < size) {
        // 새 keystream 블록 하나를 만들어 앞부분만 쓰고 나머지는 다음 호출에 남긴다.
        platform::MemoryZero(stream.keystream.bytes, AesBlockBytes);
        kernels.ctrXor(stream.schedule, stream.width, stream.counter, stream.keystream.bytes,
                       stream.keystream.bytes, AesBlockBytes);
        const usize left = size - offset;
        for (usize i = 0; i < left; ++i) {
            out[offset + i] = static_cast<uint8>(in[offset + i] ^ stream.keystream.bytes[i]);
        }

        stream.keystreamBytes = static_cast<uint32>(AesBlockBytes - left);
    }

    if (stream.keystreamBytes == 0) {
        sodium_memzero(stream.keystream.bytes, AesBlockBytes);
    }

    // 마지막 카운터 값을 썼으면 커널이 카운터를 폭 안에서 0으로 돌렸다. 그 값을 다시 쓰지 않게
    // 막는다.
    if (usesLast) {
        stream.exhausted = true;
    }
}

// ---- CMAC ----

namespace {

// CMAC 서브키 doubling(RFC 4493 2.3, SP 800-38B 6.1): 128 bit를 왼쪽으로 1 bit 밀고, 맨 앞 bit가
// 1이었으면 끝 byte에 R_128 = 0x87을 XOR한다. L = E_K(0)이 비밀이므로 분기 없이 mask로 한다.
void DoubleSubkey(AesBlock& block) noexcept {
    const uint8 mask = static_cast<uint8>(0u - static_cast<uint32>(block.bytes[0] >> 7));
    for (usize i = 0; i + 1 < AesBlockBytes; ++i) {
        block.bytes[i] = static_cast<uint8>((block.bytes[i] << 1) | (block.bytes[i + 1] >> 7));
    }

    block.bytes[AesBlockBytes - 1] =
        static_cast<uint8>((block.bytes[AesBlockBytes - 1] << 1) ^ (0x87 & mask));
}

} // namespace

void AesCmacUpdate(const AesKernels& kernels, const AesSchedule& schedule, AesCmacState& state,
                   Span<const uint8> message) noexcept {
    const usize size = message.Size();
    if (size == 0) {
        return;
    }

    if (state.pendingBytes > AesBlockBytes) {
        platform::Fatal("Crypto: AES-CMAC state is corrupt");
    }

    const uint8* data = message.Data();
    const usize room = AesBlockBytes - state.pendingBytes;
    usize offset = size < room ? size : room;
    platform::MemoryCopy(state.pending.bytes + state.pendingBytes, data, offset);
    state.pendingBytes += offset;
    if (offset == size) {
        return;
    }

    // 뒤에 입력이 더 있으므로 찬 블록은 마지막 블록이 아니다.
    kernels.cbcMacUpdate(schedule, state.chain, state.pending.bytes, 1);
    state.pendingBytes = 0;

    // 마지막 1–16 byte는 남긴다(마지막 블록 후보).
    const usize blocks = (size - offset - 1) / AesBlockBytes;
    if (blocks != 0) {
        kernels.cbcMacUpdate(schedule, state.chain, data + offset, blocks);
        offset += blocks * AesBlockBytes;
    }

    platform::MemoryCopy(state.pending.bytes, data + offset, size - offset);
    state.pendingBytes = size - offset;
}

void AesCmacFinish(const AesKernels& kernels, const AesSchedule& schedule, AesCmacState& state,
                   Span<uint8> tag) noexcept {
    if (tag.Size() != AesCmacTagBytes || state.pendingBytes > AesBlockBytes) {
        platform::Fatal("Crypto: AES-CMAC finish precondition violated");
    }

    // 서브키: K1 = dbl(L)이고, 마지막 블록이 부분 블록이면 K2 = dbl(K1)이다.
    const AesBlock zero = {};
    AesBlock subkey;
    kernels.encryptBlock(schedule, zero, subkey);
    DoubleSubkey(subkey);
    AesBlock last = {};
    platform::MemoryCopy(last.bytes, state.pending.bytes, state.pendingBytes);
    if (state.pendingBytes != AesBlockBytes) {
        // 부분 블록(빈 메시지 포함)은 10* 채우기다. 길이는 비밀이 아니다.
        last.bytes[state.pendingBytes] = 0x80;
        DoubleSubkey(subkey);
    }

    for (usize i = 0; i < AesBlockBytes; ++i) {
        last.bytes[i] = static_cast<uint8>(last.bytes[i] ^ subkey.bytes[i]);
    }

    kernels.cbcMacUpdate(schedule, state.chain, last.bytes, 1);
    platform::MemoryCopy(tag.Data(), state.chain.bytes, AesCmacTagBytes);
    sodium_memzero(&subkey, sizeof(subkey));
    sodium_memzero(&last, sizeof(last));
    sodium_memzero(&state, sizeof(state));
}

} // namespace rocks::crypto::detail
