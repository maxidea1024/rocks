#include <Core/Private/App/CommandLineStorage.h>

#include <Base/Compiler.h>
#include <Core/App/ProcessCommandLine.h>
#include <Platform/Process.h>
#include <TestSupport/Test.h>

#include <string.h>

using namespace rocks;

// ProcessCommandLine의 채우기 규칙(UTF-16·UTF-8 변환, 상한, 플랫폼 진입점 형태)을 지역 저장소로
// 검사한다. 실제 진입점(wmain·wWinMain·main)에서 한글·이모지·따옴표·빈 인자가 오는지는 시험 실행
// 파일을 자식으로 띄워 검사한다(Native.Launch.*).
namespace {

// 저장소는 크다(인자 view·텍스트·경로). stack 대신 정적 칸을 시험마다 다시 채운다.
detail::CommandLineStorage storage;

constexpr usize LargeArgumentCount = ProcessCommandLine::MaxArguments + 2;
wchar_t* wideArguments[LargeArgumentCount];
char* narrowArguments[LargeArgumentCount];
wchar_t emptyWide[] = L"";
char emptyNarrow[] = "";

// 한 byte씩 비교한다(StringView와 UTF-8 기대 값).
bool Equals(StringView view, const char* expected) noexcept {
    return view.ByteCount() == strlen(expected) &&
           memcmp(view.Data(), expected, view.ByteCount()) == 0;
}

} // namespace

// 이 플랫폼의 진입점 형태로 한글·이모지·따옴표·빈 인자가 UTF-8 원문 그대로 온다. 각 인자는 NUL로
// 끝나고, 실행 파일 경로는 argv[0]이 아니라 OS에서 얻는다.
TEST(ProcessCommandLine_ConvertsEntryArguments) {
    wchar_t program[] = L"ignored-program-name";
    wchar_t korean[] = L"한글";
    wchar_t emoji[] = L"\U0001F600";
    wchar_t quoted[] = L"a\"b c";
    wchar_t* wide[] = {program, korean, emoji, quoted, emptyWide};
    char narrowProgram[] = "ignored-program-name";
    char narrowKorean[] = "\xED\x95\x9C\xEA\xB8\x80";
    char narrowEmoji[] = "\xF0\x9F\x98\x80";
    char narrowQuoted[] = "a\"b c";
    char* narrow[] = {narrowProgram, narrowKorean, narrowEmoji, narrowQuoted, emptyNarrow};
    const Result<void> filled = IsWindows ? storage.Fill(5, wide) : storage.Fill(5, narrow);
    REQUIRE(filled);
    const Span<const StringView> arguments = storage.Arguments();
    REQUIRE(arguments.Size() == 4);
    CHECK(Equals(arguments[0], "\xED\x95\x9C\xEA\xB8\x80"));
    CHECK(Equals(arguments[1], "\xF0\x9F\x98\x80"));
    CHECK(Equals(arguments[2], "a\"b c"));
    CHECK(arguments[3].ByteCount() == 0);
    for (const StringView argument : arguments) {
        CHECK(argument.Data()[argument.ByteCount()] == '\0');
    }

    char path[ProcessCommandLine::MaxExecutablePathBytes + 1];
    usize pathBytes = 0;
    REQUIRE(platform::GetExecutablePath(path, sizeof(path), pathBytes));
    CHECK(storage.ExecutablePath().ByteCount() == pathBytes);
    CHECK(memcmp(storage.ExecutablePath().Data(), path, pathBytes) == 0);
}

// 플랫폼에 맞지 않는 진입점 형태(Windows의 char, POSIX의 wchar_t)는 인자가 없어도 Unsupported다.
TEST(ProcessCommandLine_RejectsTheOtherEntryForm) {
    wchar_t wideProgram[] = L"program";
    char narrowProgram[] = "program";
    wchar_t* wide[] = {wideProgram};
    char* narrow[] = {narrowProgram};
    const Result<void> other = IsWindows ? storage.Fill(1, narrow) : storage.Fill(1, wide);
    REQUIRE(!other);
    CHECK(other.Error() == ErrorCode::Unsupported);
    CHECK(storage.Arguments().Size() == 0);
}

// 잘못된 인코딩(짝 없는 UTF-16 surrogate, 잘못된 UTF-8)·nullptr 인자·argc 0은 InvalidArgument다.
TEST(ProcessCommandLine_RejectsInvalidArguments) {
    wchar_t wideProgram[] = L"program";
    wchar_t lone[] = {static_cast<wchar_t>(0xD800), L'x', L'\0'};
    wchar_t* badWide[] = {wideProgram, lone};
    char narrowProgram[] = "program";
    char invalid[] = "\xFF\xFE";
    char* badNarrow[] = {narrowProgram, invalid};
    const Result<void> bad = IsWindows ? storage.Fill(2, badWide) : storage.Fill(2, badNarrow);
    REQUIRE(!bad);
    CHECK(bad.Error() == ErrorCode::InvalidArgument);
    wchar_t* nullWide[] = {wideProgram, nullptr};
    char* nullNarrow[] = {narrowProgram, nullptr};
    const Result<void> missing =
        IsWindows ? storage.Fill(2, nullWide) : storage.Fill(2, nullNarrow);
    REQUIRE(!missing);
    CHECK(missing.Error() == ErrorCode::InvalidArgument);
    const Result<void> empty = IsWindows ? storage.Fill(0, nullWide) : storage.Fill(0, nullNarrow);
    REQUIRE(!empty);
    CHECK(empty.Error() == ErrorCode::InvalidArgument);
}

// 인자 수·텍스트 상한을 넘으면 CapacityExceeded이고 상한 정확히는 받는다.
TEST(ProcessCommandLine_EnforcesLimits) {
    for (usize index = 0; index < LargeArgumentCount; ++index) {
        wideArguments[index] = emptyWide;
        narrowArguments[index] = emptyNarrow;
    }

    const int atLimit = static_cast<int>(ProcessCommandLine::MaxArguments + 1);
    const Result<void> fits =
        IsWindows ? storage.Fill(atLimit, wideArguments) : storage.Fill(atLimit, narrowArguments);
    REQUIRE(fits);
    CHECK(storage.Arguments().Size() == ProcessCommandLine::MaxArguments);
    const Result<void> tooMany = IsWindows ? storage.Fill(atLimit + 1, wideArguments)
                                           : storage.Fill(atLimit + 1, narrowArguments);
    REQUIRE(!tooMany);
    CHECK(tooMany.Error() == ErrorCode::CapacityExceeded);

    // 인자 하나가 텍스트 칸(끝 NUL 포함)을 넘는다.
    static wchar_t wideLarge[ProcessCommandLine::MaxTotalBytes + 1];
    static char narrowLarge[ProcessCommandLine::MaxTotalBytes + 1];
    for (usize index = 0; index < ProcessCommandLine::MaxTotalBytes; ++index) {
        wideLarge[index] = L'a';
        narrowLarge[index] = 'a';
    }

    wchar_t* wideHuge[] = {emptyWide, wideLarge};
    char* narrowHuge[] = {emptyNarrow, narrowLarge};
    const Result<void> large = IsWindows ? storage.Fill(2, wideHuge) : storage.Fill(2, narrowHuge);
    REQUIRE(!large);
    CHECK(large.Error() == ErrorCode::CapacityExceeded);
    // 끝 NUL까지 정확히 들어가는 길이는 받는다.
    wideLarge[ProcessCommandLine::MaxTotalBytes - 1] = L'\0';
    narrowLarge[ProcessCommandLine::MaxTotalBytes - 1] = '\0';
    const Result<void> exact = IsWindows ? storage.Fill(2, wideHuge) : storage.Fill(2, narrowHuge);
    REQUIRE(exact);
    CHECK(storage.Arguments()[0].ByteCount() == ProcessCommandLine::MaxTotalBytes - 1);
}

// 프로세스 캡처는 한 번이다. 시험 실행기는 Launch 진입점이 이미 캡처했으므로 두 번째는
// AlreadyExists이고 처음 값(실행기 인자)이 그대로다. 첫 캡처의 변환 규칙은 지역 저장소 시험과
// 자식 프로세스 시험(Native.Launch.*)이 본다.
TEST(ProcessCommandLine_CapturesOnce) {
    REQUIRE(ProcessCommandLine::IsCaptured());
    wchar_t wideProgram[] = L"program";
    wchar_t wideFirst[] = L"first";
    char narrowProgram[] = "program";
    char narrowFirst[] = "first";
    wchar_t* wide[] = {wideProgram, wideFirst};
    char* narrow[] = {narrowProgram, narrowFirst};
    const Span<const StringView> before = ProcessCommandLine::Get().Arguments();
    const StringView executable = ProcessCommandLine::Get().ExecutablePath();
    const Result<void> again =
        IsWindows ? ProcessCommandLine::Capture(2, wide) : ProcessCommandLine::Capture(2, narrow);
    REQUIRE(!again);
    CHECK(again.Error() == ErrorCode::AlreadyExists);
    CHECK(ProcessCommandLine::Get().Arguments().Data() == before.Data());
    CHECK(ProcessCommandLine::Get().Arguments().Size() == before.Size());
    CHECK(ProcessCommandLine::Get().ExecutablePath().Data() == executable.Data());
}
