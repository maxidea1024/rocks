#include "CrashReportingInternal.h"

#include <Base/Compiler.h>

// CrashInfo.json 서식이다(Docs/Design/CrashHandlingAndLaunch.ko.md 14.3절). 크래시 처리 스레드가
// 정적 버퍼에 쓰므로 할당·lock·CRT 서식을 쓰지 않는다. 키는 영어이고 결과는 늘 올바른 UTF-8
// JSON이다(잘린 문자열도 닫는다).
namespace rocks::platform::detail {

namespace {

constexpr char HexDigits[] = "0123456789ABCDEF";
constexpr uint32 BitsPerHexDigit = 4;
constexpr uint64 HexDigitMask = 0xF;
constexpr uint64 DecimalBase = 10;
constexpr usize MaxUnsignedDigits = 20;
constexpr usize EscapedControlBytes = 6;
constexpr char ReplacementCharacter[] = "\\ufffd";
constexpr usize ReplacementBytes = sizeof(ReplacementCharacter) - 1;
constexpr uint8 FirstPrintable = 0x20;
constexpr uint8 AsciiLimit = 0x80;
constexpr uint8 ContinuationMask = 0xC0;
constexpr uint8 ContinuationTag = 0x80;
constexpr uint8 ContinuationPayload = 0x3F;
constexpr uint32 BitsPerContinuation = 6;
constexpr uint32 MaxCodePoint = 0x10FFFF;
constexpr uint32 FirstSurrogate = 0xD800;
constexpr uint32 LastSurrogate = 0xDFFF;
constexpr uint32 FirstTwoByte = 0x80;
constexpr uint32 FirstThreeByte = 0x800;
constexpr uint32 FirstFourByte = 0x10000;
// 구조 기호·숫자·고정 키를 위해 문자열 칸이 남겨 두는 자리다(고정 부분 전체보다 크다).
constexpr usize StructuralReserve = 3072;
constexpr usize UtcTextBytes = 24;
constexpr usize MaxLimitationsWritten = MaxCrashLimitations;

constexpr int64 TicksPerSecond = 10000000;
constexpr int64 SecondsPerDay = 86400;
constexpr int64 SecondsPerHour = 3600;
constexpr int64 SecondsPerMinute = 60;
// 0001-01-01부터 1970-01-01까지의 날 수다.
constexpr int64 DaysToUnixEpoch = 719162;
// civil_from_days(Howard Hinnant) 상수다.
constexpr int64 DaysFromCivilShift = 719468;
constexpr int64 DaysPerEra = 146097;
constexpr int64 DaysPer4Years = 1460;
constexpr int64 DaysPer100Years = 36524;
constexpr int64 DaysPerYear = 365;
constexpr int64 MonthsFromMarch = 153;
constexpr int64 YearsPerEra = 400;
constexpr int64 YearsPerLeapCycle = 4;
constexpr int64 YearsPerCentury = 100;
constexpr int64 MarchToDecember = 10;
constexpr int64 MonthsPerYear = 12;
constexpr int64 MarchMonth = 3;
constexpr int64 MonthCycleAdd = 2;
constexpr int64 MonthCycleDivisor = 5;

void PutDigits(char* buffer, usize& length, int64 value, usize digits) noexcept {
    for (usize index = digits; index-- > 0;) {
        buffer[length + index] = static_cast<char>('0' + value % 10);
        value /= 10;
    }

    length += digits;
}

const char* ReasonName(TerminationReason reason) noexcept {
    switch (reason) {
    case TerminationReason::Fatal:
        return "Fatal";
    case TerminationReason::Assert:
        return "Assert";
    case TerminationReason::Exception:
        return "Exception";
    case TerminationReason::Signal:
        return "Signal";
    case TerminationReason::Abort:
        return "Abort";
    case TerminationReason::RuntimeCheck:
        return "RuntimeCheck";
    case TerminationReason::Sanitizer:
        return "Sanitizer";
    }

    return "Unknown";
}

const char* PhaseName(RunPhase phase) noexcept {
    switch (phase) {
    case RunPhase::Startup:
        return "Startup";
    case RunPhase::Running:
        return "Running";
    case RunPhase::Shutdown:
        return "Shutdown";
    }

    return "Unknown";
}

const char* DetailName(CrashDumpDetail detail) noexcept {
    switch (detail) {
    case CrashDumpDetail::Standard:
        return "Standard";
    case CrashDumpDetail::Detailed:
        return "Detailed";
    case CrashDumpDetail::Full:
        return "Full";
    }

    return "Standard";
}

const char* AccessName(FaultAccess access) noexcept {
    switch (access) {
    case FaultAccess::Read:
        return "Read";
    case FaultAccess::Write:
        return "Write";
    case FaultAccess::Execute:
        return "Execute";
    case FaultAccess::None:
        break;
    }

    return nullptr;
}

const char* ConfigurationName() noexcept {
#if ROCKS_BUILD_DEBUG
    return "Debug";
#elif ROCKS_BUILD_DEVELOPMENT
    return "Development";
#else
    return "Release";
#endif
}

const char* ArchitectureName() noexcept {
#if ROCKS_ARCH_X64
    return "x64";
#else
    return "arm64";
#endif
}

bool TextEquals(const char* left, const char* right) noexcept {
    if (left == nullptr || right == nullptr) {
        return false;
    }

    usize index = 0;
    for (; left[index] != '\0' && left[index] == right[index]; ++index) {
    }

    return left[index] == right[index];
}

bool IsTerminationKindFatal(TerminationReason reason) noexcept {
    return reason == TerminationReason::Fatal || reason == TerminationReason::Assert ||
           reason == TerminationReason::Abort || reason == TerminationReason::RuntimeCheck;
}

void WriteDump(CrashJsonWriter& json, const char* key, const CrashDumpOutcome& dump) noexcept {
    json.Key(key);
    if (dump.status == nullptr) {
        json.Raw("null");
        return;
    }

    json.BeginObject();
    json.Key("status");
    json.Constant(dump.status);
    json.Key("writer");
    json.Constant(dump.writer);
    json.Key("detail");
    json.Constant(dump.detail);
    json.Key("file");
    json.Constant(dump.file);
    json.Key("error");
    if (dump.error != 0) {
        json.Hex(dump.error, 8);
    } else {
        json.Raw("null");
    }

    json.Key("reason");
    json.Constant(dump.reason);
    json.EndObject();
}

// CrashAnnotation 순서의 메타데이터 키다.
constexpr const char* AnnotationKeys[CrashAnnotationCount] = {"logDirectory", "logFile",
                                                              "traceFile", "custom0", "custom1"};
constexpr usize MaxIncompleteReasons = 10;
// 가변 칸의 escape 뒤 byte 상한이다. 합이 서식 버퍼(CrashMetadataBytes)에서 고정 부분을 뺀 자리보다
// 작아 보통은 잘리지 않고, 한 칸이 커도 다른 칸·불완전 사유를 밀어내지 않는다.
constexpr usize MaxShortFieldBytes = 256;
constexpr usize MaxPathFieldBytes = 768;
constexpr usize MaxMessageFieldBytes = 3072;
constexpr usize MaxAnnotationFieldBytes = 1024;

} // namespace

CrashJsonWriter::CrashJsonWriter(char* buffer, usize capacity, usize reserve) noexcept
    : buffer_(buffer), capacity_(capacity), reserve_(reserve < capacity ? reserve : capacity) {
    if (capacity_ != 0) {
        buffer_[0] = '\0';
    }
}

void CrashJsonWriter::Put(char value) noexcept {
    // NUL 자리 하나를 늘 남긴다.
    if (length_ + 1 < capacity_) {
        buffer_[length_++] = value;
        buffer_[length_] = '\0';
    } else {
        truncated_ = true;
    }
}

usize CrashJsonWriter::Remaining() const noexcept {
    const usize limit = capacity_ - reserve_;
    return length_ < limit ? limit - length_ : 0;
}

void CrashJsonWriter::Raw(const char* text) noexcept {
    for (usize index = 0; text[index] != '\0'; ++index) {
        Put(text[index]);
    }
}

void CrashJsonWriter::Raw(const char* text, usize bytes) noexcept {
    for (usize index = 0; index < bytes; ++index) {
        Put(text[index]);
    }
}

bool CrashJsonWriter::PutEscaped(const char* text, usize bytes, usize limit) noexcept {
    const auto* data = reinterpret_cast<const uint8*>(text);
    usize index = 0;
    while (index < bytes) {
        const uint8 lead = data[index];
        // 이 글자(escape 포함)를 쓰고도 limit 안인지 본다. 아니면 여기서 자른다.
        char encoded[EscapedControlBytes + 1];
        usize encodedBytes = 0;
        usize consumed = 1;
        if (lead == '"' || lead == '\\') {
            encoded[0] = '\\';
            encoded[1] = static_cast<char>(lead);
            encodedBytes = 2;
        } else if (lead < FirstPrintable) {
            encoded[0] = '\\';
            encoded[1] = 'u';
            encoded[2] = '0';
            encoded[3] = '0';
            encoded[4] = HexDigits[lead >> BitsPerHexDigit];
            encoded[5] = HexDigits[lead & HexDigitMask];
            encodedBytes = EscapedControlBytes;
        } else if (lead < AsciiLimit) {
            encoded[0] = static_cast<char>(lead);
            encodedBytes = 1;
        } else {
            usize extra = 0;
            uint32 codePoint = 0;
            uint32 minimum = 0;
            if (lead >= 0xC2 && lead <= 0xDF) {
                extra = 1;
                codePoint = lead & 0x1Fu;
                minimum = FirstTwoByte;
            } else if (lead >= 0xE0 && lead <= 0xEF) {
                extra = 2;
                codePoint = lead & 0x0Fu;
                minimum = FirstThreeByte;
            } else if (lead >= 0xF0 && lead <= 0xF4) {
                extra = 3;
                codePoint = lead & 0x07u;
                minimum = FirstFourByte;
            }

            bool valid = extra != 0 && index + extra < bytes;
            for (usize offset = 1; valid && offset <= extra; ++offset) {
                const uint8 next = data[index + offset];
                if ((next & ContinuationMask) != ContinuationTag) {
                    valid = false;
                } else {
                    codePoint = (codePoint << BitsPerContinuation) | (next & ContinuationPayload);
                }
            }

            valid = valid && codePoint >= minimum && codePoint <= MaxCodePoint &&
                    (codePoint < FirstSurrogate || codePoint > LastSurrogate);
            if (valid) {
                for (usize offset = 0; offset <= extra; ++offset) {
                    encoded[offset] = static_cast<char>(data[index + offset]);
                }

                encodedBytes = extra + 1;
                consumed = extra + 1;
            } else {
                for (usize offset = 0; offset < ReplacementBytes; ++offset) {
                    encoded[offset] = ReplacementCharacter[offset];
                }

                encodedBytes = ReplacementBytes;
            }
        }

        if (length_ + encodedBytes > limit) {
            return false;
        }

        for (usize offset = 0; offset < encodedBytes; ++offset) {
            buffer_[length_++] = encoded[offset];
        }

        index += consumed;
    }

    return true;
}

void CrashJsonWriter::String(const char* text, usize maxBytes) noexcept {
    if (text == nullptr) {
        Raw("null");
        return;
    }

    usize bytes = 0;
    while (text[bytes] != '\0') {
        ++bytes;
    }

    String(text, bytes, maxBytes);
}

void CrashJsonWriter::String(const char* text, usize bytes, usize maxBytes) noexcept {
    if (text == nullptr) {
        Raw("null");
        return;
    }

    Put('"');
    // 문자열 내용은 칸 상한과 reserve 앞 중 가까운 곳까지만 쓴다. 닫는 따옴표는 reserve 자리에
    // 쓴다.
    const usize available = capacity_ - reserve_;
    usize limit = length_;
    if (length_ < available) {
        const usize room = available - length_;
        limit = length_ + (maxBytes < room ? maxBytes : room);
    }

    if (!PutEscaped(text, bytes, limit)) {
        truncated_ = true;
    }

    if (length_ < capacity_) {
        buffer_[length_] = '\0';
    }

    Put('"');
}

void CrashJsonWriter::Constant(const char* text) noexcept {
    if (text == nullptr) {
        Raw("null");
        return;
    }

    Put('"');
    Raw(text);
    Put('"');
}

void CrashJsonWriter::Unsigned(uint64 value) noexcept {
    char digits[MaxUnsignedDigits];
    usize count = 0;
    do {
        digits[count++] = static_cast<char>('0' + value % DecimalBase);
        value /= DecimalBase;
    } while (value != 0);

    while (count > 0) {
        Put(digits[--count]);
    }
}

void CrashJsonWriter::Hex(uint64 value, uint32 digits) noexcept {
    Put('"');
    Put('0');
    Put('x');
    for (uint32 index = digits; index-- > 0;) {
        Put(HexDigits[(value >> (index * BitsPerHexDigit)) & HexDigitMask]);
    }

    Put('"');
}

void CrashJsonWriter::Bool(bool value) noexcept {
    Raw(value ? "true" : "false");
}

void CrashJsonWriter::Key(const char* key) noexcept {
    if (!first_) {
        Put(',');
    }

    first_ = false;
    Put('"');
    Raw(key);
    Put('"');
    Put(':');
}

void CrashJsonWriter::BeginObject() noexcept {
    Put('{');
    first_ = true;
}

void CrashJsonWriter::EndObject() noexcept {
    Put('}');
    first_ = false;
}

void CrashJsonWriter::BeginArray() noexcept {
    Put('[');
    first_ = true;
}

void CrashJsonWriter::EndArray() noexcept {
    Put(']');
    first_ = false;
}

void CrashJsonWriter::Element() noexcept {
    if (!first_) {
        Put(',');
    }

    first_ = false;
}

usize FormatUtcTicks(int64 ticks, bool compact, char* buffer, usize capacity) noexcept {
    if (capacity < UtcTextBytes - 2) {
        if (capacity != 0) {
            buffer[0] = '\0';
        }

        return 0;
    }

    const int64 totalSeconds = ticks / TicksPerSecond;
    const int64 days = totalSeconds / SecondsPerDay - DaysToUnixEpoch;
    const int64 secondOfDay = totalSeconds % SecondsPerDay;
    // civil_from_days: 1970-01-01부터의 날 수를 그레고리력 날짜로 바꾼다.
    const int64 shifted = days + DaysFromCivilShift;
    const int64 era = (shifted >= 0 ? shifted : shifted - (DaysPerEra - 1)) / DaysPerEra;
    const int64 dayOfEra = shifted - era * DaysPerEra;
    const int64 yearOfEra = (dayOfEra - dayOfEra / DaysPer4Years + dayOfEra / DaysPer100Years -
                             dayOfEra / (DaysPerEra - 1)) /
                            DaysPerYear;
    const int64 dayOfYear = dayOfEra - (DaysPerYear * yearOfEra + yearOfEra / YearsPerLeapCycle -
                                        yearOfEra / YearsPerCentury);
    const int64 monthPrime = (MonthCycleDivisor * dayOfYear + MonthCycleAdd) / MonthsFromMarch;
    const int64 day =
        dayOfYear - (MonthsFromMarch * monthPrime + MonthCycleAdd) / MonthCycleDivisor + 1;
    const int64 month = monthPrime < MarchToDecember ? monthPrime + MarchMonth
                                                     : monthPrime - (MonthsPerYear - MarchMonth);
    const int64 year = yearOfEra + era * YearsPerEra + (month <= 2 ? 1 : 0);
    usize length = 0;
    PutDigits(buffer, length, year, 4);
    if (!compact) {
        buffer[length++] = '-';
    }

    PutDigits(buffer, length, month, 2);
    if (!compact) {
        buffer[length++] = '-';
    }

    PutDigits(buffer, length, day, 2);
    buffer[length++] = 'T';
    PutDigits(buffer, length, secondOfDay / SecondsPerHour, 2);
    if (!compact) {
        buffer[length++] = ':';
    }

    PutDigits(buffer, length, secondOfDay % SecondsPerHour / SecondsPerMinute, 2);
    if (!compact) {
        buffer[length++] = ':';
    }

    PutDigits(buffer, length, secondOfDay % SecondsPerMinute, 2);
    buffer[length++] = 'Z';
    buffer[length] = '\0';
    return length;
}

usize FormatCrashMetadata(char* buffer, usize capacity, const CrashRecordSnapshot& record,
                          const CrashMetadataInput& input) noexcept {
    // 구조 기호·고정 문자열(Constant)은 reserve 자리까지 쓰고, 가변 문자열은 칸별 상한과 reserve
    // 앞까지만 쓴다. 가변 칸은 문서 끝에 모아 두고 그 뒤에 불완전 사유를 적는다. 그래서 가변 칸이
    // 잘려도 앞의 고정 칸은 모두 남고 metadataTruncated가 빠지지 않는다.
    CrashJsonWriter json(buffer, capacity,
                         capacity > StructuralReserve * 2 ? StructuralReserve : capacity / 2);
    char utc[UtcTextBytes];
    const bool crash = !input.unreportedExit;
    const auto reason = static_cast<TerminationReason>(record.reason);
    json.BeginObject();
    json.Key("format");
    json.Constant("rocks.crash/1");
    json.Key("crashId");
    json.Constant(record.crashId);
    json.Key("program");
    json.Constant(record.program);
    json.Key("processId");
    json.Unsigned(record.processId);
    json.Key("processStartUtc");
    if (record.processStartTicks != 0) {
        (void)FormatUtcTicks(static_cast<int64>(record.processStartTicks), false, utc, sizeof(utc));
        json.Constant(utc);
    } else {
        json.Raw("null");
    }

    json.Key("crashUtc");
    if (input.hasCrashUtc) {
        (void)FormatUtcTicks(input.crashUtcTicks, false, utc, sizeof(utc));
        json.Constant(utc);
    } else {
        json.Raw("null");
    }

    json.Key("phase");
    json.Constant(crash ? PhaseName(static_cast<RunPhase>(record.phase)) : nullptr);
    json.Key("kind");
    json.Constant(crash ? ReasonName(reason) : "UnreportedExit");
    if (!crash) {
        // 도우미가 부모의 종료 코드만 안다(처리기가 돌지 않은 종료: fast-fail·보장 없는 stack
        // overflow 등). 크래시 칸은 모두 null이다.
        json.Key("exitCode");
        json.Hex(input.exitCode, 8);
    }

    const bool signal =
        record.hasException && static_cast<CrashCodeKind>(record.codeKind) == CrashCodeKind::Signal;
    json.Key(signal ? "signal" : "exceptionCode");
    if (!crash || !record.hasException) {
        json.Raw("null");
    } else if (signal) {
        json.Unsigned(record.code);
    } else {
        json.Hex(record.code, 8);
    }

    json.Key("exceptionAddress");
    if (crash && record.hasException) {
        json.Hex(record.address, 16);
    } else {
        json.Raw("null");
    }

    const char* access = AccessName(static_cast<FaultAccess>(record.access));
    json.Key("faultAccess");
    if (crash && record.hasException && access != nullptr) {
        json.BeginObject();
        json.Key("operation");
        json.Constant(access);
        json.Key("address");
        json.Hex(record.accessAddress, 16);
        json.EndObject();
    } else {
        json.Raw("null");
    }

    json.Key("failingThreadId");
    if (crash) {
        json.Unsigned(record.failingThreadId);
    } else {
        json.Raw("null");
    }

    json.Key("reporterThreadId");
    if (crash) {
        json.Unsigned(record.reporterThreadId);
    } else {
        json.Raw("null");
    }

    json.Key("reporterFailed");
    json.Bool(crash && record.reporterFailed);
    json.Key("concurrentCrashes");
    if (crash) {
        json.BeginObject();
        json.Key("count");
        json.Unsigned(record.stoppedTotal);
        json.Key("threads");
        json.BeginArray();
        for (uint32 index = 0; index < record.stoppedCount && index < MaxStoppedCrashThreads;
             ++index) {
            json.Element();
            json.Unsigned(record.stoppedIds[index]);
        }

        json.EndArray();
        json.EndObject();
    } else {
        json.Raw("null");
    }

    WriteDump(json, "dump", input.dump);
    WriteDump(json, "detailDump", input.detailDump);
    json.Key("dumpDetail");
    json.Constant(DetailName(static_cast<CrashDumpDetail>(record.dumpDetail)));
    json.Key("limitations");
    json.BeginArray();
    for (usize index = 0; index < input.limitationCount && index < MaxLimitationsWritten; ++index) {
        json.Element();
        json.Constant(input.limitations[index]);
    }

    json.EndArray();
    json.Key("reportDirectory");
    json.Constant(input.fallbackDirectory ? "Fallback" : "Primary");
    json.Key("helper");
    if (input.helper.status != nullptr) {
        json.BeginObject();
        json.Key("status");
        json.Constant(input.helper.status);
        json.Key("error");
        if (input.helper.error != 0) {
            json.Hex(input.helper.error, 8);
        } else {
            json.Raw("null");
        }

        json.Key("exitCode");
        if (input.helper.hasExitCode) {
            json.Hex(input.helper.exitCode, 8);
        } else {
            json.Raw("null");
        }

        // 도우미가 부모의 job에서 빠져나오지 못했다(job을 닫으면 도우미도 끝난다). 부모는 기본
        // 덤프가 끝날 때까지 기다린 뒤 끝난다.
        json.Key("inJob");
        json.Bool(input.helper.inJob);
        json.EndObject();
    } else {
        json.Raw("null");
    }

    json.Key("terminationCallbacks");
    if (crash) {
        json.BeginObject();
        json.Key("status");
        json.Constant(record.callbackStatus[0] != '\0' ? record.callbackStatus : "NotRun");
        json.Key("milliseconds");
        json.Unsigned(record.callbackMilliseconds);
        json.EndObject();
    } else {
        json.Raw("null");
    }

    // 명령줄은 개수만 남긴다(원문은 수집하지 않는다, 설계 14.3절). 모르면(Launch를 거치지 않은
    // 설치) null이다.
    json.Key("arguments");
    if (record.argumentCount != CrashReportingOptions::UnknownArgumentCount) {
        json.BeginObject();
        json.Key("count");
        json.Unsigned(record.argumentCount);
        json.EndObject();
    } else {
        json.Raw("null");
    }
    json.Key("build");
    json.BeginObject();
    json.Key("configuration");
    json.Constant(ConfigurationName());
    json.Key("platform");
    json.Constant(CrashPlatformName());
    json.Key("architecture");
    json.Constant(ArchitectureName());
    json.Key("sanitizer");
    json.Constant(ROCKS_ADDRESS_SANITIZER ? "address" : nullptr);
    // 길이 칸은 교환 블록에서 온 값일 수 있다. 배열 크기를 넘지 않는다.
    const usize identityBytes =
        record.identityBytes < CrashIdentityBytes ? record.identityBytes : CrashIdentityBytes - 1;
    if (identityBytes != 0) {
        // Launch가 만든 멤버 조각이다. 다 들어가지 않으면 잘린 조각 대신 표시만 남긴다.
        if (identityBytes + 1 < json.Remaining()) {
            json.Raw(",");
            json.Raw(record.identity, identityBytes);
        } else {
            json.MarkTruncated();
            json.Key("identityTruncated");
            json.Bool(true);
        }
    }

    json.EndObject();
    json.Key("module");
    if (record.hasModule && CrashModuleJsonBytes < json.Remaining()) {
        json.Raw(record.moduleJson);
    } else {
        if (record.hasModule) {
            json.MarkTruncated();
        }

        json.Raw("null");
    }

    // 가변 문자열 칸이다(칸별 상한, 잘리면 metadataTruncated).
    json.Key("executable");
    json.String(record.hasExecutable ? record.executable : nullptr, MaxPathFieldBytes);
    json.Key("stage");
    json.String(crash && record.hasStage ? record.stage : nullptr, MaxShortFieldBytes);
    json.Key("failingThreadName");
    json.String(crash && record.hasThreadName ? record.failingThreadName : nullptr,
                MaxShortFieldBytes);
    json.Key("fatal");
    if (crash && IsTerminationKindFatal(reason)) {
        json.BeginObject();
        json.Key("kind");
        json.Constant(ReasonName(reason));
        json.Key("line");
        if (record.hasFile) {
            json.Unsigned(record.line);
        } else {
            json.Raw("null");
        }

        json.Key("returnAddress");
        json.Hex(record.returnAddress, 16);
        json.Key("file");
        json.String(record.hasFile ? record.file : nullptr, MaxPathFieldBytes);
        json.EndObject();
    } else {
        json.Raw("null");
    }

    json.Key("message");
    if (crash) {
        json.String(record.message, record.messageBytes, MaxMessageFieldBytes);
    } else {
        json.Raw("null");
    }

    json.Key("associatedFiles");
    json.BeginObject();
    for (usize slot = 0; slot < CrashAnnotationCount; ++slot) {
        json.Key(AnnotationKeys[slot]);
        if (record.annotationBytes[slot] != 0) {
            json.String(record.annotations[slot], record.annotationBytes[slot],
                        MaxAnnotationFieldBytes);
        } else {
            json.Raw("null");
        }
    }

    json.EndObject();

    // 불완전 사유는 끝에 둔다(앞 칸이 잘렸는지 여기서 안다).
    const char* reasons[MaxIncompleteReasons];
    usize reasonCount = 0;
    if (!input.final) {
        reasons[reasonCount++] = "pending";
    }

    if (crash && record.reporterFailed) {
        reasons[reasonCount++] = "reporterFailed";
    }

    if (crash && TextEquals(record.callbackStatus, "TimedOut")) {
        reasons[reasonCount++] = "callbacksTimedOut";
    }

    if (crash && TextEquals(record.callbackStatus, "Unknown")) {
        reasons[reasonCount++] = "callbacksUnknown";
    }

    if (TextEquals(input.dump.status, "Failed") || TextEquals(input.dump.status, "Cancelled") ||
        TextEquals(input.dump.status, "Skipped")) {
        reasons[reasonCount++] = "dumpNotWritten";
    }

    if (TextEquals(input.detailDump.status, "Failed") ||
        TextEquals(input.detailDump.status, "Cancelled") ||
        TextEquals(input.detailDump.status, "Skipped")) {
        reasons[reasonCount++] = "detailDumpNotWritten";
    }

    if (record.annotationTorn) {
        reasons[reasonCount++] = "annotationTorn";
    }

    if (json.Truncated()) {
        reasons[reasonCount++] = "metadataTruncated";
    }

    json.Key("complete");
    json.Bool(reasonCount == 0);
    json.Key("incompleteReasons");
    json.BeginArray();
    for (usize index = 0; index < reasonCount; ++index) {
        json.Element();
        json.Constant(reasons[index]);
    }

    json.EndArray();
    json.EndObject();
    json.Raw("\n");
    return json.Length();
}

} // namespace rocks::platform::detail
