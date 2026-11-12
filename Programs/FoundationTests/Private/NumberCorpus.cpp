#include <Core/Memory/SystemAllocator.h>
#include <Core/Text/NumberText.h>
#include <Core/Text/StringBuilder.h>
#include <Platform/File.h>
#include <Platform/Memory.h>
#include <stdio.h>
#include <string.h>

// .NET 대조 말뭉치 검사(도구 시험 Native.NumberConversionMatchesDotNet이 만든 파일). 한 줄이 한
// 사례다:
//   D <double 비트 16진> <.NET "R" 문자열>   최단 표기와 그 문자열의 해석
//   F <float 비트 16진> <.NET "R" 문자열>
//   P <10진 문자열> <.NET double.Parse 비트 16진>   (7FF0000000000000은 넘침 → Overflow 기대)
//   Q <10진 문자열> <.NET float.Parse 비트 16진>    (7F800000은 넘침)
// 최단 표기는 표기 형식(.NET은 "E+XX")이 달라 유효 숫자열과 첫 숫자의 10진 지수로 비교한다.
namespace rocks::test {
namespace {

struct Decimal {
    char digits[32];
    int count = 0;
    int exponent = 0; // 첫 유효 숫자의 10진 지수
};

// "123.45E-6" 같은 표기를 유효 숫자열(앞뒤 0 없음)과 지수로 나눈다.
bool Normalize(const char* text, usize length, Decimal& out) noexcept {
    out = Decimal{};
    int point = 0;
    bool seenPoint = false;
    bool started = false;
    usize index = 0;
    for (; index < length && text[index] != 'e' && text[index] != 'E'; ++index) {
        const char character = text[index];
        if (character == '.') {
            seenPoint = true;
            continue;
        }

        if (character < '0' || character > '9') {
            return false;
        }

        if (!started && character == '0') {
            if (seenPoint) {
                --point;
            }

            continue;
        }

        started = true;
        if (out.count == static_cast<int>(sizeof(out.digits))) {
            return false;
        }

        out.digits[out.count++] = character;
        if (!seenPoint) {
            ++point;
        }
    }

    int exponent = 0;
    if (index < length) {
        ++index;
        bool negative = false;
        if (index < length && (text[index] == '+' || text[index] == '-')) {
            negative = text[index++] == '-';
        }

        for (; index < length; ++index) {
            exponent = exponent * 10 + (text[index] - '0');
        }

        exponent = negative ? -exponent : exponent;
    }

    while (out.count > 0 && out.digits[out.count - 1] == '0') {
        --out.count;
    }

    out.exponent = point - 1 + exponent;
    return out.count != 0;
}

bool SameDecimal(const Decimal& left, const Decimal& right) noexcept {
    return left.count == right.count && left.exponent == right.exponent &&
           memcmp(left.digits, right.digits, static_cast<usize>(left.count)) == 0;
}

bool ParseHex(const char* text, uint64& value) noexcept {
    value = 0;
    if (*text == '\0') {
        return false;
    }

    for (; *text != '\0'; ++text) {
        const char character = *text;
        const int digit = character >= '0' && character <= '9'   ? character - '0'
                          : character >= 'A' && character <= 'F' ? character - 'A' + 10
                                                                 : -1;
        if (digit < 0) {
            return false;
        }

        value = value * 16 + static_cast<uint64>(digit);
    }

    return true;
}

} // namespace

// 파일을 1 MiB 단위로 읽어 한 줄씩(LF 기준, 최대 16 KiB) 넘긴다.
class LineReader {
public:
    bool Open(const char* path) noexcept {
        return static_cast<bool>(file_.Open(path, platform::FileOpenMode::ReadExisting));
    }

    bool Close() noexcept {
        const platform::FileStatus closed = file_.Close();
        return static_cast<bool>(closed);
    }

    // 다음 줄을 line에 NUL 종료로 넣는다. 끝이면 false. 읽기 오류·너무 긴 줄은 Failed()로 남긴다.
    bool Next(char* line, usize capacity) noexcept {
        usize length = 0;
        while (true) {
            if (position_ == filled_) {
                const platform::FileTransfer read = file_.Read(buffer_, sizeof(buffer_));
                if (!read.status) {
                    failed_ = true;
                    return false;
                }

                if (read.bytes == 0) {
                    line[length] = '\0';
                    return length != 0;
                }

                filled_ = read.bytes;
                position_ = 0;
            }

            const char character = buffer_[position_++];
            if (character == '\n') {
                line[length] = '\0';
                return true;
            }

            if (length + 1 >= capacity) {
                failed_ = true;
                return false;
            }

            line[length++] = character;
        }
    }

    bool Failed() const noexcept {
        return failed_;
    }

private:
    platform::File file_;
    char buffer_[1 << 20];
    usize filled_ = 0;
    usize position_ = 0;
    bool failed_ = false;
};

int RunNumberCorpus(const char* path) noexcept {
    static LineReader reader;
    if (!reader.Open(path)) {
        fprintf(stderr, "number corpus: cannot open %s\n", path);
        return 2;
    }

    static char line[16384];
    SystemAllocator allocator;
    StringBuilder<64> builder(allocator);
    uint64 cases = 0;
    uint64 mismatches = 0;
    auto report = [&mismatches](const char* kind, const char* first, const char* second,
                                const char* detail) noexcept {
        if (++mismatches <= 20) {
            fprintf(stderr, "number corpus mismatch %s %s %s: %s\n", kind, first, second, detail);
        }
    };
    while (reader.Next(line, sizeof(line))) {
        usize length = strlen(line);
        while (length > 0 && (line[length - 1] == '\n' || line[length - 1] == '\r')) {
            line[--length] = '\0';
        }

        if (length < 4 || line[1] != ' ') {
            continue;
        }

        char* first = line + 2;
        char* space = strchr(first, ' ');
        if (space == nullptr) {
            report("?", line, "", "malformed line");
            continue;
        }

        *space = '\0';
        char* second = space + 1;
        ++cases;
        const char kind = line[0];
        if (kind == 'D' || kind == 'F') {
            uint64 bits = 0;
            if (!ParseHex(first, bits)) {
                report("?", first, second, "bad bits");
                continue;
            }

            builder.Clear();
            const bool formatted =
                kind == 'D' ? static_cast<bool>(
                                  builder.TryAppendFormat("{}", platform::BitCast<double>(bits)))
                            : static_cast<bool>(builder.TryAppendFormat(
                                  "{}", platform::BitCast<float>(static_cast<uint32>(bits))));
            Decimal ours;
            Decimal theirs;
            if (!formatted || !Normalize(builder.View().Data(), builder.ByteCount(), ours) ||
                !Normalize(second, strlen(second), theirs) || !SameDecimal(ours, theirs)) {
                report(kind == 'D' ? "shortest double" : "shortest float", first, second,
                       "digits differ");
                continue;
            }

            // .NET 문자열과 Rocks 자신의 출력이 모두 원래 비트로 읽혀야 한다.
            auto roundTrips = [kind, bits](StringView text) noexcept {
                if (kind == 'D') {
                    auto parsed = TryParseDouble(text);
                    return parsed && platform::BitCast<uint64>(parsed.Value()) == bits;
                }

                auto parsed = TryParseFloat(text);
                return parsed &&
                       platform::BitCast<uint32>(parsed.Value()) == static_cast<uint32>(bits);
            };
            if (!roundTrips(StringView(second, strlen(second))) || !roundTrips(builder.View())) {
                report(kind == 'D' ? "parse shortest double" : "parse shortest float", first,
                       second, "bits differ");
            }
        } else if (kind == 'P' || kind == 'Q') {
            uint64 expected = 0;
            if (!ParseHex(second, expected)) {
                report("?", first, second, "bad bits");
                continue;
            }

            const bool single = kind == 'Q';
            const uint64 infinity = single ? 0x7F800000ull : 0x7FF0000000000000ull;
            const uint64 magnitude = expected & (single ? 0x7FFFFFFFull : 0x7FFFFFFFFFFFFFFFull);
            bool same = false;
            if (single) {
                auto parsed = TryParseFloat(first);
                same = magnitude == infinity
                           ? !parsed && parsed.Error() == ErrorCode::Overflow
                           : parsed && platform::BitCast<uint32>(parsed.Value()) == expected;
            } else {
                auto parsed = TryParseDouble(first);
                same = magnitude == infinity
                           ? !parsed && parsed.Error() == ErrorCode::Overflow
                           : parsed && platform::BitCast<uint64>(parsed.Value()) == expected;
            }

            if (!same) {
                report(single ? "parse float" : "parse double", first, second, "result differs");
            }
        } else {
            report("?", line, second, "unknown kind");
        }
    }

    const bool closed = reader.Close();
    if (reader.Failed()) {
        fprintf(stderr, "number corpus: read error or line longer than %llu bytes\n",
                static_cast<unsigned long long>(sizeof(line)));
    }

    printf("number corpus: %llu cases, %llu mismatches\n", static_cast<unsigned long long>(cases),
           static_cast<unsigned long long>(mismatches));
    return mismatches == 0 && cases != 0 && closed && !reader.Failed() ? 0 : 1;
}

} // namespace rocks::test
