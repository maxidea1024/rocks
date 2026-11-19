#include <Launch/Private/BuildIdentity.h>
#include <Launch/Private/LaunchStages.h>
#include <Launch/Private/TerminationRequest.h>

#include <Core/Text/StringBuilder.h>
#include <Core/Build/BuildInfo.h>
#include <Launch/Application.h>
#include <TestSupport/Test.h>

#include <string.h>

using namespace rocks;
using namespace rocks::launch;

// Launch 시작 단계 실행기(순서·역순 정리·실패 처리)와 빌드 식별 조각을 검사한다. 진입점·크래시
// 수집, 명령줄, 도구 로그를 실제로 거치는 흐름은 시험 실행 파일(Programs/LaunchTests)을 자식으로
// 띄워 검사한다(Native.Launch.*).
namespace {

constexpr usize MaxEvents = 32;
constexpr int BodyExitCode = 17;
constexpr int AbortExitCode = 41;

// 단계 시작·정리·본문·단계 표시를 순서대로 남긴다.
struct FakeState {
    const char* events[MaxEvents] = {};
    platform::RunPhase phases[MaxEvents] = {};
    usize count = 0;
    usize stops = 0;
    bool bodyRan = false;
    const char* failing = nullptr;

    void Record(const char* event, platform::RunPhase phase) noexcept {
        if (count < MaxEvents) {
            events[count] = event;
            phases[count] = phase;
            ++count;
        }
    }

    void MarkStage(platform::RunPhase phase, const char* name) noexcept {
        Record(name, phase);
    }
};

bool StartNamed(FakeState& state, const char* name) noexcept {
    state.Record("start", platform::RunPhase::Startup);
    return state.failing == nullptr || strcmp(state.failing, name) != 0;
}

bool StartA(FakeState& state) noexcept {
    return StartNamed(state, "A");
}

bool StartB(FakeState& state) noexcept {
    return StartNamed(state, "B");
}

bool StartC(FakeState& state) noexcept {
    return StartNamed(state, "C");
}

bool StartD(FakeState& state) noexcept {
    return StartNamed(state, "D");
}

void Stop(FakeState& state) noexcept {
    ++state.stops;
    state.Record("stop", platform::RunPhase::Shutdown);
}

int Body(FakeState& state) noexcept {
    state.bodyRan = true;
    state.Record("body", platform::RunPhase::Running);
    return BodyExitCode;
}

// B는 실패해도 계속하는 단계, C는 실패하면 끝내는 단계다. A·C만 정리 함수가 있다.
constexpr launch::detail::LaunchStage<FakeState> Stages[] = {
    {"A", &StartA, &Stop, launch::detail::StageFailure::Abort, AbortExitCode},
    {"B", &StartB, nullptr, launch::detail::StageFailure::Continue, 0},
    {"C", &StartC, &Stop, launch::detail::StageFailure::Abort, AbortExitCode},
};

// events의 단계 표시(이름)만 순서대로 이은 문자열이다(start·stop·body 기록 제외).
void CollectMarks(const FakeState& state, char* out, usize capacity) noexcept {
    usize length = 0;
    for (usize index = 0; index < state.count; ++index) {
        const char* event = state.events[index];
        if (strcmp(event, "start") == 0 || strcmp(event, "stop") == 0 ||
            strcmp(event, "body") == 0) {
            continue;
        }

        for (usize offset = 0; event[offset] != '\0' && length + 1 < capacity; ++offset) {
            out[length++] = event[offset];
        }

        if (length + 1 < capacity) {
            out[length++] = ' ';
        }
    }

    out[length] = '\0';
}

} // namespace

// 모두 시작하면 본문을 부르고, 정리 함수가 있는 단계만 역순으로 한 번씩 정리한다.
TEST(Launch_StagesRunInOrderAndStopInReverseOnce) {
    FakeState state;
    CHECK(launch::detail::RunLaunchStages(Stages, state, &Body) == BodyExitCode);
    CHECK(state.bodyRan);
    CHECK(state.stops == 2);
    char marks[256];
    CollectMarks(state, marks, sizeof(marks));
    CHECK(strcmp(marks, "A B C Main C A ") == 0);
    // 본문은 Running 표시 뒤, 정리는 Shutdown 표시 뒤다.
    REQUIRE(state.count >= 8);
    CHECK(state.phases[6] == platform::RunPhase::Running);
    CHECK(strcmp(state.events[7], "body") == 0);
    CHECK(state.phases[state.count - 2] == platform::RunPhase::Shutdown);
}

// 끝내는 단계(C)가 실패하면 본문을 부르지 않고, 시작에 성공한 단계만(A) 역순으로 정리한다. 실패한
// 단계 자신은 정리하지 않는다.
TEST(Launch_AbortingStageCleansOnlyStartedStages) {
    FakeState state;
    state.failing = "C";
    CHECK(launch::detail::RunLaunchStages(Stages, state, &Body) == AbortExitCode);
    CHECK(!state.bodyRan);
    CHECK(state.stops == 1);
    char marks[256];
    CollectMarks(state, marks, sizeof(marks));
    CHECK(strcmp(marks, "A B C A ") == 0);
}

// 첫 단계가 실패하면 아무것도 정리하지 않는다.
TEST(Launch_FirstStageFailureCleansNothing) {
    FakeState state;
    state.failing = "A";
    CHECK(launch::detail::RunLaunchStages(Stages, state, &Body) == AbortExitCode);
    CHECK(!state.bodyRan);
    CHECK(state.stops == 0);
}

// 정리 함수가 있지만 실패해도 계속하는 단계(D)다. 실패하면 시작하지 않은 단계라 정리하지 않는다.
constexpr launch::detail::LaunchStage<FakeState> ContinuingStages[] = {
    {"A", &StartA, &Stop, launch::detail::StageFailure::Abort, AbortExitCode},
    {"D", &StartD, &Stop, launch::detail::StageFailure::Continue, 0},
};

TEST(Launch_FailedContinuingStageIsNotStopped) {
    FakeState state;
    state.failing = "D";
    CHECK(launch::detail::RunLaunchStages(ContinuingStages, state, &Body) == BodyExitCode);
    CHECK(state.bodyRan);
    CHECK(state.stops == 1);
    char marks[128];
    CollectMarks(state, marks, sizeof(marks));
    CHECK(strcmp(marks, "A D Main A ") == 0);
}

// 계속하는 단계(B)가 실패하면 그 단계 없이 본문을 부른다(B는 정리 대상이 아니다).
TEST(Launch_ContinuingStageFailureStillRunsBody) {
    FakeState state;
    state.failing = "B";
    CHECK(launch::detail::RunLaunchStages(Stages, state, &Body) == BodyExitCode);
    CHECK(state.bodyRan);
    CHECK(state.stops == 2);
}

// 빌드 식별 조각은 엔진 버전과 도구 빌드 ID(hex 64자)를 담은 JSON 멤버 목록이다. 칸이 모자라면
// 비운다.
TEST(Launch_BuildIdentityFragment) {
    char identity[launch::detail::BuildIdentityBytes];
    const usize bytes = launch::detail::FormatBuildIdentity(identity, sizeof(identity));
    REQUIRE(bytes != 0);
    CHECK(bytes == strlen(identity));
    char version[EngineVersion::TextBytes];
    build::CurrentEngineVersion().ToChars(version);
    char toolBuildId[Sha256Digest::HexBytes + 1];
    build::ToolBuildId().ToHex(toolBuildId);
    char expected[launch::detail::BuildIdentityBytes];
    FixedStringBuilder text{Span<char>(expected)};
    REQUIRE(text.TryAppendFormat("\"engineVersion\": \"{}\", \"toolBuildId\": \"{}\"",
                                 StringView(version), StringView(toolBuildId)));
    CHECK(strcmp(identity, expected) == 0);
    CHECK(strlen(toolBuildId) == Sha256Digest::HexBytes);
    char small[16] = {'x'};
    CHECK(launch::detail::FormatBuildIdentity(small, sizeof(small)) == 0);
    CHECK(small[0] == '\0');
    CHECK(launch::detail::FormatBuildIdentity(nullptr, 0) == 0);
}

// 종료 요청 표시는 한 번 켜지면 계속 켜져 있다(원자 표시). 시험 실행기의 다른 시험이 켜진 표시를
// 보지 않게 끝에서 되돌린다.
TEST(Launch_TerminationRequestFlag) {
    REQUIRE(!IsTerminationRequested());
    RequestTermination();
    CHECK(IsTerminationRequested());
    RequestTermination();
    CHECK(IsTerminationRequested());
    launch::detail::ClearTerminationRequest();
    CHECK(!IsTerminationRequested());
}
