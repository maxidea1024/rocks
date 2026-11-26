#include <TestSupport/Test.h>
#include <Core/Memory/SystemAllocator.h>
#include <Core/Text/NamePool.h>
#include <Engine/Engine.h>
#include <World/Actor.generated.h>

// 엔진 이름 pool 시험이다(Docs/Design/EngineRuntime 31.2·31.7절 1): 엔진 Registry와 CreateWorld가
// 만든 World가 Engine::Names()를 쓰고(한쪽에서 intern한 Name을 다른 쪽 pool이 Acquire한다),
// WorldCreateDesc가 준 pool은 그대로 쓴다. 새 Engine의 pool은 앞 Engine의 토큰을 받지 않는다(옛
// 토큰 StaleHandle).
using namespace rocks;
namespace engine_name_pool_tests_file {

TEST(Engine_NamePoolSharedByRegistryAndWorlds) {
    SystemAllocator allocator;
    NamePool own(allocator);
    REQUIRE(own.Initialize());
    Name previous;
    for (uint32 round = 0; round < 2; ++round) {
        EngineConfig config;
        config.modules = &test::ProcessModules();
        Engine engine(allocator);
        REQUIRE(engine.Initialize(config));
        NamePool& names = engine.Names();
        CHECK(names.Stats().accepting);
        CHECK(engine.Objects().Names() == &names);

        auto world = engine.CreateWorld({});
        REQUIRE(world);
        CHECK(world.Value()->Objects().Names() == &names);
        auto enemy = world.Value()->Objects().Names()->TryIntern("Enemy");
        REQUIRE(enemy);
        auto same = engine.Objects().Names()->TryFind("Enemy");
        REQUIRE(same);
        CHECK(same.Value() == enemy.Value());
        {
            auto lease = names.Acquire(enemy.Value());
            CHECK(lease.IsOk());
        }

        // 앞 Engine pool의 토큰은 이 pool에서 옛 토큰이다.
        if (round != 0) {
            CHECK(names.Acquire(previous).Error() == ErrorCode::StaleHandle);
        }

        previous = enemy.Value();

        // 호출자가 준 pool은 그대로 쓴다. 그 pool의 토큰은 엔진 pool에서 Acquire되지 않는다.
        WorldCreateDesc desc;
        desc.objects.names = &own;
        auto custom = engine.CreateWorld(desc);
        REQUIRE(custom);
        CHECK(custom.Value()->Objects().Names() == &own);
        auto ownEnemy = own.TryIntern("Enemy");
        REQUIRE(ownEnemy);
        CHECK(names.Acquire(ownEnemy.Value()).Error() == ErrorCode::StaleHandle);

        REQUIRE(engine.Shutdown());
    }

    CHECK(own.Shutdown());
}

} // namespace engine_name_pool_tests_file
