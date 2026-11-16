#include <ReflectionText/TextUse.h>

#include <Platform/Fatal.h>

namespace rocks::reflection_text {

namespace {

constexpr uint32 Bit(FieldFlags flag) noexcept {
    return static_cast<uint32>(flag);
}

// 용도별 출력 제외(10.1절 표). Copy = Edit 출력 + DuplicateTransient · TextExportTransient.
constexpr uint32 EditOutput =
    Bit(FieldFlags::Transient) | Bit(FieldFlags::NonPIEDuplicateTransient);
constexpr uint32 CopyOutput =
    EditOutput | Bit(FieldFlags::DuplicateTransient) | Bit(FieldFlags::TextExportTransient);
constexpr uint32 PieCopyOutput = Bit(FieldFlags::Transient) | Bit(FieldFlags::DuplicateTransient) |
                                 Bit(FieldFlags::TextExportTransient);
constexpr uint32 ReportOutput = Bit(FieldFlags::Transient);

// Edit 입력은 Transient만 뺀다(NonPIEDuplicateTransient·TextExportTransient 필드는 받는다).
constexpr uint32 EditInput = Bit(FieldFlags::Transient);

} // namespace

bool IsValidTextUse(TextUse use) noexcept {
    switch (use) {
    case TextUse::Edit:
    case TextUse::Copy:
    case TextUse::PieCopy:
    case TextUse::Report:
        return true;
    }

    return false;
}

uint32 TextOutputExcludedFlags(TextUse use) noexcept {
    switch (use) {
    case TextUse::Edit:
        return EditOutput;
    case TextUse::Copy:
        return CopyOutput;
    case TextUse::PieCopy:
        return PieCopyOutput;
    case TextUse::Report:
        return ReportOutput;
    }

    platform::Fatal("TextOutputExcludedFlags: undefined TextUse");
}

Result<uint32> TextInputExcludedFlags(TextUse use) noexcept {
    switch (use) {
    case TextUse::Edit:
        return Result<uint32>::Success(EditInput);
    case TextUse::Copy:
        return Result<uint32>::Success(CopyOutput);
    case TextUse::PieCopy:
        return Result<uint32>::Success(PieCopyOutput);
    case TextUse::Report:
        break;
    }

    return Result<uint32>::Failure(ErrorCode::InvalidArgument);
}

bool IsTextOutputExcluded(TextUse use, uint32 flags) noexcept {
    return (flags & TextOutputExcludedFlags(use)) != 0;
}

TextFieldInput ClassifyTextInput(TextUse use, uint32 flags) noexcept {
    auto excluded = TextInputExcludedFlags(use);
    if (!excluded) {
        platform::Fatal("ClassifyTextInput: TextUse does not accept input");
    }

    if ((flags & excluded.Value()) != 0) {
        return TextFieldInput::Excluded;
    }

    return HasFlag(flags, FieldFlags::ReadOnly) ? TextFieldInput::ReadOnly : TextFieldInput::Accept;
}

} // namespace rocks::reflection_text
