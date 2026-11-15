#include <Core/Hash.h>
#include <Platform/Fatal.h>
#include <Platform/SecureRandom.h>

namespace rocks {
namespace {

HashSeed MakeProcessHashSeed() noexcept {
    uint64 words[2] = {0, 0};
    if (!platform::TryFillSecureRandom(words, sizeof(words))) {
        platform::Fatal("Process hash seed could not be read from the OS random source");
    }

    return HashSeed{words[0], words[1]};
}

} // namespace

HashSeed ProcessHashSeed() noexcept {
    // 처음 부를 때 한 번 정한다(함수 지역 static 초기화는 thread-safe다).
    static const HashSeed seed = MakeProcessHashSeed();
    return seed;
}

} // namespace rocks
