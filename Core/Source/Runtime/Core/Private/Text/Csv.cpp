#include <Core/Text/Csv.h>

// CSV 읽기다(Core/Text/Csv.h). UE FCsvParser::ParseRows·ParseRow·ParseCell·MeasureNewLine
// (Runtime/Core/Private/Serialization/Csv/CsvParser.cpp:25-160)의 상태 기계를 그대로 따르되, 원본을
// 제자리에서 고치지 않고 칸을 사본으로 만든다.
namespace rocks {

namespace {

constexpr char Quote = '"';
constexpr char Comma = ',';
constexpr char CarriageReturn = '\r';
constexpr char LineFeed = '\n';
constexpr char Terminator = '\0';

// 파싱 상태다(UE EParseResult와 읽는 위치).
enum class CsvStep : uint8 {
    EndOfCell,
    EndOfRow,
    EndOfString
};

class CsvParser {
public:
    CsvParser(StringView text, Array<CsvRow>& out) noexcept : text_(text), out_(out) {
    }

    Result<void> Run() noexcept {
        // UE는 비지 않은 문자열이면 파싱한다(첫 byte가 NUL이어도 빈 칸 하나짜리 행이 생긴다).
        if (text_.IsEmpty()) {
            return Result<void>::Success();
        }

        CsvStep step = CsvStep::EndOfRow;
        do {
            auto parsed = ParseRow();
            if (!parsed) {
                return Result<void>::Failure(parsed.Error());
            }

            step = parsed.Value();
        } while (step != CsvStep::EndOfString);

        return Result<void>::Success();
    }

private:
    // 문자열 끝(길이 끝 또는 NUL)이다.
    bool AtEnd() const noexcept {
        return position_ >= text_.ByteCount() || text_.Data()[position_] == Terminator;
    }

    char At(usize index) const noexcept {
        return index < text_.ByteCount() ? text_.Data()[index] : Terminator;
    }

    // UE MeasureNewLine: `\r\n` 2, `\r`·`\n` 1, 아니면 0.
    usize MeasureNewLine(usize index) const noexcept {
        const char current = At(index);
        if (current == CarriageReturn) {
            return At(index + 1) == LineFeed ? 2 : 1;
        }

        return current == LineFeed ? 1 : 0;
    }

    // 줄 끝 하나를 건너뛰고 물리 줄 번호를 센다.
    void SkipNewLine(usize size) noexcept {
        position_ += size;
        ++line_;
    }

    Result<CsvStep> ParseRow() noexcept {
        // 행 첫머리의 줄 끝은 빈 줄이다.
        const usize newLine = MeasureNewLine(position_);
        if (newLine != 0) {
            SkipNewLine(newLine);
            return Result<CsvStep>::Success(AtEnd() ? CsvStep::EndOfString : CsvStep::EndOfRow);
        }

        CsvRow row(out_.GetAllocator());
        row.line = line_;
        if (auto pushed = out_.TryPushBack(Move(row)); !pushed) {
            return Result<CsvStep>::Failure(pushed.Error());
        }

        CsvStep step = CsvStep::EndOfCell;
        do {
            auto parsed = ParseCell(out_[out_.Size() - 1]);
            if (!parsed) {
                return parsed;
            }

            step = parsed.Value();
        } while (step == CsvStep::EndOfCell);

        return Result<CsvStep>::Success(step);
    }

    Result<CsvStep> ParseCell(CsvRow& row) noexcept {
        String cell(out_.GetAllocator());
        bool quoted = At(position_) == Quote;
        if (quoted) {
            ++position_;
        }

        auto finish = [&row, &cell](CsvStep step) noexcept -> Result<CsvStep> {
            if (auto pushed = row.cells.TryPushBack(Move(cell)); !pushed) {
                return Result<CsvStep>::Failure(pushed.Error());
            }

            return Result<CsvStep>::Success(step);
        };

        while (!AtEnd()) {
            const char current = text_.Data()[position_];
            if (quoted) {
                if (current == Quote) {
                    // 따옴표 연속: 하나마다 열림·닫힘이 바뀌고, 다시 열릴 때 따옴표 하나를
                    // 쓴다(`""`).
                    while (At(position_) == Quote) {
                        ++position_;
                        quoted = !quoted;
                        if (quoted) {
                            if (auto appended = cell.TryAppend(StringView("\"", 1)); !appended) {
                                return Result<CsvStep>::Failure(appended.Error());
                            }
                        }
                    }

                    continue;
                }
            } else {
                const usize newLine = MeasureNewLine(position_);
                if (newLine != 0) {
                    SkipNewLine(newLine);
                    return finish(AtEnd() ? CsvStep::EndOfString : CsvStep::EndOfRow);
                }

                if (current == Comma) {
                    ++position_;
                    // 다음 칸이 하나 더 있다(끝이면 빈 칸을 더하고 EndOfString).
                    return finish(CsvStep::EndOfCell);
                }
            }

            // 따옴표 안의 줄바꿈도 물리 줄로 센다.
            if (current == LineFeed ||
                (current == CarriageReturn && At(position_ + 1) != LineFeed)) {
                ++line_;
            }

            if (auto appended = cell.TryAppend(StringView(text_.Data() + position_, 1));
                !appended) {
                return Result<CsvStep>::Failure(appended.Error());
            }

            ++position_;
        }

        return finish(CsvStep::EndOfString);
    }

    StringView text_;
    Array<CsvRow>& out_;
    usize position_ = 0;
    usize line_ = 1;
};

} // namespace

Result<void> TryParseCsv(StringView text, Array<CsvRow>& out) noexcept {
    // 사본에 다 만든 뒤 붙인다(실패면 out 그대로).
    Array<CsvRow> staged(out.GetAllocator());
    CsvParser parser(text, staged);
    if (auto parsed = parser.Run(); !parsed) {
        return parsed;
    }

    if (auto reserved = out.TryReserve(out.Size() + staged.Size()); !reserved) {
        return reserved;
    }

    for (CsvRow& row : staged) {
        (void)out.TryPushBack(Move(row));
    }

    return Result<void>::Success();
}

} // namespace rocks
