#include <ReflectionTestFixtures/ArrayFixture.h>
#include <ReflectionTestFixtures/ReflectionFixture.h>
#include <ReflectionTestFixtures/StructFixture.h>
#include <ReflectionTestFixtures/ValidateFixture.h>

// 시험 fixture의 관찰용 정적 저장소다. 공개 헤더에 쓰기 가능한 정적 저장소·thread_local을 두지
// 않으므로 (Docs/Design/BuildTargets.ko.md 3.8절 BD29) 선언은 헤더에, 정의는 여기에 둔다.
namespace rocks::reflection_fixture {

uint32 ArrayOwner::constructed = 0;

bool Node::destroyedWithOldLabel = false;

bool Node::destroyedWithNewLabel = false;

uint32 AbstractBase::constructions = 0;

uint32 AbstractByPolicy::constructions = 0;

AssetId AssetFields::destroyedIcon{};

char AssetFields::destroyedIconPath[AssetFields::DestroyedIconPathCapacity] = {};

usize AssetFields::destroyedIconPathBytes = 0;

uint32 StructLeaf::migrations = 0;

uint32 StructOwner::constructed = 0;

ValidateLog& CurrentValidateLog() noexcept {
    static thread_local ValidateLog log;
    return log;
}

} // namespace rocks::reflection_fixture
