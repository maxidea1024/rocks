#pragma once

#include <Base/Types.h>

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
void RunParallelNegativeControl() noexcept;
int RunAssertWithLockedStderr() noexcept;
int RunAsanNegativeControl() noexcept;
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
