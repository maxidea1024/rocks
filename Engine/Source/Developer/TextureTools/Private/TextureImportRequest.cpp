#include <Base/NumericLimits.h>
#include <ImageCodec/ImageDecode.h>
#include <Platform/File.h>
#include <TextureTools/TextureImport.h>

// 텍스처 임포트 요청과 asset-import 예약 인자 번역이다(Docs/Design/AssetSystem 12.12절 명령 문법
// 표, 13절 목록·이스케이프 규칙).
namespace rocks {
namespace {

using reflection_text::AssignmentKind;
using reflection_text::TextAssignment;

constexpr StringView CubeLayoutNames[] = {"None", "HorizontalCross", "VerticalCross", "Strip6"};
constexpr StringView PngInfillNames[] = {"", "Never", "OnlyOnBinaryTransparency", "Always"};
constexpr StringView DdsExtension = ".dds";
constexpr uint64 DecimalBase = 10;
// uint64 10진 표기의 최대 자릿수다(18446744073709551615).
constexpr usize MaxDecimalDigits = 20;
// `--cube=` 면 지정의 면 이름 길이와 구분자다(`px:<파일>` — 13절).
constexpr usize FaceNameBytes = 2;
constexpr char FaceSeparator = ':';
constexpr char ListSeparator = ',';
constexpr char CubeSeparator = ';';
constexpr char Escape = '\\';
constexpr char TileSeparator = 'x';

// out.strings에 새 문자열을 더하고 그 주소를 준다(String은 힙 byte라 배열이 자라도 view가 유효).
Result<String*> NewString(AssetImportTranslation& out) noexcept {
    if (auto pushed = out.strings.TryPushBack(String(out.strings.GetAllocator())); !pushed) {
        return Result<String*>::Failure(pushed.Error());
    }

    return Result<String*>::Success(&out.strings[out.strings.Size() - 1]);
}

Result<void> AppendDecimal(String& text, uint64 value) noexcept {
    char digits[MaxDecimalDigits];
    usize count = 0;
    do {
        digits[count++] = static_cast<char>('0' + value % DecimalBase);
        value /= DecimalBase;
    } while (value != 0);

    char reversed[MaxDecimalDigits];
    for (usize index = 0; index < count; ++index) {
        reversed[index] = digits[count - 1 - index];
    }

    return text.TryAppend(StringView(reversed, count));
}

Result<void> AddOption(AssetImportTranslation& out, StringView field, StringView value) noexcept {
    TextAssignment assignment;
    assignment.kind = AssignmentKind::Set;
    assignment.path = field;
    assignment.value = value;
    return out.options.TryPushBack(assignment);
}

Result<void> AddNumberOption(AssetImportTranslation& out, StringView field, uint64 value) noexcept {
    auto text = NewString(out);
    if (!text) {
        return Result<void>::Failure(text.Error());
    }

    if (auto appended = AppendDecimal(*text.Value(), value); !appended) {
        return appended;
    }

    return AddOption(out, field, text.Value()->View());
}

// 이스케이프 문자(`\` 뒤 `,`·`;`)를 건너뛰며 separator로 나눈 원문 구간을 out에 더한다(이스케이프는
// 그대로 둔다 — Unescape가 푼다). 빈 구간은 거짓이다.
bool SplitRaw(StringView text, char separator, Array<StringView>& out) noexcept {
    usize start = 0;
    for (usize index = 0; index <= text.ByteCount(); ++index) {
        if (index < text.ByteCount()) {
            const char c = text.Data()[index];
            if (c == Escape && index + 1 < text.ByteCount() &&
                (text.Data()[index + 1] == ListSeparator ||
                 text.Data()[index + 1] == CubeSeparator)) {
                ++index;
                continue;
            }

            if (c != separator) {
                continue;
            }
        }

        if (index == start || !out.TryPushBack(text.SliceBytes(start, index - start))) {
            return false;
        }

        start = index + 1;
    }

    return true;
}

// raw의 `\,`·`\;`를 푼 새 문자열을 out.strings에 둔다(그 밖의 `\`는 글자 그대로 — 13절).
Result<StringView> Unescape(StringView raw, AssetImportTranslation& out) noexcept {
    auto text = NewString(out);
    if (!text) {
        return Result<StringView>::Failure(text.Error());
    }

    usize start = 0;
    for (usize index = 0; index < raw.ByteCount(); ++index) {
        if (raw.Data()[index] == Escape && index + 1 < raw.ByteCount() &&
            (raw.Data()[index + 1] == ListSeparator || raw.Data()[index + 1] == CubeSeparator)) {
            if (auto appended = text.Value()->TryAppend(raw.SliceBytes(start, index - start));
                !appended) {
                return Result<StringView>::Failure(appended.Error());
            }

            start = index + 1;
            ++index;
        }
    }

    if (auto appended = text.Value()->TryAppend(raw.SliceBytes(start, raw.ByteCount() - start));
        !appended) {
        return Result<StringView>::Failure(appended.Error());
    }

    return Result<StringView>::Success(text.Value()->View());
}

// cube 지정(`px:<f>,nx:<f>,…` — 면 6개, 순서 자유)을 면 순서로 files에 더한다. 위반은 거짓이다.
Result<bool> ParseCube(StringView text, Allocator& allocator, AssetImportTranslation& out,
                       Array<StringView>& files) noexcept {
    Array<StringView> items(allocator);
    if (!SplitRaw(text, ListSeparator, items) || items.Size() != TextureCubeFaces) {
        return Result<bool>::Success(false);
    }

    StringView faces[TextureCubeFaces] = {};
    for (StringView item : items) {
        if (item.ByteCount() <= FaceNameBytes + 1 || item.Data()[FaceNameBytes] != FaceSeparator) {
            return Result<bool>::Success(false);
        }

        const StringView name = item.SliceBytes(0, FaceNameBytes);
        usize face = TextureCubeFaces;
        for (usize index = 0; index < TextureCubeFaces; ++index) {
            face = name == TextureCubeFaceRoles[index] ? index : face;
        }

        if (face == TextureCubeFaces || !faces[face].IsEmpty()) {
            return Result<bool>::Success(false);
        }

        auto file =
            Unescape(item.SliceBytes(FaceNameBytes + 1, item.ByteCount() - FaceNameBytes - 1), out);
        if (!file) {
            return Result<bool>::Failure(file.Error());
        }

        faces[face] = file.Value();
    }

    for (StringView face : faces) {
        if (auto pushed = files.TryPushBack(face); !pushed) {
            return Result<bool>::Failure(pushed.Error());
        }
    }

    return Result<bool>::Success(true);
}

// 파일 목록(`,` 구분)을 files에 더한다(빈 항목은 거짓).
Result<bool> ParseFileList(StringView text, Allocator& allocator, AssetImportTranslation& out,
                           Array<StringView>& files) noexcept {
    Array<StringView> items(allocator);
    if (!SplitRaw(text, ListSeparator, items)) {
        return Result<bool>::Success(false);
    }

    for (StringView item : items) {
        auto file = Unescape(item, out);
        if (!file) {
            return Result<bool>::Failure(file.Error());
        }

        if (auto pushed = files.TryPushBack(file.Value()); !pushed) {
            return Result<bool>::Failure(pushed.Error());
        }
    }

    return Result<bool>::Success(true);
}

// 1 이상의 10진 수(앞 0 허용 안 함)다.
bool ParsePositive(StringView text, uint32& out) noexcept {
    if (text.IsEmpty() || text.Data()[0] == '0') {
        return false;
    }

    uint64 value = 0;
    for (char digit : text) {
        if (digit < '0' || digit > '9') {
            return false;
        }

        value = value * DecimalBase + static_cast<uint64>(digit - '0');
        if (value > NumericLimits<uint32>::Max()) {
            return false;
        }
    }

    out = static_cast<uint32>(value);
    return true;
}

bool EndsWithIgnoringCase(StringView text, StringView suffix) noexcept {
    return text.ByteCount() >= suffix.ByteCount() &&
           EqualsIgnoringAsciiCase(
               text.SliceBytes(text.ByteCount() - suffix.ByteCount(), suffix.ByteCount()), suffix);
}

// DDS 원본 하나의 머리 차원으로 임포터를 고른다(UE EditorFactories.cpp 3330–3376 — 읽지 못하면
// 고르지 않는다).
void ChooseDdsImporter(StringView path, AssetImportTranslation& out) noexcept {
    if (!EndsWithIgnoringCase(path, DdsExtension)) {
        return;
    }

    platform::File file;
    if (!file.Open(platform::PhysicalFileSystem(), path.Data(), path.ByteCount(),
                   platform::FileOpenMode::ReadExisting)) {
        return;
    }

    uint8 header[image::ImagePeekBytes];
    const platform::FileTransfer read = file.ReadExact(header, sizeof(header));
    (void)file.Close();
    auto dimension = image::PeekImageDimension(Span<const uint8>(header, read.bytes));
    if (!dimension) {
        return;
    }

    TextureImportKind kind = TextureImportKind::Texture2D;
    switch (dimension.Value()) {
    case TextureDimension::TextureCube:
        kind = TextureImportKind::CubeFile;
        break;
    case TextureDimension::Texture2DArray:
        kind = TextureImportKind::ArrayFile;
        break;
    case TextureDimension::TextureCubeArray:
        kind = TextureImportKind::CubeArrayFile;
        break;
    case TextureDimension::Volume:
        kind = TextureImportKind::VolumeFile;
        break;
    case TextureDimension::Texture2D:
        return;
    }

    out.importer = TextureImporterNames[static_cast<usize>(kind)];
}

} // namespace

Result<bool> MakeTextureImportRequest(const TextureImportSpec& spec,
                                      AssetImportTranslation& out) noexcept {
    const usize files = spec.files.Size();
    bool valid = true;
    switch (spec.kind) {
    case TextureImportKind::Cube:
        valid = files == TextureCubeFaces;
        break;
    case TextureImportKind::Array:
    case TextureImportKind::Volume:
        valid = files >= 1;
        break;
    case TextureImportKind::CubeArray:
        valid = files >= TextureCubeFaces && files % TextureCubeFaces == 0;
        break;
    default:
        valid = files == 1;
        break;
    }

    const bool noOptions =
        spec.kind == TextureImportKind::ArrayFile || spec.kind == TextureImportKind::CubeArrayFile;
    const bool defaultInfill = spec.pngInfill == TextureImportPngInfill::OnlyOnBinaryTransparency;
    valid =
        valid && (spec.volumeTileX == 0) == (spec.volumeTileY == 0) &&
        (spec.kind == TextureImportKind::CubeFile || spec.cubeLayout == TextureCubeLayout::None) &&
        (spec.kind == TextureImportKind::VolumeFile || spec.volumeTileX == 0) &&
        (!noOptions || defaultInfill);
    for (StringView file : spec.files) {
        valid = valid && !file.IsEmpty();
    }

    if (!valid) {
        return Result<bool>::Success(false);
    }

    out.importer = TextureImporterNames[static_cast<usize>(spec.kind)];
    out.sources.Clear();
    out.options.Clear();
    if (auto reserved = out.sources.TryReserve(files); !reserved) {
        return Result<bool>::Failure(reserved.Error());
    }

    for (usize index = 0; index < files; ++index) {
        StringView role = DefaultImportSourceRole;
        if (spec.kind == TextureImportKind::Cube) {
            role = TextureCubeFaceRoles[index];
        } else if (spec.kind == TextureImportKind::Array ||
                   spec.kind == TextureImportKind::Volume ||
                   spec.kind == TextureImportKind::CubeArray) {
            auto text = NewString(out);
            if (!text) {
                return Result<bool>::Failure(text.Error());
            }

            const bool cube = spec.kind == TextureImportKind::CubeArray;
            Result<void> step =
                text.Value()->TryAppend(cube ? TextureCubeRolePrefix : TextureSliceRolePrefix);
            if (step) {
                step = AppendDecimal(*text.Value(), cube ? index / TextureCubeFaces : index);
            }

            if (step && cube) {
                step = text.Value()->TryAppend(".");
                if (step) {
                    step = text.Value()->TryAppend(TextureCubeFaceRoles[index % TextureCubeFaces]);
                }
            }

            if (!step) {
                return Result<bool>::Failure(step.Error());
            }

            role = text.Value()->View();
        }

        (void)out.sources.TryPushBack(AssetImportSourceInput{role, spec.files[index]});
    }

    Result<void> step = Result<void>::Success();
    if (spec.kind == TextureImportKind::CubeFile && spec.cubeLayout != TextureCubeLayout::None) {
        step = AddOption(out, "cubeLayout", CubeLayoutNames[static_cast<usize>(spec.cubeLayout)]);
    }

    if (step && spec.kind == TextureImportKind::VolumeFile && spec.volumeTileX != 0) {
        step = AddNumberOption(out, "volumeTileX", spec.volumeTileX);
        if (step) {
            step = AddNumberOption(out, "volumeTileY", spec.volumeTileY);
        }
    }

    if (step && !defaultInfill) {
        step = AddOption(out, "pngInfill", PngInfillNames[static_cast<usize>(spec.pngInfill)]);
    }

    if (!step) {
        return Result<bool>::Failure(step.Error());
    }

    return Result<bool>::Success(true);
}

Result<bool> TranslateTextureImportArguments(void* state, Allocator& allocator,
                                             Span<const AssetImportReservedArgument> reserved,
                                             Span<const AssetImportSourceInput> sources,
                                             StringView importer,
                                             AssetImportTranslation& out) noexcept {
    (void)state;
    const bool single = sources.Size() == 1 && sources[0].role == DefaultImportSourceRole;
    // 예약 인자 없음: DDS 원본 하나의 차원 선택만 한다.
    if (reserved.IsEmpty()) {
        if (single && importer.IsEmpty()) {
            ChooseDdsImporter(sources[0].path, out);
        }

        return Result<bool>::Success(true);
    }

    // 예약 인자는 `--importer=`와 함께 쓰지 않는다.
    if (!importer.IsEmpty()) {
        return Result<bool>::Success(false);
    }

    const AssetImportReservedArgument* multi = nullptr;
    const AssetImportReservedArgument* layout = nullptr;
    const AssetImportReservedArgument* tile = nullptr;
    for (const AssetImportReservedArgument& argument : reserved) {
        if (argument.name == "cube-layout") {
            layout = &argument;
        } else if (argument.name == "volume-tile") {
            tile = &argument;
        } else if (multi == nullptr) {
            multi = &argument;
        } else {
            return Result<bool>::Success(false);
        }
    }

    // 원본 하나 구성(배치·타일)은 위치 인자 원본과, 여러 원본 구성은 원본 없이 쓴다.
    if ((layout != nullptr && tile != nullptr) ||
        (multi != nullptr && (layout != nullptr || tile != nullptr || !sources.IsEmpty())) ||
        (multi == nullptr && !single)) {
        return Result<bool>::Success(false);
    }

    TextureImportSpec spec;
    Array<StringView> files(allocator);
    Result<bool> parsed = Result<bool>::Success(true);
    if (multi != nullptr) {
        if (multi->name == "cube") {
            spec.kind = TextureImportKind::Cube;
            parsed = ParseCube(multi->value, allocator, out, files);
        } else if (multi->name == "cube-array") {
            spec.kind = TextureImportKind::CubeArray;
            Array<StringView> cubes(allocator);
            if (!SplitRaw(multi->value, CubeSeparator, cubes)) {
                return Result<bool>::Success(false);
            }

            for (usize index = 0; index < cubes.Size() && parsed && parsed.Value(); ++index) {
                parsed = ParseCube(cubes[index], allocator, out, files);
            }
        } else {
            spec.kind =
                multi->name == "array" ? TextureImportKind::Array : TextureImportKind::Volume;
            parsed = ParseFileList(multi->value, allocator, out, files);
        }
    } else if (layout != nullptr) {
        spec.kind = TextureImportKind::CubeFile;
        usize found = IndexNone;
        usize index = 0;
        for (StringView name : CubeLayoutNames) {
            found = EqualsIgnoringAsciiCase(layout->value, name) ? index : found;
            ++index;
        }

        // None(DDS Cube)은 `--importer=texture_cube_file`로 고른다 — `--cube-layout=`은 배치
        // 셋만(13절).
        if (found == IndexNone || found == static_cast<usize>(TextureCubeLayout::None)) {
            return Result<bool>::Success(false);
        }

        spec.cubeLayout = static_cast<TextureCubeLayout>(found);
        if (auto pushed = files.TryPushBack(sources[0].path); !pushed) {
            return Result<bool>::Failure(pushed.Error());
        }
    } else {
        spec.kind = TextureImportKind::VolumeFile;
        const StringView value = tile->value;
        usize separator = IndexNone;
        for (usize index = 0; index < value.ByteCount() && separator == IndexNone; ++index) {
            separator = value.Data()[index] == TileSeparator ? index : IndexNone;
        }

        if (separator == IndexNone ||
            !ParsePositive(value.SliceBytes(0, separator), spec.volumeTileX) ||
            !ParsePositive(value.SliceBytes(separator + 1, value.ByteCount() - separator - 1),
                           spec.volumeTileY)) {
            return Result<bool>::Success(false);
        }

        if (auto pushed = files.TryPushBack(sources[0].path); !pushed) {
            return Result<bool>::Failure(pushed.Error());
        }
    }

    if (!parsed || !parsed.Value()) {
        return parsed;
    }

    spec.files = files.View();
    return MakeTextureImportRequest(spec, out);
}

} // namespace rocks
