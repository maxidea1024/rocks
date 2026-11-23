#include <TestSupport/Test.h>
#include <TestSupport/HostFailure.h>
#include <Core/App/ProcessCommandLine.h>
#include <Core/Module/ModuleEntry.h>
#include <Launch/EntryPoint.h>
#include <Platform/Clock.h>
#include <Platform/Fatal.h>
#include <Platform/Sync.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>

namespace rocks::test {
int RunLoggerFailureProbe() noexcept;
int RunLogFatalProbe(uint32 mode) noexcept;
int RunLogCrashProbe(uint32 mode, const char* directory) noexcept;
int RunCrashProbe(const char* mode, const char* directory) noexcept;
void ConfigureCrashProbe(const char* mode, const char* directory,
                         launch::ApplicationSettings& settings) noexcept;
void ConfigureLogCrashProbe(const char* directory, launch::ApplicationSettings& settings) noexcept;
int RunProcessProbe(int argc, const char* const* argv) noexcept;
int RunHashTraversalNegativeControl(bool useSet, uint32 operation) noexcept;
int RunHashOperationsNegativeControl(uint32 operation) noexcept;
int RunOrderedMapNegativeControl(uint32 operation) noexcept;
int RunOrderedSetNegativeControl() noexcept;
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
int RunModuleLoaderShutdownDuringLoadControl() noexcept;
int RunModuleLoaderPhaseMisuseControl(uint32 operation) noexcept;
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
// Launch가 소유한 프로세스 모듈 로더다(RunRunner가 본문 동안 건다).
static ModuleLoader* processModules = nullptr;
static int (*objectScopeNegativeControl)(bool) noexcept = nullptr;
static int (*destroyRequestedNegativeControl)(bool) noexcept = nullptr;
static int (*typeConvenienceNegativeControl)(uint32) noexcept = nullptr;
static int (*objectObserverNegativeControl)(uint32) noexcept = nullptr;
static int (*requiredCheckNegativeControl)(uint32) noexcept = nullptr;

// 상위 계층 시험(엔진 모듈)이 등록하는 추가 process probe 모드다. Core 계층
// probe(RunProcessProbe)는 엔진 모듈을 모르므로 엔진 시험은 자기 probe를 이름으로 등록한다.
struct RegisteredProcessProbe {
    const char* mode = nullptr;
    int (*function)(int, const char* const*) noexcept = nullptr;
};

// 등록 자리 수다. 넘치면 등록이 false이고 그 probe를 쓰는 시험의 "…ProbeRegistered" 검사가
// 실패한다. 실행 파일 하나에 등록되는 probe 수(EngineTests는 에셋 로더 probe를 더해 10개를
// 넘는다)보다 넉넉히 둔다 — 8일 때 서로 다른 세션이 하나씩 더한 probe가 병합에서 만나 넘쳤다.
static constexpr usize MaxRegisteredProcessProbes = 32;
static RegisteredProcessProbe registeredProcessProbes[MaxRegisteredProcessProbes];
static usize registeredProcessProbeCount = 0;

// "--process-probe <mode> ..."의 mode를 등록한다(정적 초기화에서 부른다). 자리가 없으면 false다.
bool RegisterProcessProbe(const char* mode,
                          int (*function)(int, const char* const*) noexcept) noexcept {
    if (registeredProcessProbeCount == MaxRegisteredProcessProbes) {
        return false;
    }

    registeredProcessProbes[registeredProcessProbeCount++] = {mode, function};
    return true;
}

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

void RegisterObjectObserverNegativeControl(int (*function)(uint32) noexcept) noexcept {
    objectObserverNegativeControl = function;
}

void RegisterRequiredCheckNegativeControl(int (*function)(uint32) noexcept) noexcept {
    requiredCheckNegativeControl = function;
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

ModuleLoader& ProcessModules() noexcept {
    if (processModules == nullptr) {
        platform::Fatal("test::ProcessModules called outside the test runner body");
    }

    return *processModules;
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

// 크래시 probe 모드 접두사다(`--negative-control crash-<mode>`, CrashProcessProbe.cpp).
static constexpr char CrashProbePrefix[] = "crash-";
static constexpr rocks::usize CrashProbePrefixBytes = sizeof(CrashProbePrefix) - 1;

static void RequireNegativeControl() {
    REQUIRE(false);
    requireReturned = true;
}

// 시험 실행기 본문이다. argv 모양(0번은 실행 파일)은 Launch가 캡처한 UTF-8 인자를 가리킨다.
static int RunTestRunner(int argc, const char* const* argv) {
    using namespace rocks;
    // Process 시험이 이 실행 파일을 자식으로 다시 띄운 경우다(PlatformBase/Tests/ProcessTests.cpp).
    if (argc >= 3 && strcmp(argv[1], "--process-probe") == 0) {
        for (usize index = 0; index < test::registeredProcessProbeCount; ++index) {
            if (strcmp(argv[2], test::registeredProcessProbes[index].mode) == 0) {
                return test::registeredProcessProbes[index].function(argc, argv);
            }
        }

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
        } else if (strcmp(argv[i], "--crash-dir") == 0 && i + 1 < argc) {
            // 크래시 폴더 뿌리다. Launch 설정(ConfigureRunner)이 이미 썼다.
            ++i;
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
            } else if (strcmp(control, "object-observer-unpolicied") == 0 ||
                       strcmp(control, "object-observer-shared") == 0 ||
                       strcmp(control, "object-observer-attached") == 0) {
                if (test::objectObserverNegativeControl == nullptr) {
                    return 2;
                }

                const uint32 operation = strcmp(control, "object-observer-unpolicied") == 0 ? 0u
                                         : strcmp(control, "object-observer-shared") == 0   ? 1u
                                                                                            : 2u;
                return test::objectObserverNegativeControl(operation);
            } else if (strcmp(control, "required-check-rollback") == 0 ||
                       strcmp(control, "required-check-twice") == 0) {
                if (test::requiredCheckNegativeControl == nullptr) {
                    return 2;
                }

                return test::requiredCheckNegativeControl(
                    strcmp(control, "required-check-rollback") == 0 ? 0u : 1u);
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
            } else if (strcmp(control, "ordered-map-iterator-modified") == 0) {
                return test::RunOrderedMapNegativeControl(0);
            } else if (strcmp(control, "ordered-map-foreach-erase") == 0) {
                return test::RunOrderedMapNegativeControl(1);
            } else if (strcmp(control, "ordered-set-iterator-modified") == 0) {
                return test::RunOrderedSetNegativeControl();
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
            } else if (strcmp(control, "module-loader-shutdown-during-load") == 0) {
                return test::RunModuleLoaderShutdownDuringLoadControl();
            } else if (strncmp(control, "module-loader-phase-misuse-", 27) == 0 &&
                       control[27] >= '0' && control[27] <= '2' && control[28] == '\0') {
                return test::RunModuleLoaderPhaseMisuseControl(
                    static_cast<uint32>(control[27] - '0'));
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
            } else if (strcmp(control, "log-fatal-sync") == 0) {
                return test::RunLogFatalProbe(0);
            } else if (strcmp(control, "log-fatal-async") == 0) {
                return test::RunLogFatalProbe(1);
            } else if (strcmp(control, "log-assert") == 0) {
                return test::RunLogFatalProbe(2);
            } else if (strcmp(control, "log-fatal-recursive") == 0) {
                return test::RunLogFatalProbe(3);
            } else if (strcmp(control, "log-crash-platform-fatal") == 0) {
                return test::RunLogCrashProbe(0, i + 1 < argc ? argv[i + 1] : nullptr);
            } else if (strcmp(control, "log-crash-access-violation") == 0) {
                return test::RunLogCrashProbe(1, i + 1 < argc ? argv[i + 1] : nullptr);
            } else if (strcmp(control, "log-crash-stack-overflow") == 0) {
                return test::RunLogCrashProbe(2, i + 1 < argc ? argv[i + 1] : nullptr);
            } else if (strcmp(control, "log-crash-stuck-sink") == 0) {
                return test::RunLogCrashProbe(3, i + 1 < argc ? argv[i + 1] : nullptr);
            } else if (strncmp(control, CrashProbePrefix, CrashProbePrefixBytes) == 0) {
                // 크래시 수집기 자식 probe(CrashProcessProbe.cpp)다.
                return test::RunCrashProbe(control + CrashProbePrefixBytes,
                                           i + 1 < argc ? argv[i + 1] : nullptr);
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
                            "[--repeat N] [--json path] [--crash-dir path]\n");
            return 2;
        }
    }

    // 한 번에 고를 수 있는 시험 수의 상한이다. 시험 본문이 이 스레드 스택을 쓰므로 표는 정적
    // 저장소에 둔다.
    constexpr usize MaxRunnableTests = 16384;
    static test::TestCase* cases[MaxRunnableTests]{};
    usize count = 0;
    for (test::TestCase* test = test::first; test != nullptr; test = test->next) {
        if (strstr(test->name, filter) == nullptr) {
            continue;
        }

        if (count == MaxRunnableTests) {
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
        // 결과 줄마다 stdout을 비운다. 다음 시험이 Fatal로 죽으면 stdout 버퍼가 비워지지 않아
        // 마지막 PASS 줄이 잘린다(파이프로 받는 CI 로그는 줄 단위로 비우지 않는다) — 죽은 시험을
        // 찾기 어렵다.
        fflush(stdout);
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

// 덤프 도우미 시험 장치(ROCKS_CRASH_PROBE_HELPER, 설계 17절)다. Launch 진입점은 다른 무엇보다 먼저
// 도우미 모드로 분기하므로 정적 초기화에서 건다. 환경 변수가 없으면 아무것도 하지 않는다.
[[maybe_unused]] static const bool crashHelperTestMode = rocks::test::InstallCrashHelperTestMode();

namespace {

using rocks::Span;
using rocks::StringView;
namespace launch = rocks::launch;

// 실행 파일 이름이다. 시험 실행 파일 모듈 이름이 실행 파일 이름이므로 빌드가 정의하는 모듈 이름을
// 쓴다(같은 소스가 CoreFoundationTests·FoundationTests·EngineTests·EngineModularTests가 된다). 크래시
// 폴더 이름과 메타데이터 "program"이고, 하니스는 실행 파일 이름으로 찾는다.
constexpr const char RunnerName[] = ROCKS_MODULE_NAME_TEXT;

bool Is(StringView argument, const char* text) noexcept {
    return argument == StringView(text);
}

// 실행기 설정이다(설계 16절). 실행기 본체는 Full이고 폴더는 --crash-dir이 있으면 그것이다. 이
// 실행 파일을 다시 띄운 자식 probe는 크래시 수집을 명시적으로 고른다. 크래시 probe(crash-*)와 로그
// 크래시 probe(log-crash-*)는 probe가 정한 설정이고, 그 밖의 probe(--process-probe,
// --logger-failure-probe, 다른 --negative-control)는 Off다(의도적 Fatal·assert는 수집기 없이 비상
// 출력 뒤 끝나는 기존 계약을 시험한다). 인자 해석은 RunTestRunner와 같다: --process-probe는 첫
// 인자일 때만, --logger-failure-probe는 유일한 인자일 때만이고, 값을 받는 옵션(--filter·--seed·
// --repeat·--json·--crash-dir)의 값은 옵션으로 보지 않는다.
void ConfigureRunner(Span<const StringView> arguments,
                     launch::ApplicationSettings& settings) noexcept {
    if ((arguments.Size() >= 2 && Is(arguments[0], "--process-probe")) ||
        (arguments.Size() == 1 && Is(arguments[0], "--logger-failure-probe"))) {
        settings.crashReporting = launch::CrashReportingMode::Off;
        return;
    }

    for (rocks::usize index = 0; index < arguments.Size(); ++index) {
        const StringView argument = arguments[index];
        const char* next = index + 1 < arguments.Size() ? arguments[index + 1].Data() : nullptr;
        if (next == nullptr) {
            return;
        }

        if (Is(argument, "--crash-dir")) {
            // Launch 인자의 Data()는 NUL로 끝나고 프로세스 수명 동안 유효하다.
            settings.crashDirectory = next;
            ++index;
        } else if (Is(argument, "--filter") || Is(argument, "--seed") || Is(argument, "--repeat") ||
                   Is(argument, "--json")) {
            ++index;
        } else if (Is(argument, "--negative-control")) {
            const StringView control(next);
            const char* directory =
                index + 2 < arguments.Size() ? arguments[index + 2].Data() : nullptr;
            if (control.StartsWith(CrashProbePrefix, rocks::StringComparison::CaseSensitive)) {
                rocks::test::ConfigureCrashProbe(next + CrashProbePrefixBytes, directory, settings);
            } else if (control.StartsWith("log-crash-", rocks::StringComparison::CaseSensitive)) {
                rocks::test::ConfigureLogCrashProbe(directory, settings);
            } else {
                settings.crashReporting = launch::CrashReportingMode::Off;
            }

            return;
        }
    }
}

int RunRunner(launch::ApplicationContext& context) noexcept {
    static const char* argv[rocks::ProcessCommandLine::MaxArguments + 1];
    const Span<const StringView> arguments = context.Arguments();
    argv[0] = context.ExecutablePath().Data();
    for (rocks::usize index = 0; index < arguments.Size(); ++index) {
        argv[index + 1] = arguments[index].Data();
    }

    rocks::test::processModules = &context.Modules();
    const int exitCode = RunTestRunner(static_cast<int>(arguments.Size() + 1), argv);
    rocks::test::processModules = nullptr;
    return exitCode;
}

const launch::ApplicationDescription& Describe() noexcept {
    static constexpr launch::ApplicationDescription Description{
        .name = RunnerName,
        .kind = launch::ApplicationKind::TestRunner,
        .configure = &ConfigureRunner,
        .run = &RunRunner};
    return Description;
}

} // namespace

ROCKS_IMPLEMENT_APPLICATION(Describe)
