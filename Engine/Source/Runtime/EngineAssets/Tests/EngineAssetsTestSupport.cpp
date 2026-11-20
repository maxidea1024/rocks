#include "EngineAssetsTestSupport.h"

#include <AssetTestFixtures/AssetTestFixtures.module.generated.h>

// EngineAssets 시험 공용 도구의 정의다(EngineAssetsTestSupport.h).
namespace rocks::engine_assets_test {

Result<void> RegisterFixtureTypes(ModuleRegistration& modules) noexcept {
    return generated::AssetTestFixtures::RegisterModuleTypes(modules);
}

namespace {

// 시험 서명 키의 seed 첫 byte다(seed는 이 값부터 1씩 늘어나는 32 byte).
constexpr uint8 TestSeedFirst = 0x21;

crypto::Ed25519SigningKey TestSigningKey() noexcept {
    uint8 seed[crypto::Ed25519SeedBytes];
    for (usize index = 0; index < crypto::Ed25519SeedBytes; ++index) {
        seed[index] = static_cast<uint8>(TestSeedFirst + index);
    }

    auto key = crypto::Ed25519SigningKey::TryFromSeed(Span<const uint8>(seed));
    return key ? Move(key.Value()) : crypto::Ed25519SigningKey();
}

Sha256Digest TestScope() noexcept {
    auto scope = MakeContentScope("rocks.engine-assets.test", "main");
    return scope ? scope.Value() : Sha256Digest{};
}

} // namespace

crypto::KeyId TestContentKeyId() noexcept {
    crypto::KeyId id;
    id.value.high = 0x5445535443534b31ull;
    id.value.low = 1;
    return id;
}

const ContentTrust& TestContentTrust() noexcept {
    // 정적 자리는 trivially destructible 값뿐이다(키 저장소를 쥔 서명 키는 쓸 때마다 만든다).
    static TrustedContentKey keys[1];
    static ContentTrust trust;
    static bool made = false;
    if (!made) {
        keys[0].id = TestContentKeyId();
        keys[0].key = TestSigningKey().PublicKey();
        trust.keys = Span<const TrustedContentKey>(keys);
        trust.minimumContentVersion = TestMinimumContentVersion;
        trust.scope = TestScope();
        made = true;
    }

    return trust;
}

bool SignTestManifest(Allocator& allocator, Span<const uint8> manifest, Array<uint8>& out,
                      uint64 version, const Sha256Digest* scope) noexcept {
    return WriteContentEnvelope(allocator, manifest, TestSigningKey(), TestContentKeyId(), version,
                                scope != nullptr ? *scope : TestScope(), out)
        .IsOk();
}

} // namespace rocks::engine_assets_test
