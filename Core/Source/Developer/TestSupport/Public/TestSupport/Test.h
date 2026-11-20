#pragma once

#include <Base/Types.h>

namespace rocks {

class ModuleLoader;

} // namespace rocks

namespace rocks::test {

struct TestCase {
    const char* name;
    void (*function)();
    TestCase* next;
};

class Registration {
public:
    Registration(const char*, void (*)()) noexcept;

private:
    TestCase test_;
};

void RecordFailure(const char* expression, const char* file, int line) noexcept;
uint64 CurrentSeed() noexcept;
// 시험 실행기의 프로세스 모듈 로더다(Launch가 소유 — Docs/Design/ModuleLoader.ko.md 3.11절). 시험
// 실행기 본문이 정의하는 시험 전용 접근점이다(제품 코드의 전역 접근자가 아니다). 엔진을 만드는
// 시험은 EngineConfig::modules에 넣는다. 그래서 차례로 만드는 Engine마다 같은 로더의 엔진 묶음을
// 시작·종료한다. 시험 스레드(주 스레드)에서만 쓴다. 실행기 본문 밖에서 부르면 Fatal이다.
//
// 사용 예: config.modules = &test::ProcessModules();
ModuleLoader& ProcessModules() noexcept;
void RunParallelNegativeControl() noexcept;
int RunAssertWithLockedStderr() noexcept;
int RunAsanNegativeControl() noexcept;
int RunUnjoinedThreadNegativeControl(uint32 scenario = 0) noexcept;
int RunOutstandingNameLeaseNegativeControl() noexcept;
} // namespace rocks::test

#define TEST(name)                                                                                 \
    static void RocksTest_##name();                                                                \
    static ::rocks::test::Registration RocksRegistration_##name(#name, &RocksTest_##name);         \
    static void RocksTest_##name()

#define CHECK(expression)                                                                          \
    do {                                                                                           \
        if (!(expression)) {                                                                       \
            ::rocks::test::RecordFailure(#expression, __FILE__, __LINE__);                         \
        }                                                                                          \
    } while (false)

#define REQUIRE(expression)                                                                        \
    do {                                                                                           \
        if (!(expression)) {                                                                       \
            ::rocks::test::RecordFailure(#expression, __FILE__, __LINE__);                         \
            return;                                                                                \
        }                                                                                          \
    } while (false)
