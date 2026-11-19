#include "../Private/Common/CrashReportingInternal.h"

#include <Base/Compiler.h>
#include <Platform/CrashReporting.h>
#include <Platform/Sync.h>
#include <Platform/Thread.h>
#include <TestSupport/HostFailure.h>
#include <TestSupport/Test.h>

#include <string.h>

using namespace rocks;
using namespace rocks::platform;

// 크래시 수집기의 프로세스 안 계약(등록·해제·보호 호출 통과·capability·설치 인자 검사·연관 파일
// 칸·메타데이터 서식)을 검사한다. 설치와 크래시 흐름은 시험 실행기를 크래시시키지 않도록 자식
// probe(Native.Crash.*)에서 검사한다.
namespace {

TerminationCallbackResult CountingCallback(const TerminationContext&, void* user) noexcept {
    ++*static_cast<uint32*>(user);
    return TerminationCallbackResult::Completed;
}

// 지금 비어 있는 칸 수를 센다(등록했다가 모두 푼다).
uint32 CountFreeSlots() noexcept {
    uint32 calls = 0;
    TerminationCallbackHandle handles[MaxTerminationCallbacks];
    uint32 count = 0;
    while (count < MaxTerminationCallbacks &&
           RegisterTerminationCallback(&CountingCallback, &calls, handles[count])) {
        ++count;
    }

    for (uint32 index = 0; index < count; ++index) {
        UnregisterTerminationCallback(handles[index]);
    }

    return count;
}

constexpr int GuardedValue = 42;

int ReturnGuardedValue(void* context) noexcept {
    *static_cast<bool*>(context) = true;
    return GuardedValue;
}

} // namespace

// 칸이 찰 때까지 등록되고, 해제하면 다시 쓸 수 있다. 해제한 handle은 "없음" 표지로 돌아가고 두 번
// 풀어도 무시된다. 같은 시험 프로세스의 다른 등록자(로그 콜백)가 칸을 쓰고 있을 수 있으므로 성공
// 수는 1 이상으로만 본다.
TEST(CrashReporting_TerminationCallbackSlotsAreBoundedAndReusable) {
    uint32 calls = 0;
    TerminationCallbackHandle handles[MaxTerminationCallbacks + 1];
    uint32 registered = 0;
    while (registered <= MaxTerminationCallbacks &&
           RegisterTerminationCallback(&CountingCallback, &calls, handles[registered])) {
        ++registered;
    }

    CHECK(registered >= 1);
    CHECK(registered <= MaxTerminationCallbacks);
    TerminationCallbackHandle overflow;
    CHECK(!RegisterTerminationCallback(&CountingCallback, &calls, overflow));
    CHECK(overflow.slot == NumericLimits<uint32>::Max());
    for (uint32 index = 0; index < registered; ++index) {
        UnregisterTerminationCallback(handles[index]);
        CHECK(handles[index].slot == NumericLimits<uint32>::Max());
        UnregisterTerminationCallback(handles[index]);
    }

    TerminationCallbackHandle again;
    REQUIRE(RegisterTerminationCallback(&CountingCallback, &calls, again));
    UnregisterTerminationCallback(again);
    CHECK(calls == 0);
}

// nullptr 콜백은 등록하지 않는다.
TEST(CrashReporting_NullCallbackIsRejected) {
    TerminationCallbackHandle handle;
    CHECK(!RegisterTerminationCallback(nullptr, nullptr, handle));
    CHECK(handle.slot == NumericLimits<uint32>::Max());
}

// 이미 풀린 칸을 다른 등록이 다시 쓴 뒤에는 옛 handle의 해제가 새 등록을 풀지 않는다(세대 확인).
TEST(CrashReporting_StaleHandleDoesNotReleaseReusedSlot) {
    uint32 calls = 0;
    const uint32 freeBefore = CountFreeSlots();
    REQUIRE(freeBefore >= 1);
    TerminationCallbackHandle first;
    REQUIRE(RegisterTerminationCallback(&CountingCallback, &calls, first));
    TerminationCallbackHandle stale = first;
    UnregisterTerminationCallback(first);
    TerminationCallbackHandle second;
    REQUIRE(RegisterTerminationCallback(&CountingCallback, &calls, second));
    UnregisterTerminationCallback(stale);
    CHECK(CountFreeSlots() == freeBefore - 1);
    UnregisterTerminationCallback(second);
    CHECK(CountFreeSlots() == freeBefore);
}

// 보호 호출은 예외가 없으면 body의 반환값을 그대로 돌려준다(설치 여부와 무관).
TEST(CrashReporting_GuardedCallReturnsBodyValue) {
    bool ran = false;
    CHECK(RunWithCrashGuard(&ReturnGuardedValue, &ran) == GuardedValue);
    CHECK(ran);
}

// capability는 플랫폼이 실제로 하는 것만 켠다. 메타데이터는 모든 플랫폼, 덤프·도우미·보고 없는
// 종료 기록은 Windows(sanitizer 구성 제외)만이다.
TEST(CrashReporting_CapabilitiesMatchThisStage) {
    const CrashCapability capabilities = QueryCrashCapabilities();
    CHECK(HasCrashCapability(capabilities, CrashCapability::OsFaultHandling));
    CHECK(HasCrashCapability(capabilities, CrashCapability::TerminationCallbacks));
    CHECK(HasCrashCapability(capabilities, CrashCapability::Metadata));
    CHECK(HasCrashCapability(capabilities, CrashCapability::Minidump) ==
          (IsWindows && !ROCKS_ADDRESS_SANITIZER));
    CHECK(HasCrashCapability(capabilities, CrashCapability::OutOfProcessDump) ==
          (IsWindows && !ROCKS_ADDRESS_SANITIZER));
    CHECK(HasCrashCapability(capabilities, CrashCapability::UnreportedExitRecord) ==
          (IsWindows && !ROCKS_ADDRESS_SANITIZER));
    // Windows는 SEH 보호 프레임을 두고, POSIX는 처리 뒤 OS 수집기에 넘긴다.
    CHECK(HasCrashCapability(capabilities, CrashCapability::ThreadGuard) == IsWindows);
    CHECK(HasCrashCapability(capabilities, CrashCapability::SystemCrashReport) == !IsWindows);
}

// 이름이 없거나 파일 이름으로 쓸 수 없거나 예산이 0이면 설치가 거절할 옵션이다. 미리 검사는 상태를
// 바꾸지 않는다(시험 실행기에는 Launch가 설치한 수집기가 있으므로 설치 자체는 AlreadyInstalled다).
TEST(CrashReporting_ValidationRejectsInvalidOptions) {
    CrashReportingOptions valid;
    valid.programName = "Probe";
    CHECK(ValidateCrashReportingOptions(valid));
    CrashReportingOptions missingName;
    CHECK(!ValidateCrashReportingOptions(missingName));
    CrashReportingOptions badName;
    badName.programName = "bad/name";
    CHECK(!ValidateCrashReportingOptions(badName));
    CrashReportingOptions dots;
    dots.programName = "..";
    CHECK(!ValidateCrashReportingOptions(dots));
    CrashReportingOptions emptyDirectory;
    emptyDirectory.programName = "Probe";
    emptyDirectory.outputDirectory = "";
    CHECK(!ValidateCrashReportingOptions(emptyDirectory));
    CrashReportingOptions emptyHelper;
    emptyHelper.programName = "Probe";
    emptyHelper.helperExecutable = "";
    CHECK(!ValidateCrashReportingOptions(emptyHelper));
    CrashReportingOptions zeroCapture;
    zeroCapture.programName = "Probe";
    zeroCapture.captureTimeoutMilliseconds = 0;
    CHECK(!ValidateCrashReportingOptions(zeroCapture));
    CrashReportingOptions zeroBudget;
    zeroBudget.programName = "Probe";
    zeroBudget.inProcessDumpTimeoutMilliseconds = 0;
    CHECK(!ValidateCrashReportingOptions(zeroBudget));
    CrashReportingOptions tooLong;
    tooLong.programName = "Probe";
    tooLong.callbackBudgetMilliseconds = NumericLimits<uint32>::Max();
    CHECK(!ValidateCrashReportingOptions(tooLong));
    CHECK(InstallCrashReporting(missingName) == CrashReportingStatus::AlreadyInstalled);
    CHECK(IsCrashReportingInstalled());
    // 도우미 표지가 아닌 인자는 도우미 모드가 아니다.
    int helperExit = 0;
    char program[] = "FoundationTests";
    char other[] = "--list";
    char* arguments[] = {program, other};
    CHECK(!RunCrashHelperIfRequested(2, arguments, helperExit));
}

// 연관 파일 칸은 쓴 값을 그대로 돌려주고, 0 byte로 비운다. 범위 밖 인자는 칸을 바꾸지 않는다.
TEST(CrashReporting_AnnotationSlotsRoundTrip) {
    constexpr char path[] = "Saved/Logs/Probe-2026.log";
    REQUIRE(SetCrashAnnotation(CrashAnnotation::Custom1, path, sizeof(path) - 1));
    char text[MaxCrashAnnotationBytes];
    usize bytes = 0;
    REQUIRE(detail::ReadCrashAnnotation(CrashAnnotation::Custom1, text, sizeof(text), bytes));
    CHECK(bytes == sizeof(path) - 1);
    CHECK(memcmp(text, path, bytes) == 0);
    char large[MaxCrashAnnotationBytes + 1] = {};
    CHECK(!SetCrashAnnotation(CrashAnnotation::Custom1, large, sizeof(large)));
    CHECK(!SetCrashAnnotation(CrashAnnotation::Count, path, 1));
    CHECK(!SetCrashAnnotation(CrashAnnotation::Custom1, nullptr, 1));
    REQUIRE(detail::ReadCrashAnnotation(CrashAnnotation::Custom1, text, sizeof(text), bytes));
    CHECK(bytes == sizeof(path) - 1);
    REQUIRE(SetCrashAnnotation(CrashAnnotation::Custom1, nullptr, 0));
    REQUIRE(detail::ReadCrashAnnotation(CrashAnnotation::Custom1, text, sizeof(text), bytes));
    CHECK(bytes == 0);
}

// JSON 쓰기는 따옴표·역슬래시·제어 문자를 escape하고 잘못된 UTF-8을 U+FFFD로 바꾼다.
TEST(CrashReporting_JsonWriterEscapesText) {
    char buffer[256];
    detail::CrashJsonWriter json(buffer, sizeof(buffer), 16);
    json.BeginObject();
    json.Key("text");
    json.String("a\"b\\c\n\x01\xC3\xA9\xFF");
    json.Key("none");
    json.String(nullptr);
    json.Key("code");
    json.Hex(0xC0000005u, 8);
    json.Key("list");
    json.BeginArray();
    json.Element();
    json.Unsigned(NumericLimits<uint64>::Max());
    json.Element();
    json.Bool(true);
    json.EndArray();
    json.EndObject();
    CHECK(!json.Truncated());
    CHECK(strcmp(buffer, "{\"text\":\"a\\\"b\\\\c\\u000A\\u0001\xC3\xA9\\ufffd\","
                         "\"none\":null,\"code\":\"0xC0000005\",\"list\":"
                         "[18446744073709551615,true]}") == 0);
}

// 문자열이 남은 자리를 넘으면 글자 경계에서 잘라 닫고 Truncated가 참이다. 결과는 닫힌 JSON이다.
TEST(CrashReporting_JsonWriterTruncatesStringsButStaysClosed) {
    char buffer[32];
    detail::CrashJsonWriter json(buffer, sizeof(buffer), 8);
    json.BeginObject();
    json.Key("t");
    // "가"(3 byte) 여덟 개다.
    json.String("\xEA\xB0\x80\xEA\xB0\x80\xEA\xB0\x80\xEA\xB0\x80\xEA\xB0\x80\xEA\xB0\x80"
                "\xEA\xB0\x80\xEA\xB0\x80");
    json.EndObject();
    CHECK(json.Truncated());
    const usize length = strlen(buffer);
    REQUIRE(length >= 2);
    CHECK(buffer[length - 1] == '}');
    CHECK(buffer[length - 2] == '"');
    // 잘린 자리가 3 byte 글자의 경계다("{"t":" 6 byte 뒤 내용).
    CHECK((length - 2 - 6) % 3 == 0);
}

// UtcTicks 서식은 그레고리력 날짜와 compact 폴더 이름 형식을 만든다.
TEST(CrashReporting_FormatsUtcTicks) {
    // 2026-11-18T09:15:42Z = 1970-01-01부터 1794993342초.
    constexpr int64 UnixEpochTicks = 621355968000000000;
    constexpr int64 TicksPerSecond = 10000000;
    const int64 ticks = UnixEpochTicks + int64{1794993342} * TicksPerSecond;
    char text[32];
    CHECK(detail::FormatUtcTicks(ticks, false, text, sizeof(text)) == 20);
    CHECK(strcmp(text, "2026-11-18T09:15:42Z") == 0);
    CHECK(detail::FormatUtcTicks(ticks, true, text, sizeof(text)) == 16);
    CHECK(strcmp(text, "20261118T091542Z") == 0);
    // 윤년 2월 29일.
    const int64 leap = UnixEpochTicks + int64{1709164800} * TicksPerSecond;
    (void)detail::FormatUtcTicks(leap, false, text, sizeof(text));
    CHECK(strcmp(text, "2024-02-29T00:00:00Z") == 0);
}

namespace {

// JSON 구조가 닫혔는지(따옴표 짝·괄호 깊이) 본다. 완전한 파서는 아니지만 잘린 문서를 잡는다.
bool IsClosedJson(const char* text) noexcept {
    int depth = 0;
    bool inString = false;
    bool escaped = false;
    for (const char* cursor = text; *cursor != '\0'; ++cursor) {
        const char value = *cursor;
        if (inString) {
            if (escaped) {
                escaped = false;
            } else if (value == '\\') {
                escaped = true;
            } else if (value == '"') {
                inString = false;
            } else if (static_cast<unsigned char>(value) < 0x20) {
                return false;
            }
        } else if (value == '"') {
            inString = true;
        } else if (value == '{' || value == '[') {
            ++depth;
        } else if (value == '}' || value == ']') {
            if (--depth < 0) {
                return false;
            }
        }
    }

    return depth == 0 && !inString;
}

detail::CrashRecordSnapshot& ProbeRecord() noexcept {
    static detail::CrashRecordSnapshot record;
    record = detail::CrashRecordSnapshot{};
    memcpy(record.program, "Probe", sizeof("Probe"));
    memcpy(record.crashId, "00112233445566778899AABBCCDDEEFF", detail::CrashIdTextBytes);
    record.reason = static_cast<uint8>(TerminationReason::Fatal);
    record.phase = static_cast<uint8>(RunPhase::Running);
    return record;
}

} // namespace

// 메시지(잘못된 UTF-8 2047 byte)와 연관 파일 5칸(역슬래시 512 byte)이 칸 상한을 넘으면 그 칸만
// 잘리고, 뒤의 고정 칸·불완전 사유(metadataTruncated)는 남으며 문서는 닫힌다.
TEST(CrashReporting_MetadataTruncationKeepsFixedFieldsAndReason) {
    detail::CrashRecordSnapshot& record = ProbeRecord();
    memset(record.message, 0xFF, detail::MaxCrashMessageBytes - 1);
    record.message[detail::MaxCrashMessageBytes - 1] = '\0';
    record.messageBytes = static_cast<uint32>(detail::MaxCrashMessageBytes - 1);
    for (usize slot = 0; slot < detail::CrashAnnotationCount; ++slot) {
        memset(record.annotations[slot], '\\', MaxCrashAnnotationBytes);
        record.annotationBytes[slot] = static_cast<uint32>(MaxCrashAnnotationBytes);
    }

    detail::CrashMetadataInput input;
    input.final = true;
    input.dump = {"Written", "Helper", "Standard", "Minidump.dmp", 0, nullptr};
    static char buffer[detail::CrashMetadataBytes];
    const usize length = detail::FormatCrashMetadata(buffer, sizeof(buffer), record, input);
    REQUIRE(length > 0);
    REQUIRE(length < sizeof(buffer));
    CHECK(IsClosedJson(buffer));
    CHECK(buffer[length - 1] == '\n');
    CHECK(strstr(buffer, "\"metadataTruncated\"") != nullptr);
    CHECK(strstr(buffer, "\"complete\":false") != nullptr);
    CHECK(strstr(buffer, "\"kind\":\"Fatal\"") != nullptr);
    CHECK(strstr(buffer, "\"dumpDetail\":\"Standard\"") != nullptr);
    CHECK(strstr(buffer, "\"custom1\":\"\\\\") != nullptr);
}

// 같은 입력을 작은 버퍼에 쓰면 가변 칸이 reserve 앞에서 잘리지만 문서는 닫히고 사유가 남는다.
TEST(CrashReporting_MetadataTruncationInSmallBufferStaysClosed) {
    detail::CrashRecordSnapshot& record = ProbeRecord();
    memset(record.message, 'm', 1000);
    record.messageBytes = 1000;
    detail::CrashMetadataInput input;
    input.final = true;
    static char buffer[6 * 1024];
    const usize length = detail::FormatCrashMetadata(buffer, sizeof(buffer), record, input);
    REQUIRE(length > 0);
    CHECK(IsClosedJson(buffer));
    CHECK(strstr(buffer, "\"incompleteReasons\"") != nullptr);
}

// 덤프가 실패·취소·건너뜀이면 불완전(dumpNotWritten)이다. dbghelp를 열지 못한 경우가 이 값이다.
TEST(CrashReporting_FailedDumpIsIncomplete) {
    detail::CrashRecordSnapshot& record = ProbeRecord();
    detail::CrashMetadataInput input;
    input.final = true;
    input.dump = {"Failed", nullptr, nullptr, nullptr, 126, "dbghelp.dll unavailable"};
    static char buffer[detail::CrashMetadataBytes];
    (void)detail::FormatCrashMetadata(buffer, sizeof(buffer), record, input);
    CHECK(IsClosedJson(buffer));
    CHECK(strstr(buffer, "\"dumpNotWritten\"") != nullptr);
    CHECK(strstr(buffer, "\"complete\":false") != nullptr);
    CHECK(strstr(buffer, "\"reason\":\"dbghelp.dll unavailable\"") != nullptr);
}

namespace {

// 시험용 PE32+ 메모리 이미지다(RVA 배치). 머리 0x80, 디버그 디렉터리 0x400, RSDS 레코드 0x500.
constexpr usize PeImageBytes = 8192;
constexpr usize PeHeaders = 0x80;
constexpr usize PeOptional = PeHeaders + 24;
constexpr usize PeDebugDirectory = 0x400;
constexpr usize PeCodeViewRecord = 0x500;
constexpr char PePdbPath[] = "C:\\build\\out\\Probe.pdb";

void Put32(uint8* image, usize offset, uint32 value) noexcept {
    for (usize index = 0; index < 4; ++index) {
        image[offset + index] = static_cast<uint8>(value >> (index * 8));
    }
}

void Put16(uint8* image, usize offset, uint16 value) noexcept {
    image[offset] = static_cast<uint8>(value);
    image[offset + 1] = static_cast<uint8>(value >> 8);
}

uint8* BuildPeImage() noexcept {
    static uint8 image[PeImageBytes];
    memset(image, 0, sizeof(image));
    Put16(image, 0, 0x5A4D);
    Put32(image, 0x3C, static_cast<uint32>(PeHeaders));
    Put32(image, PeHeaders, 0x00004550);
    Put32(image, PeHeaders + 8, 0x6AFBF903);
    Put16(image, PeHeaders + 20, 240);
    Put16(image, PeOptional, 0x020B);
    Put32(image, PeOptional + 56, static_cast<uint32>(PeImageBytes));
    Put32(image, PeOptional + 108, 16);
    Put32(image, PeOptional + 112 + 6 * 8, static_cast<uint32>(PeDebugDirectory));
    Put32(image, PeOptional + 112 + 6 * 8 + 4, 28);
    Put32(image, PeDebugDirectory + 12, 2);
    Put32(image, PeDebugDirectory + 16, static_cast<uint32>(24 + sizeof(PePdbPath)));
    Put32(image, PeDebugDirectory + 20, static_cast<uint32>(PeCodeViewRecord));
    Put32(image, PeCodeViewRecord, 0x53445352);
    for (usize index = 0; index < 16; ++index) {
        image[PeCodeViewRecord + 4 + index] = static_cast<uint8>(0x10 + index);
    }

    Put32(image, PeCodeViewRecord + 20, 3);
    memcpy(image + PeCodeViewRecord + 24, PePdbPath, sizeof(PePdbPath));
    return image;
}

} // namespace

// 올바른 이미지에서 GUID(레지스트리 형식)·age·PDB 파일 이름(경로 제외)·TimeDateStamp·크기를 읽는다.
TEST(CrashReporting_ParsesPeCodeView) {
    const uint8* image = BuildPeImage();
    CHECK(detail::PeImageSize(image, 4096) == PeImageBytes);
    detail::PeCodeView codeView;
    REQUIRE(detail::ParsePeCodeView(image, PeImageBytes, codeView));
    REQUIRE(codeView.hasCodeView);
    CHECK(strcmp(codeView.guid, "13121110-1514-1716-1819-1A1B1C1D1E1F") == 0);
    CHECK(codeView.age == 3);
    CHECK(codeView.pdbNameBytes == sizeof("Probe.pdb") - 1);
    CHECK(memcmp(codeView.pdbName, "Probe.pdb", codeView.pdbNameBytes) == 0);
    CHECK(codeView.timeDateStamp == 0x6AFBF903);
    CHECK(codeView.sizeOfImage == PeImageBytes);
}

// 머리·디렉터리·레코드가 이미지 밖을 가리키거나 형식이 틀리면 거부한다(범위 밖을 읽지 않는다).
TEST(CrashReporting_RejectsDamagedPeImages) {
    detail::PeCodeView codeView;
    uint8* image = BuildPeImage();
    Put32(image, 0x3C, 0x7FFFFFF0);
    CHECK(!detail::ParsePeCodeView(image, PeImageBytes, codeView));
    CHECK(detail::PeImageSize(image, 4096) == 0);
    image = BuildPeImage();
    Put16(image, PeOptional, 0x010B);
    CHECK(!detail::ParsePeCodeView(image, PeImageBytes, codeView));
    image = BuildPeImage();
    Put32(image, PeOptional + 108, 6);
    CHECK(!detail::ParsePeCodeView(image, PeImageBytes, codeView));
    image = BuildPeImage();
    Put32(image, PeOptional + 56, static_cast<uint32>(PeImageBytes * 2));
    CHECK(!detail::ParsePeCodeView(image, PeImageBytes, codeView));
    image = BuildPeImage();
    Put32(image, PeOptional + 112 + 6 * 8, static_cast<uint32>(PeImageBytes - 8));
    CHECK(!detail::ParsePeCodeView(image, PeImageBytes, codeView));
    image = BuildPeImage();
    Put32(image, PeDebugDirectory + 20, static_cast<uint32>(PeImageBytes - 16));
    CHECK(!detail::ParsePeCodeView(image, PeImageBytes, codeView));
    image = BuildPeImage();
    Put32(image, PeDebugDirectory + 16, 0xFFFFFFF0u);
    CHECK(!detail::ParsePeCodeView(image, PeImageBytes, codeView));
    CHECK(!detail::ParsePeCodeView(image, 32, codeView));
    CHECK(!detail::ParsePeCodeView(nullptr, PeImageBytes, codeView));
}

// 다른 코드가 처리되지 않은 예외 filter를 바꾸면 교환 확인이 false이고 그 filter를 그대로 둔다.
// 되돌린 뒤에는 다시 수집기 것이다(Windows, 시험 실행기는 Launch가 설치했다).
TEST(CrashReporting_ReplacedUnhandledFilterIsDetectedAndKept) {
    if (!IsWindows) {
        CHECK(CheckUnhandledCrashFilter());
        return;
    }

    REQUIRE(IsCrashReportingInstalled());
    REQUIRE(CheckUnhandledCrashFilter());
    REQUIRE(rocks::test::ReplaceUnhandledFilterForTest());
    CHECK(!CheckUnhandledCrashFilter());
    CHECK(rocks::test::TestUnhandledFilterIsCurrent());
    rocks::test::RestoreUnhandledFilterForTest();
    CHECK(CheckUnhandledCrashFilter());
}

// 단계 표시는 마지막으로 표시한 값을 돌려준다(크래시 기록이 남길 값).
TEST(CrashReporting_RunPhaseReadsBackTheLastMark) {
    const RunPhase phase = CurrentRunPhase();
    const char* stage = CurrentRunStage();
    static constexpr char Probe[] = "Tests.RunPhase";
    SetRunPhase(RunPhase::Shutdown, Probe);
    CHECK(CurrentRunPhase() == RunPhase::Shutdown);
    CHECK(CurrentRunStage() == Probe);
    SetRunPhase(phase, stage);
}

namespace {

// 단계 이름 표 시험의 이름들이다. 표는 포인터 동일성으로 등록하므로 칸마다 다른 주소면 된다. 표보다
// 많이 둔다(가득 찬 뒤의 등록을 본다).
constexpr uint32 StageNameCandidates = MaxRunStageNames * 2;
constexpr usize StageNameBytes = 2;
constinit char stageNameStorage[StageNameCandidates][StageNameBytes] = {};

const char* StageName(uint32 index) noexcept {
    return stageNameStorage[index];
}

// 동시 등록 시험의 스레드 수다.
constexpr uint32 StageRegistrationThreads = 4;

struct StageRegistration {
    detail::RunStageNameTable* table = nullptr;
    // 모든 스레드가 준비를 알린 뒤 한꺼번에 연다.
    ManualResetEvent* start = nullptr;
    ManualResetEvent ready;
    // 0이면 모든 후보를 앞에서부터, 아니면 뒤에서부터 등록한다(순서가 다른 경쟁).
    bool reverse = false;
    uint32 slots[StageNameCandidates] = {};
};

void RegisterStageNames(void* context) noexcept {
    StageRegistration& work = *static_cast<StageRegistration*>(context);
    work.ready.Signal();
    (void)work.start->Wait();
    for (uint32 step = 0; step < StageNameCandidates; ++step) {
        const uint32 index = work.reverse ? StageNameCandidates - 1 - step : step;
        work.slots[index] = work.table->Register(StageName(index));
    }
}

} // namespace

// 표가 차면 새 이름은 0(이름 없음)이고 이미 쓴 칸은 그대로다. 같은 포인터는 같은 칸이다.
TEST(CrashReporting_RunStageNameTableStopsWhenFull) {
    detail::RunStageNameTable table;
    CHECK(table.Register(nullptr) == 0);
    for (uint32 index = 0; index < MaxRunStageNames; ++index) {
        CHECK(table.Register(StageName(index)) == index + 1);
    }

    for (uint32 index = MaxRunStageNames; index < StageNameCandidates; ++index) {
        CHECK(table.Register(StageName(index)) == 0);
    }

    for (uint32 index = 0; index < MaxRunStageNames; ++index) {
        CHECK(table.Register(StageName(index)) == index + 1);
        CHECK(table.Find(index + 1) == StageName(index));
    }

    CHECK(table.Find(0) == nullptr);
    CHECK(table.Find(MaxRunStageNames + 1) == nullptr);
}

// 여러 스레드가 같은 이름들을 서로 다른 순서로 동시에 처음 등록한다. 시작은 gate 하나로 연다. 어느
// 순서로 엇갈려도 이름마다 모든 스레드가 같은 칸을 받고, 칸은 정확히 MaxRunStageNames개 쓰이며,
// 칸마다 이름이 하나다(덮어쓰거나 중복 칸을 만들지 않는다).
TEST(CrashReporting_RunStageNameTableConcurrentRegistration) {
    detail::RunStageNameTable table;
    ManualResetEvent start;
    StageRegistration work[StageRegistrationThreads];
    Thread threads[StageRegistrationThreads];
    uint32 started = 0;
    for (uint32 index = 0; index < StageRegistrationThreads; ++index) {
        work[index].table = &table;
        work[index].start = &start;
        work[index].reverse = index % 2 != 0;
        if (threads[index].Start(&RegisterStageNames, &work[index]) != ThreadStatus::Success) {
            break;
        }

        ++started;
    }

    // 시작한 스레드가 모두 gate 앞에 온 뒤 연다. 시작에 실패했어도 연 뒤 모아서 끝낸다.
    for (uint32 index = 0; index < started; ++index) {
        (void)work[index].ready.Wait();
    }

    start.Signal();
    bool joined = true;
    for (uint32 index = 0; index < started; ++index) {
        joined = threads[index].Join() == ThreadStatus::Success && joined;
    }

    REQUIRE(started == StageRegistrationThreads);
    REQUIRE(joined);

    uint32 registered = 0;
    bool slotUsed[MaxRunStageNames + 1] = {};
    for (uint32 index = 0; index < StageNameCandidates; ++index) {
        const uint32 slot = work[0].slots[index];
        for (const StageRegistration& each : work) {
            CHECK(each.slots[index] == slot);
        }

        if (slot != 0) {
            ++registered;
            REQUIRE(slot <= MaxRunStageNames);
            CHECK(!slotUsed[slot]);
            slotUsed[slot] = true;
            CHECK(table.Find(slot) == StageName(index));
        }
    }

    CHECK(registered == MaxRunStageNames);
}

// 시험 실행기는 Launch TestRunner(크래시 수집 Full)로 시작했다. 두 번째 설치는 상태를 바꾸지 않고
// AlreadyInstalled다.
TEST(CrashReporting_TestRunnerIsInstalledByLaunch) {
    REQUIRE(IsCrashReportingInstalled());
    CrashReportingOptions second;
    second.programName = "Probe";
    CHECK(InstallCrashReporting(second) == CrashReportingStatus::AlreadyInstalled);
    CHECK(IsCrashReportingInstalled());
}
