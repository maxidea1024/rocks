# First Foundation build

Run `python Build/build.py` on Windows x64 or macOS with native C++ tools, CMake, Ninja and Python. Windows discovers Visual Studio C++ tools/SDK and Ninja using vswhere; clang-cl must be on PATH. The local baseline is clang-cl 21.1.6. macOS uses clang++ and its native SDK. `CMakePresets.json` contains Debug/Release Windows and Debug/ASan macOS presets. Actual macOS results remain NOT RUN until native CI executes.

The build creates separate Base, Platform, Core, metadata, Task and Object targets, a CPU-only FoundationTests executable, and one translation unit per public header. Exceptions and RTTI are disabled. `ctest` runs Foundation, harness failure controls, the owned-source lexical STL guard and the module scaffold transaction. The STL guard is not a proof about external SDK internals or linked binaries.

`FoundationTests --list`, `--filter text`, `--seed N`, `--repeat N`, and `--json file` are supported. No matching tests, invalid arguments, test failures and report write failures return nonzero. CHECK records and continues; REQUIRE returns from the current void test function. Test names are globally unique. The seed is exposed to deterministic fixtures; the runner does not pretend to randomize test order.

`python Build/format.py --check` requires clang-format 21.1.6. Set ROCKS_CLANG_FORMAT to the pinned executable if necessary. Without --check it formats engine/test sources.

The C06 Task implementation is a single-owner synchronous executor. Submit reserves its completion observer record before consuming a caller's function. Cleanup runs directly outside locks and requires no second allocation or task submission; completion handles retain only the result. Handles and their allocator are confined to the creation thread, and the allocator must outlive every handle. Worker queues, dependencies, deferred lanes and cancellation are not implemented. This is the declared C06 subset, not the C17 scheduler.

Function is move-only and allocator-owned with one heap allocation; SBO is deferred. ModuleLease is loader-independent and protects only the lifetime of owner-provided code. LogBuffer is a fixed-capacity, single-owner bootstrap diagnostic capture with loss/truncation counts and ProfileScope records. It has no background writer or viewer. Assert failure uses bounded stack formatting and the native Platform fatal sink, bypassing C stdio locks. Native fatal termination is not certified as async-signal-safe. A child-process test holds the C stderr lock while another thread asserts.

The C00 scaffold supports only an ordinary module: `python Tools/Scaffold/rocks.py template plan module Example --output path --plan plan.json`, then `template apply --plan plan.json` and `template validate --plan plan.json`. The plan fixes the generated module ID and file digests; apply stages a new directory and rejects an existing target. No shell hooks run. Build integration is explicit. Full project/component/prefab/UI/material templates, linked updates, durable recovery journals and user catalogs remain planned.

The GitHub Actions workflow is configured for Windows/macOS and uploads test reports. A committed workflow is not evidence that hosted CI has run. First implementation does not claim Foundation completion, parallel GC, real asset loading, GPU/Studio integration or production build caching/distribution.

Core-only: `python Build/build.py --preset windows-clang-core` sets ROCKS_BUILD_OBJECT=OFF. Windows AddressSanitizer: `python Build/build.py --preset windows-clang-asan`; the launcher locates the matching compiler resource DLL for child tests. Windows ASan does not replace the explicit allocation/owner ledger or establish LeakSanitizer coverage.

## Mandatory compiler diagnostics and platform boundary

Every owned C++ target, including tests, generated code and standalone public headers, must treat warnings as errors: `/W4 /WX` for the MSVC driver family; `-Wall -Wextra -Wpedantic -Werror` for Clang/Apple Clang. Warning suppression or downgrading is prohibited. See `Docs/Design/CompilerDiagnostics.ko.md` for the normative policy. CTest `warning_policy` audits actual compile commands and proves both a clean compiler control and rejection of an intentional warning. The generated probe targets are excluded from the default build.

`rocks_localization` depends on Core, while Core/Object do not depend on Localization. Platform Thread/Mutex/CV/Event/Semaphore have common APIs; CMake selects native Windows/macOS implementation files. `platform_boundary_audit` and its negative controls reject known OS leakage into common code. Neither lexical audit proves arbitrary preprocessor expansion. Current native execution evidence is in `Docs/Implementation/FoundationStatus.ko.md`; historical totals do not cover newly edited source.
