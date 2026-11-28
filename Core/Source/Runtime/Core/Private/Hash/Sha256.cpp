#include <Core/Hash/Sha256.h>

#include "Sha256Kernels.h"

#include <Base/Compiler.h>
#include <Platform/Fatal.h>
#include <Platform/Memory.h>

namespace rocks {
namespace {

constexpr uint32 RotateRight(uint32 value, uint32 count) noexcept {
    return (value >> count) | (value << (32u - count));
}

int HexValue(char value) noexcept {
    if (value >= '0' && value <= '9') {
        return value - '0';
    }
    if (value >= 'a' && value <= 'f') {
        return value - 'a' + 10;
    }
    if (value >= 'A' && value <= 'F') {
        return value - 'A' + 10;
    }
    return -1;
}

} // namespace

void Sha256Digest::ToHex(char (&out)[HexBytes + 1]) const noexcept {
    constexpr char Digits[] = "0123456789abcdef";
    for (usize index = 0; index < Bytes; ++index) {
        out[index * 2u] = Digits[bytes[index] >> 4];
        out[index * 2u + 1u] = Digits[bytes[index] & 0x0Fu];
    }
    out[HexBytes] = '\0';
}

Result<Sha256Digest> Sha256Digest::ParseHex(StringView text) noexcept {
    if (text.ByteCount() != HexBytes) {
        return Result<Sha256Digest>::Failure(ErrorCode::InvalidArgument);
    }
    Sha256Digest digest;
    for (usize index = 0; index < Bytes; ++index) {
        const int high = HexValue(text.Data()[index * 2u]);
        const int low = HexValue(text.Data()[index * 2u + 1u]);
        if (high < 0 || low < 0) {
            return Result<Sha256Digest>::Failure(ErrorCode::InvalidArgument);
        }
        digest.bytes[index] = static_cast<uint8>((high << 4) | low);
    }
    return Result<Sha256Digest>::Success(digest);
}

void Sha256::Reset() noexcept {
    state_[0] = 0x6a09e667u;
    state_[1] = 0xbb67ae85u;
    state_[2] = 0x3c6ef372u;
    state_[3] = 0xa54ff53au;
    state_[4] = 0x510e527fu;
    state_[5] = 0x9b05688cu;
    state_[6] = 0x1f83d9abu;
    state_[7] = 0x5be0cd19u;
    length_ = 0;
    used_ = 0;
}

namespace detail {

// FIPS 180-4 4.2.2 라운드 상수다. 커널 파일들이 같이 쓴다.
const uint32 Sha256RoundConstants[64] = {
    0x428a2f98u, 0x71374491u, 0xb5c0fbcfu, 0xe9b5dba5u, 0x3956c25bu, 0x59f111f1u, 0x923f82a4u,
    0xab1c5ed5u, 0xd807aa98u, 0x12835b01u, 0x243185beu, 0x550c7dc3u, 0x72be5d74u, 0x80deb1feu,
    0x9bdc06a7u, 0xc19bf174u, 0xe49b69c1u, 0xefbe4786u, 0x0fc19dc6u, 0x240ca1ccu, 0x2de92c6fu,
    0x4a7484aau, 0x5cb0a9dcu, 0x76f988dau, 0x983e5152u, 0xa831c66du, 0xb00327c8u, 0xbf597fc7u,
    0xc6e00bf3u, 0xd5a79147u, 0x06ca6351u, 0x14292967u, 0x27b70a85u, 0x2e1b2138u, 0x4d2c6dfcu,
    0x53380d13u, 0x650a7354u, 0x766a0abbu, 0x81c2c92eu, 0x92722c85u, 0xa2bfe8a1u, 0xa81a664bu,
    0xc24b8b70u, 0xc76c51a3u, 0xd192e819u, 0xd6990624u, 0xf40e3585u, 0x106aa070u, 0x19a4c116u,
    0x1e376c08u, 0x2748774cu, 0x34b0bcb5u, 0x391c0cb3u, 0x4ed8aa4au, 0x5b9cca4fu, 0x682e6ff3u,
    0x748f82eeu, 0x78a5636fu, 0x84c87814u, 0x8cc70208u, 0x90befffau, 0xa4506cebu, 0xbef9a3f7u,
    0xc67178f2u};

namespace {

// 스칼라 압축 한 블록이다(FIPS 180-4 6.2.2).
void CompressScalar(uint32* state, const uint8* block) noexcept {
    uint32 schedule[64];
    for (uint32 index = 0; index < 16; ++index) {
        schedule[index] = (static_cast<uint32>(block[index * 4u]) << 24) |
                          (static_cast<uint32>(block[index * 4u + 1u]) << 16) |
                          (static_cast<uint32>(block[index * 4u + 2u]) << 8) |
                          static_cast<uint32>(block[index * 4u + 3u]);
    }
    for (uint32 index = 16; index < 64; ++index) {
        const uint32 s0 = RotateRight(schedule[index - 15], 7) ^
                          RotateRight(schedule[index - 15], 18) ^ (schedule[index - 15] >> 3);
        const uint32 s1 = RotateRight(schedule[index - 2], 17) ^
                          RotateRight(schedule[index - 2], 19) ^ (schedule[index - 2] >> 10);
        schedule[index] = schedule[index - 16] + s0 + schedule[index - 7] + s1;
    }
    uint32 a = state[0];
    uint32 b = state[1];
    uint32 c = state[2];
    uint32 d = state[3];
    uint32 e = state[4];
    uint32 f = state[5];
    uint32 g = state[6];
    uint32 h = state[7];
    for (uint32 index = 0; index < 64; ++index) {
        const uint32 sum1 = RotateRight(e, 6) ^ RotateRight(e, 11) ^ RotateRight(e, 25);
        const uint32 choose = (e & f) ^ (~e & g);
        const uint32 first = h + sum1 + choose + Sha256RoundConstants[index] + schedule[index];
        const uint32 sum0 = RotateRight(a, 2) ^ RotateRight(a, 13) ^ RotateRight(a, 22);
        const uint32 majority = (a & b) ^ (a & c) ^ (b & c);
        const uint32 second = sum0 + majority;
        h = g;
        g = f;
        f = e;
        e = d + first;
        d = c;
        c = b;
        b = a;
        a = first + second;
    }
    state[0] += a;
    state[1] += b;
    state[2] += c;
    state[3] += d;
    state[4] += e;
    state[5] += f;
    state[6] += g;
    state[7] += h;
}

// 자가 시험의 초기 상태다(FIPS 180-4 5.3.3).
constexpr uint32 InitialState[8] = {0x6a09e667u, 0xbb67ae85u, 0x3c6ef372u, 0xa54ff53au,
                                    0x510e527fu, 0x9b05688cu, 0x1f83d9abu, 0x5be0cd19u};

// 자가 시험 블록 수다(여러 블록 한 번 호출과 블록마다 호출의 동치).
constexpr usize SelfTestBlocks = 5;

// 패딩한 자가 시험 메시지 버퍼 크기다(두 블록).
constexpr usize SelfTestMessageBytes = 128;

// "abc" 한 블록의 결과다(FIPS 180-4 예제).
constexpr uint32 AbcDigest[8] = {0xba7816bfu, 0x8f01cfeau, 0x414140deu, 0x5dae2223u,
                                 0xb00361a3u, 0x96177a9cu, 0xb410ff61u, 0xf20015adu};

// 448 bit NIST 메시지의 두 블록 결과다.
constexpr uint32 TwoBlockDigest[8] = {0x248d6a61u, 0xd20638b8u, 0xe5c02693u, 0x0c3e6039u,
                                      0xa33ce459u, 0x64ff2167u, 0xf6ecedd4u, 0x19db06c1u};

constexpr char TwoBlockMessage[] = "abcdbcdecdefdefgefghfghighijhijkijkljklmklmnlmnomnopnopq";

bool StateEquals(const uint32* state, const uint32* expected) noexcept {
    for (usize index = 0; index < 8; ++index) {
        if (state[index] != expected[index]) {
            return false;
        }
    }

    return true;
}

// 메시지(55 byte 이하는 한 블록, 그 위는 두 블록)에 패딩을 붙여 out에 쓰고 블록 수를 돌려준다.
usize PadMessage(const char* message, usize bytes, uint8 (&out)[SelfTestMessageBytes]) noexcept {
    platform::MemorySet(out, 0, sizeof(out));
    platform::MemoryCopy(out, message, bytes);
    out[bytes] = 0x80u;
    const usize blocks = bytes + 9u <= 64u ? 1u : 2u;
    const uint64 bits = static_cast<uint64>(bytes) * 8u;
    for (uint32 index = 0; index < 8; ++index) {
        out[blocks * 64u - 1u - index] = static_cast<uint8>(bits >> (index * 8u));
    }

    return blocks;
}

Sha256Kernel InitializeActiveKernel() noexcept {
    const Sha256Kernel candidate = SelectSha256Kernel(platform::QueryCpuFeatures());
    if (candidate == Sha256Kernel::Scalar || CheckSha256Kernel(Sha256KernelBlocks(candidate))) {
        return candidate;
    }

    // 커널 결함이나 잘못된 CPU 보고다. Core는 Log에 의존하지 않으므로 stderr에 바로 남긴다.
    constexpr char Message[] =
        "[Error][Sha256] SHA-256 hardware kernel self-test failed; using the scalar kernel\n";
    platform::WriteDiagnostic(Message, sizeof(Message) - 1);
    return Sha256Kernel::Scalar;
}

// 이 프로세스의 블록 함수다(첫 호출에 한 번 고른다).
Sha256BlocksFunction ActiveBlocks() noexcept {
    static const Sha256BlocksFunction blocks = Sha256KernelBlocks(ActiveSha256Kernel());
    return blocks;
}

} // namespace

void Sha256BlocksScalar(uint32* state, const uint8* blocks, usize count) noexcept {
    for (usize index = 0; index < count; ++index) {
        CompressScalar(state, blocks + index * 64u);
    }
}

const char* Sha256KernelName(Sha256Kernel kernel) noexcept {
    switch (kernel) {
    case Sha256Kernel::Scalar:
        return "Scalar";
    case Sha256Kernel::X64Sha:
        return "X64Sha";
    case Sha256Kernel::Arm64Sha2:
        return "Arm64Sha2";
    }

    return "unknown";
}

Sha256Kernel SelectSha256Kernel(const platform::CpuFeatures& cpu) noexcept {
#if ROCKS_ARCH_X64
    if (cpu.sha && cpu.sse42) {
        return Sha256Kernel::X64Sha;
    }
#elif ROCKS_ARCH_ARM64
    if (cpu.armSha2) {
        return Sha256Kernel::Arm64Sha2;
    }
#endif

    (void)cpu;
    return Sha256Kernel::Scalar;
}

Sha256BlocksFunction Sha256KernelBlocks(Sha256Kernel kernel) noexcept {
    switch (kernel) {
    case Sha256Kernel::Scalar:
        return &Sha256BlocksScalar;
    case Sha256Kernel::X64Sha:
#if ROCKS_ARCH_X64
        return &Sha256BlocksX64Sha;
#else
        return nullptr;
#endif
    case Sha256Kernel::Arm64Sha2:
#if ROCKS_ARCH_ARM64
        return &Sha256BlocksArm64Sha2;
#else
        return nullptr;
#endif
    }

    return nullptr;
}

bool CheckSha256Kernel(Sha256BlocksFunction blocks) noexcept {
    if (blocks == nullptr) {
        return false;
    }

    uint8 padded[SelfTestMessageBytes];
    uint32 state[8];
    usize count = PadMessage("abc", 3, padded);
    platform::MemoryCopy(state, InitialState, sizeof(state));
    blocks(state, padded, count);
    if (!StateEquals(state, AbcDigest)) {
        return false;
    }

    count = PadMessage(TwoBlockMessage, sizeof(TwoBlockMessage) - 1, padded);
    platform::MemoryCopy(state, InitialState, sizeof(state));
    blocks(state, padded, count);
    if (!StateEquals(state, TwoBlockDigest)) {
        return false;
    }

    // 여러 블록을 한 번에 넘긴 결과가 스칼라로 블록마다 압축한 결과와 같아야 한다.
    uint8 data[SelfTestBlocks * 64u];
    for (usize index = 0; index < sizeof(data); ++index) {
        data[index] = static_cast<uint8>(index * 131u + 17u);
    }

    uint32 expected[8];
    platform::MemoryCopy(expected, InitialState, sizeof(expected));
    Sha256BlocksScalar(expected, data, SelfTestBlocks);
    platform::MemoryCopy(state, InitialState, sizeof(state));
    blocks(state, data, SelfTestBlocks);
    return StateEquals(state, expected);
}

Sha256Kernel ActiveSha256Kernel() noexcept {
    static const Sha256Kernel kernel = InitializeActiveKernel();
    return kernel;
}

} // namespace detail

void Sha256::Compress(const uint8* block) noexcept {
    detail::ActiveBlocks()(state_, block, 1);
}

void Sha256::Update(Span<const uint8> data) noexcept {
    const usize size = data.Size();
    if (size > (0x1FFFFFFFFFFFFFFFull - length_)) {
        platform::Fatal("SHA-256 input longer than 2^61 - 1 bytes");
    }
    length_ += size;
    const uint8* bytes = data.Data();
    usize offset = 0;
    if (used_ != 0) {
        const usize take = size < 64u - used_ ? size : 64u - used_;
        platform::MemoryCopy(block_ + used_, bytes, take);
        used_ += take;
        offset = take;
        if (used_ < 64u) {
            return;
        }
        Compress(block_);
        used_ = 0;
    }
    // 남은 전체 블록은 커널 한 번에 넘긴다(블록 하나가 아니라 버퍼 단위 경계).
    const usize blocks = (size - offset) / 64u;
    if (blocks != 0) {
        detail::ActiveBlocks()(state_, bytes + offset, blocks);
        offset += blocks * 64u;
    }
    if (offset < size) {
        platform::MemoryCopy(block_, bytes + offset, size - offset);
        used_ = size - offset;
    }
}

Sha256Digest Sha256::Finish() noexcept {
    const uint64 bitLength = length_ * 8u;
    block_[used_++] = 0x80u;
    if (used_ > 56u) {
        platform::MemorySet(block_ + used_, 0, 64u - used_);
        Compress(block_);
        used_ = 0;
    }
    platform::MemorySet(block_ + used_, 0, 56u - used_);
    for (uint32 index = 0; index < 8; ++index) {
        block_[56u + index] = static_cast<uint8>(bitLength >> (56u - index * 8u));
    }
    Compress(block_);
    Sha256Digest digest;
    for (uint32 index = 0; index < 8; ++index) {
        digest.bytes[index * 4u] = static_cast<uint8>(state_[index] >> 24);
        digest.bytes[index * 4u + 1u] = static_cast<uint8>(state_[index] >> 16);
        digest.bytes[index * 4u + 2u] = static_cast<uint8>(state_[index] >> 8);
        digest.bytes[index * 4u + 3u] = static_cast<uint8>(state_[index]);
    }
    Reset();
    return digest;
}

} // namespace rocks
