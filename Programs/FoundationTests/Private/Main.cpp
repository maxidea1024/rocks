#include <TestSupport/Test.h>
#include <TestSupport/HostFailure.h>
#include <Platform/Clock.h>
#include <Platform/Sync.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>

namespace rocks::test {
int RunLoggerFailureProbe() noexcept;
int RunProcessProbe(int argc, char** argv) noexcept;
int RunHashTraversalNegativeControl(bool useSet, uint32 operation) noexcept;
int RunHashOperationsNegativeControl(uint32 operation) noexcept;
int RunStackAllocatorNegativeControl(uint32 operation) noexcept;
int RunMemoryAllocatorNegativeControl(uint32 operation) noexcept;
int RunBinnedAllocatorNegativeControl(uint32 operation) noexcept;
int RunBitArrayInvalidIndexControl() noexcept;
int RunFileNegativeControl(uint32 operation) noexcept;
int RunMathNegativeControl(uint32 operation) noexcept;
int RunOptionalEmptyValueControl() noexcept;
int RunUniquePtrEmptyControl() noexcept;
int RunHashMapIteratorAfterInsertControl() noexcept;
int RunSharedPtrEmptyControl() noexcept;
int RunSharedRefFromEmptyControl() noexcept;
int RunSparseArrayFreeIndexControl() noexcept;
int RunSlotArrayStaleHandleControl() noexcept;
int RunTimeSpanOverflowControl() noexcept;
int RunDateTimeOutOfRangeControl() noexcept;
int RunVariantWrongAlternativeControl() noexcept;
int RunDelegateRebindDuringTargetDestructionControl() noexcept;
int RunUniquePtrNullDeleterControl() noexcept;
int RunDelegateExecuteUnboundControl() noexcept;
int RunDelegateRebindDuringExecutionControl() noexcept;
int RunSpanSliceOutOfBoundsControl() noexcept;
int RunRandomStreamPreconditionControl(uint32 operation) noexcept;
int RunExternalAllocatorNegativeControl(uint32 operation) noexcept;
int RunDebugAllocatorNegativeControl(uint32 operation) noexcept;
int RunVirtualMemoryAccessProbe(uint32 state, bool write) noexcept;
int RunSecretArenaGuardProbe(bool after) noexcept;
int RunSecretSlotUseAfterFreeProbe() noexcept;
int RunSecretSlotTailOverflowProbe() noexcept;
int RunNumberCorpus(const char* path) noexcept;
static TestCase* first = nullptr;
static uint64 failures = 0;
static platform::Mutex failureMutex;
static uint64 seed = 1;
static int (*objectScopeNegativeControl)(bool) noexcept = nullptr;
static int (*destroyRequestedNegativeControl)(bool) noexcept = nullptr;
static int (*typeConvenienceNegativeControl)(uint32) noexcept = nullptr;

// Object 테스트가 포함된 타깃만 음성 대조 callback을 등록한다. Core-only는 링크 의존이 없다.
void RegisterObjectScopeNegativeControl(int (*function)(bool) noexcept) noexcept {
    objectScopeNegativeControl = function;
}

void RegisterDestroyRequestedNegativeControl(int (*function)(bool) noexcept) noexcept {
    destroyRequestedNegativeControl = function;
}

void RegisterTypeConvenienceNegativeControl(int (*function)(uint32) noexcept) noexcept {
    typeConvenienceNegativeControl = function;
}

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
    test::PrepareAutomatedTestProcess();
    // Process 시험이 이 실행 파일을 자식으로 다시 띄운 경우다(PlatformBase/Tests/ProcessTests.cpp).
    if (argc >= 3 && strcmp(argv[1], "--process-probe") == 0) {
        return test::RunProcessProbe(argc, argv);
    }

    const char* filter = "";
    const char* jsonPath = nullptr;
    uint64 repeat = 1;
    bool list = false;
    for (int i = 1; i < argc; ++i) {
        if (strcmp(argv[i], "--logger-failure-probe") == 0 && argc == 2) {
            return test::RunLoggerFailureProbe();
        } else if (strcmp(argv[i], "--number-corpus") == 0 && argc == 3) {
            // .NET 대조 말뭉치(도구 시험 Native.NumberConversionMatchesDotNet이 만든다).
            return test::RunNumberCorpus(argv[2]);
        } else if (strcmp(argv[i], "--list") == 0) {
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
            } else if (strcmp(control, "thread-unjoined") == 0) {
                return test::RunUnjoinedThreadNegativeControl();
            } else if (strcmp(control, "thread-unjoined-start-failure") == 0) {
                return test::RunUnjoinedThreadNegativeControl(1);
            } else if (strcmp(control, "thread-unjoined-wait-failure") == 0) {
                return test::RunUnjoinedThreadNegativeControl(2);
            } else if (strcmp(control, "name-live-lease") == 0) {
                return test::RunOutstandingNameLeaseNegativeControl();
            } else if (strcmp(control, "object-scope-lifo") == 0 ||
                       strcmp(control, "object-scope-thread") == 0) {
                if (test::objectScopeNegativeControl == nullptr) {
                    return 2;
                }

                return test::objectScopeNegativeControl(strcmp(control, "object-scope-thread") ==
                                                        0);
            } else if (strcmp(control, "object-status-stale") == 0 ||
                       strcmp(control, "object-status-context") == 0) {
                if (test::destroyRequestedNegativeControl == nullptr) {
                    return 2;
                }

                return test::destroyRequestedNegativeControl(
                    strcmp(control, "object-status-context") == 0);
            } else if (strcmp(control, "object-type-stale") == 0 ||
                       strcmp(control, "object-type-context") == 0 ||
                       strcmp(control, "empty-type-isa") == 0) {
                if (test::typeConvenienceNegativeControl == nullptr) {
                    return 2;
                }

                const uint32 operation = strcmp(control, "object-type-stale") == 0     ? 0u
                                         : strcmp(control, "object-type-context") == 0 ? 1u
                                                                                       : 2u;
                return test::typeConvenienceNegativeControl(operation);
            } else if (strcmp(control, "hash-map-foreach-erase") == 0 ||
                       strcmp(control, "hash-map-foreach-clear") == 0 ||
                       strcmp(control, "hash-map-foreach-reset") == 0 ||
                       strcmp(control, "hash-set-foreach-erase") == 0 ||
                       strcmp(control, "hash-set-foreach-clear") == 0 ||
                       strcmp(control, "hash-set-foreach-reset") == 0) {
                const bool useSet = strncmp(control, "hash-set-", 9) == 0;
                const uint32 operation = strstr(control, "-erase") != nullptr   ? 0u
                                         : strstr(control, "-clear") != nullptr ? 1u
                                                                                : 2u;
                return test::RunHashTraversalNegativeControl(useSet, operation);
            } else if (strcmp(control, "hash-map-eraseif-clear") == 0 ||
                       strcmp(control, "hash-set-eraseif-clear") == 0 ||
                       strcmp(control, "hash-map-factory-clear") == 0 ||
                       strcmp(control, "hash-set-union-erase") == 0) {
                const uint32 operation = strcmp(control, "hash-map-eraseif-clear") == 0   ? 0u
                                         : strcmp(control, "hash-set-eraseif-clear") == 0 ? 1u
                                         : strcmp(control, "hash-map-factory-clear") == 0 ? 2u
                                                                                          : 3u;
                return test::RunHashOperationsNegativeControl(operation);
            } else if (strcmp(control, "allocator-arena-live") == 0 ||
                       strcmp(control, "allocator-pool-live") == 0 ||
                       strcmp(control, "allocator-budget-live") == 0) {
                const uint32 operation = strcmp(control, "allocator-arena-live") == 0  ? 0u
                                         : strcmp(control, "allocator-pool-live") == 0 ? 1u
                                                                                       : 2u;
                return test::RunMemoryAllocatorNegativeControl(operation);
            } else if (strcmp(control, "allocator-external-overlap") == 0 ||
                       strcmp(control, "allocator-external-overflow") == 0 ||
                       strcmp(control, "allocator-external-metadata") == 0 ||
                       strcmp(control, "allocator-external-live") == 0) {
                const uint32 operation = strcmp(control, "allocator-external-overlap") == 0    ? 0u
                                         : strcmp(control, "allocator-external-overflow") == 0 ? 1u
                                         : strcmp(control, "allocator-external-metadata") == 0 ? 2u
                                                                                               : 3u;
                return test::RunExternalAllocatorNegativeControl(operation);
            } else if (strcmp(control, "allocator-debug-front") == 0 ||
                       strcmp(control, "allocator-debug-back") == 0 ||
                       strcmp(control, "allocator-debug-after-free") == 0 ||
                       strcmp(control, "allocator-debug-live") == 0) {
                const uint32 operation = strcmp(control, "allocator-debug-front") == 0        ? 0u
                                         : strcmp(control, "allocator-debug-back") == 0       ? 1u
                                         : strcmp(control, "allocator-debug-after-free") == 0 ? 2u
                                                                                              : 3u;
                return test::RunDebugAllocatorNegativeControl(operation);
            } else if (strcmp(control, "bit-array-invalid-index") == 0) {
                return test::RunBitArrayInvalidIndexControl();
            } else if (strncmp(control, "allocator-binned-invalid-", 25) == 0 &&
                       control[25] >= '0' && control[25] <= '6' && control[26] == '\0') {
                return test::RunBinnedAllocatorNegativeControl(
                    static_cast<uint32>(control[25] - '0'));
            } else if (strncmp(control, "platform-file-invalid-", 22) == 0 && control[22] >= '0' &&
                       control[22] <= '7' && control[23] == '\0') {
                return test::RunFileNegativeControl(static_cast<uint32>(control[22] - '0'));
            } else if (strncmp(control, "math-invalid-", 13) == 0 && control[13] >= '0' &&
                       control[13] <= '7' && control[14] == '\0') {
                return test::RunMathNegativeControl(static_cast<uint32>(control[13] - '0'));
            } else if (strcmp(control, "optional-empty-value") == 0) {
                return test::RunOptionalEmptyValueControl();
            } else if (strcmp(control, "unique-ptr-empty") == 0) {
                return test::RunUniquePtrEmptyControl();
            } else if (strcmp(control, "hash-map-iterator-modified") == 0) {
                return test::RunHashMapIteratorAfterInsertControl();
            } else if (strcmp(control, "shared-ptr-empty") == 0) {
                return test::RunSharedPtrEmptyControl();
            } else if (strcmp(control, "shared-ref-from-empty") == 0) {
                return test::RunSharedRefFromEmptyControl();
            } else if (strcmp(control, "sparse-array-free-index") == 0) {
                return test::RunSparseArrayFreeIndexControl();
            } else if (strcmp(control, "slot-array-stale-handle") == 0) {
                return test::RunSlotArrayStaleHandleControl();
            } else if (strcmp(control, "time-span-overflow") == 0) {
                return test::RunTimeSpanOverflowControl();
            } else if (strcmp(control, "date-time-out-of-range") == 0) {
                return test::RunDateTimeOutOfRangeControl();
            } else if (strcmp(control, "variant-wrong-alternative") == 0) {
                return test::RunVariantWrongAlternativeControl();
            } else if (strcmp(control, "delegate-rebind-during-target-destruction") == 0) {
                return test::RunDelegateRebindDuringTargetDestructionControl();
            } else if (strcmp(control, "unique-ptr-null-deleter") == 0) {
                return test::RunUniquePtrNullDeleterControl();
            } else if (strcmp(control, "delegate-execute-unbound") == 0) {
                return test::RunDelegateExecuteUnboundControl();
            } else if (strcmp(control, "delegate-rebind-during-execution") == 0) {
                return test::RunDelegateRebindDuringExecutionControl();
            } else if (strcmp(control, "span-slice-out-of-bounds") == 0) {
                return test::RunSpanSliceOutOfBoundsControl();
            } else if (strncmp(control, "random-invalid-", 15) == 0 && control[15] >= '0' &&
                       control[15] <= '2' && control[16] == '\0') {
                return test::RunRandomStreamPreconditionControl(
                    static_cast<uint32>(control[15] - '0'));
            } else if (strcmp(control, "allocator-stack-live") == 0 ||
                       strcmp(control, "allocator-stack-wrong-thread") == 0 ||
                       strcmp(control, "allocator-stack-array-growth") == 0) {
                const uint32 operation = strcmp(control, "allocator-stack-live") == 0 ? 0u
                                         : strcmp(control, "allocator-stack-wrong-thread") == 0
                                             ? 1u
                                             : 2u;
                return test::RunStackAllocatorNegativeControl(operation);
            } else if (strcmp(control, "vm-reserved-read") == 0 ||
                       strcmp(control, "vm-reserved-write") == 0 ||
                       strcmp(control, "vm-decommitted-read") == 0 ||
                       strcmp(control, "vm-decommitted-write") == 0 ||
                       strcmp(control, "vm-committed-read") == 0 ||
                       strcmp(control, "vm-committed-write") == 0) {
                const uint32 state = strncmp(control, "vm-reserved-", 12) == 0      ? 0u
                                     : strncmp(control, "vm-decommitted-", 15) == 0 ? 1u
                                                                                    : 2u;
                return test::RunVirtualMemoryAccessProbe(state,
                                                         strstr(control, "-write") != nullptr);
            } else if (strcmp(control, "secret-arena-guard-before") == 0 ||
                       strcmp(control, "secret-arena-guard-after") == 0) {
                return test::RunSecretArenaGuardProbe(strcmp(control, "secret-arena-guard-after") ==
                                                      0);
            } else if (strcmp(control, "secret-slot-use-after-free") == 0) {
                return test::RunSecretSlotUseAfterFreeProbe();
            } else if (strcmp(control, "secret-slot-tail-overflow") == 0) {
                return test::RunSecretSlotTailOverflowProbe();
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

    FILE* json = jsonPath == nullptr ? nullptr : test::OpenReportFile(jsonPath);
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
