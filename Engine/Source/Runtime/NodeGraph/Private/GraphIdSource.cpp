#include <Core/Hash/Sha256.h>
#include <Core/Guid.h>
#include <NodeGraph/GraphIdSource.h>

// 그래프 id 발급기다(Docs/Design/NodeGraph 8.5절).
namespace rocks::nodegraph {

namespace {

// seed 발급기 hash 입력의 바이트 수다(seed·asset high·asset low·순번, 각 u64).
constexpr usize SeededIdInputBytes = 32;

constexpr usize SeededIdWordBytes = 8;

void SeededIdPut(uint8* out, uint64 value) noexcept {
    for (usize index = 0; index < SeededIdWordBytes; ++index) {
        out[index] = static_cast<uint8>(value >> (index * 8));
    }
}

uint64 SeededIdGet(const uint8* in) noexcept {
    uint64 value = 0;
    for (usize index = 0; index < SeededIdWordBytes; ++index) {
        value |= static_cast<uint64>(in[index]) << (index * 8);
    }

    return value;
}

} // namespace

Result<Guid> RandomGraphIdSource::Next() noexcept {
    return TryNewGuid();
}

Result<Guid> SeededGraphIdSource::Next() noexcept {
    for (;;) {
        uint8 input[SeededIdInputBytes] = {};
        SeededIdPut(input, seed_);
        SeededIdPut(input + SeededIdWordBytes, asset_.high);
        SeededIdPut(input + 2 * SeededIdWordBytes, asset_.low);
        SeededIdPut(input + 3 * SeededIdWordBytes, counter_);
        ++counter_;
        const Sha256Digest digest = Sha256::Hash(Span<const uint8>(input, SeededIdInputBytes));
        const Guid id{SeededIdGet(digest.bytes), SeededIdGet(digest.bytes + SeededIdWordBytes)};
        if (id.high != 0 || id.low != 0) {
            return Result<Guid>::Success(id);
        }
    }
}

} // namespace rocks::nodegraph
