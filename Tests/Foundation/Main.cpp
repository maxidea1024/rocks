#include "Tests/Support/Test.h"
#include <Platform/Clock.h>
#include <Platform/Sync.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>

namespace rocks::test {
static TestCase* first = nullptr;
static uint64 failures = 0;
static platform::Mutex failureMutex;
static uint64 seed = 1;

Registration::Registration(const char* name, void (*function)()) noexcept
    : test_{name, function, first} {
    first = &test_;
}

void RecordFailure(const char* expression, const char* file, int line) noexcept {
    platform::LockGuard lock(failureMutex);
    ++failures;
    fprintf(stderr, "CHECK FAILED: %s (%s:%d)\n", expression, file, line);
}

uint64 CurrentSeed() noexcept {
    return seed;
}
} // namespace rocks::test

static bool ParseUnsigned(const char* text, rocks::uint64& value) {
    if (text == nullptr || text[0] == '\0' || text[0] == '-') {
        return false;
    }
    errno = 0;
    char* end = nullptr;
    const unsigned long long result = strtoull(text, &end, 10);
    if (errno != 0 || end == text || *end != '\0') {
        return false;
    }
    value = static_cast<rocks::uint64>(result);
    return true;
}

static bool requireReturned = false;

static void RequireNegativeControl() {
    REQUIRE(false);
    requireReturned = true;
}

int main(int argc, char** argv) {
    using namespace rocks;
    const char* filter = "";
    const char* jsonPath = nullptr;
    uint64 repeat = 1;
    bool list = false;
    for (int i = 1; i < argc; ++i) {
        if (strcmp(argv[i], "--list") == 0) {
            list = true;
        } else if (strcmp(argv[i], "--filter") == 0 && i + 1 < argc) {
            filter = argv[++i];
        } else if (strcmp(argv[i], "--seed") == 0 && i + 1 < argc) {
            if (!ParseUnsigned(argv[++i], test::seed)) {
                return 2;
            }
        } else if (strcmp(argv[i], "--repeat") == 0 && i + 1 < argc) {
            if (!ParseUnsigned(argv[++i], repeat) || repeat == 0 || repeat > 1000000) {
                return 2;
            }
        } else if (strcmp(argv[i], "--json") == 0 && i + 1 < argc) {
            jsonPath = argv[++i];
        } else if (strcmp(argv[i], "--negative-control") == 0 && i + 1 < argc) {
            const char* control = argv[++i];
            if (strcmp(control, "check") == 0) {
                CHECK(false);
            } else if (strcmp(control, "require") == 0) {
                RequireNegativeControl();
                if (requireReturned) {
                    return 3;
                }
            } else if (strcmp(control, "parallel") == 0) {
                test::RunParallelNegativeControl();
                return test::failures == 200 ? 1 : 3;
            } else if (strcmp(control, "asan-uaf") == 0) {
                return test::RunAsanNegativeControl();
            } else if (strcmp(control, "assert-stderr-lock") == 0) {
                return test::RunAssertWithLockedStderr();
            } else {
                return 2;
            }
            return test::failures == 1 ? 1 : 3;
        } else {
            fprintf(stderr, "Usage: FoundationTests [--list] [--filter text] [--seed N] "
                            "[--repeat N] [--json path]\n");
            return 2;
        }
    }

    test::TestCase* cases[4096]{};
    usize count = 0;
    for (test::TestCase* test = test::first; test != nullptr; test = test->next) {
        if (strstr(test->name, filter) == nullptr) {
            continue;
        }
        if (count == 4096) {
            fprintf(stderr, "Test registry capacity exceeded\n");
            return 2;
        }
        usize position = count;
        while (position > 0 && strcmp(cases[position - 1]->name, test->name) > 0) {
            cases[position] = cases[position - 1];
            --position;
        }
        cases[position] = test;
        ++count;
    }
    if (count == 0) {
        fprintf(stderr, "No tests matched filter\n");
        return 2;
    }
    for (usize i = 1; i < count; ++i) {
        if (strcmp(cases[i - 1]->name, cases[i]->name) == 0) {
            fprintf(stderr, "Duplicate test name: %s\n", cases[i]->name);
            return 2;
        }
    }
    if (list) {
        for (usize i = 0; i < count; ++i) {
            puts(cases[i]->name);
        }
        return 0;
    }

    FILE* json = jsonPath == nullptr ? nullptr : fopen(jsonPath, "wb");
    if (jsonPath != nullptr && json == nullptr) {
        fprintf(stderr, "Cannot open JSON report\n");
        return 2;
    }
    if (json != nullptr) {
        fprintf(json, "{\"schema\":1,\"seed\":%llu,\"repeat\":%llu,\"tests\":[",
                static_cast<unsigned long long>(test::seed),
                static_cast<unsigned long long>(repeat));
    }
    uint64 failedCases = 0;
    for (usize i = 0; i < count; ++i) {
        const uint64 before = test::failures;
        const uint64 start = platform::MonotonicNanoseconds();
        for (uint64 iteration = 0; iteration < repeat; ++iteration) {
            cases[i]->function();
        }
        const uint64 elapsed = platform::MonotonicNanoseconds() - start;
        const uint64 caseFailures = test::failures - before;
        if (caseFailures != 0) {
            ++failedCases;
        }
        printf("[%s] %s (%llu ns)\n", caseFailures == 0 ? "PASS" : "FAIL", cases[i]->name,
               static_cast<unsigned long long>(elapsed));
        if (json != nullptr) {
            fprintf(json,
                    "%s{\"name\":\"%s\",\"status\":\"%s\",\"failures\":%llu,"
                    "\"duration_ns\":%llu}",
                    i == 0 ? "" : ",", cases[i]->name, caseFailures == 0 ? "PASS" : "FAIL",
                    static_cast<unsigned long long>(caseFailures),
                    static_cast<unsigned long long>(elapsed));
        }
    }
    if (json != nullptr) {
        fprintf(json, "],\"failed_tests\":%llu,\"failures\":%llu}\n",
                static_cast<unsigned long long>(failedCases),
                static_cast<unsigned long long>(test::failures));
        const bool writeFailed = ferror(json) != 0;
        const int closeResult = fclose(json);
        if (writeFailed || closeResult != 0) {
            fprintf(stderr, "JSON report write failed\n");
            return 2;
        }
    }
    printf("%llu tests, %llu iterations each, %llu failed tests, seed %llu\n",
           static_cast<unsigned long long>(count), static_cast<unsigned long long>(repeat),
           static_cast<unsigned long long>(failedCases),
           static_cast<unsigned long long>(test::seed));
    return test::failures == 0 ? 0 : 1;
}
