#include "ImageCodecInternal.h"

#include <Core/Module/ModuleEntry.h>

// ImageCodec 모듈 진입점과 판별·분기다(Docs/Design/AssetSystem 12.9절).
ROCKS_IMPLEMENT_MODULE(rocks::DefaultModuleImplementation, ImageCodec)

namespace rocks::image {
namespace {

bool StartsWith(Span<const uint8> file, const char* signature, usize length) noexcept {
    if (file.Size() < length) {
        return false;
    }

    for (usize index = 0; index < length; ++index) {
        if (file[index] != static_cast<uint8>(signature[index])) {
            return false;
        }
    }

    return true;
}

bool EqualsAt(Span<const uint8> file, usize offset, const char* text, usize length) noexcept {
    if (offset > file.Size() || file.Size() - offset < length) {
        return false;
    }

    for (usize index = 0; index < length; ++index) {
        if (file[offset + index] != static_cast<uint8>(text[index])) {
            return false;
        }
    }

    return true;
}

bool HasExtension(StringView path, StringView extension) noexcept {
    return path.EndsWith(extension, StringComparison::IgnoreAsciiCase);
}

// WebP의 이미지 chunk 종류다(VP8 손실 · VP8L 무손실 — VP8X는 뒤따르는 chunk로 가른다).
Result<void> RejectWebP(Span<const uint8> file, ImageDecodeResult& out) noexcept {
    // RIFF(4) 크기(4) WEBP(4) 뒤 chunk들: fourcc(4) 크기(4 LE) 데이터(짝수로 채움).
    constexpr usize HeaderBytes = 12;
    usize at = HeaderBytes;
    while (at + 8 <= file.Size()) {
        const uint32 size = detail::LoadLe32(file.Data() + at + 4);
        if (EqualsAt(file, at, "VP8L", 4)) {
            return detail::Unsupported(out, at);
        }

        if (EqualsAt(file, at, "VP8 ", 4)) {
            return detail::Fail(out, AssetDiagnosticCode::LossySource, ErrorCode::Unsupported, at);
        }

        // ANMF(애니메이션 프레임)는 그 안의 첫 이미지 chunk로 가른다(프레임 머리 16 byte 뒤).
        if (EqualsAt(file, at, "ANMF", 4)) {
            constexpr usize FrameHeaderBytes = 16;
            at += 8 + FrameHeaderBytes;
            continue;
        }

        const uint64 next = uint64{at} + 8 + size + (size & 1u);
        if (next > file.Size()) {
            break;
        }

        at = static_cast<usize>(next);
    }

    return detail::Invalid(out, at);
}

} // namespace

const char* ImageFileFormatName(ImageFileFormat format) noexcept {
    switch (format) {
    case ImageFileFormat::Unknown:
        return "Unknown";
    case ImageFileFormat::Bmp:
        return "Bmp";
    case ImageFileFormat::Tga:
        return "Tga";
    case ImageFileFormat::Png:
        return "Png";
    case ImageFileFormat::Psd:
        return "Psd";
    case ImageFileFormat::Dds:
        return "Dds";
    case ImageFileFormat::Tiff:
        return "Tiff";
    case ImageFileFormat::Hdr:
        return "Hdr";
    case ImageFileFormat::Exr:
        return "Exr";
    case ImageFileFormat::Jpeg:
        return "Jpeg";
    case ImageFileFormat::WebP:
        return "WebP";
    }

    return "Unknown";
}

ImageFileFormat DetectImageFileFormat(Span<const uint8> file, StringView pathHint) noexcept {
    if (StartsWith(file, "\x89PNG\r\n\x1a\n", 8)) {
        return ImageFileFormat::Png;
    }

    if (StartsWith(file, "8BPS", 4)) {
        return ImageFileFormat::Psd;
    }

    if (StartsWith(file, "BM", 2)) {
        return ImageFileFormat::Bmp;
    }

    if (StartsWith(file, "DDS ", 4)) {
        return ImageFileFormat::Dds;
    }

    if (StartsWith(file, "II*\0", 4) || StartsWith(file, "MM\0*", 4)) {
        return ImageFileFormat::Tiff;
    }

    if (StartsWith(file, "#?RADIANCE", 10) || StartsWith(file, "#?RGBE", 6)) {
        return ImageFileFormat::Hdr;
    }

    if (StartsWith(file, "\x76\x2f\x31\x01", 4)) {
        return ImageFileFormat::Exr;
    }

    if (StartsWith(file, "\xff\xd8\xff", 3)) {
        return ImageFileFormat::Jpeg;
    }

    if (StartsWith(file, "RIFF", 4) && EqualsAt(file, 8, "WEBP", 4)) {
        return ImageFileFormat::WebP;
    }

    // TGA는 서명이 없다: 2.0 footer(끝 18 byte "TRUEVISION-XFILE.\0") 또는 확장자.
    constexpr usize FooterBytes = 18;
    if (file.Size() >= FooterBytes &&
        EqualsAt(file, file.Size() - FooterBytes, "TRUEVISION-XFILE.\0", FooterBytes)) {
        return ImageFileFormat::Tga;
    }

    const StringView extensions[] = {".tga", ".icb", ".vda", ".vst"};
    for (StringView extension : extensions) {
        if (HasExtension(pathHint, extension)) {
            return ImageFileFormat::Tga;
        }
    }

    return ImageFileFormat::Unknown;
}

Result<TextureDimension> PeekImageDimension(Span<const uint8> file,
                                            ImageDecodeFailure* failure) noexcept {
    if (DetectImageFileFormat(file) != ImageFileFormat::Dds) {
        return Result<TextureDimension>::Success(TextureDimension::Texture2D);
    }

    return detail::PeekDdsDimension(file, failure);
}

Result<void> DecodeImage(Allocator& allocator, Span<const uint8> file, ImageFileFormat format,
                         const ImageDecodeLimits& limits, ImageDecodeResult& out,
                         StringView pathHint) noexcept {
    out.warnings.Clear();
    out.failure = ImageDecodeFailure{};
    out.image.pixels.Clear();
    out.image.format = TextureSourceFormat::Invalid;
    out.image.dimension = TextureDimension::Texture2D;
    out.image.encoding = ImageColorEncoding::Unknown;
    out.image.width = 0;
    out.image.height = 0;
    out.image.numSlices = 1;
    out.image.sourceMips = 1;
    if (format == ImageFileFormat::Unknown) {
        format = DetectImageFileFormat(file, pathHint);
    }

    Result<void> decoded = Result<void>::Success();
    switch (format) {
    case ImageFileFormat::Bmp:
        decoded = detail::DecodeBmp(allocator, file, limits, out);
        break;
    case ImageFileFormat::Tga:
        decoded = detail::DecodeTga(allocator, file, limits, out);
        break;
    case ImageFileFormat::Png:
        decoded = detail::DecodePng(allocator, file, limits, out);
        break;
    case ImageFileFormat::Psd:
        decoded = detail::DecodePsd(allocator, file, limits, out);
        break;
    case ImageFileFormat::Dds:
        decoded = detail::DecodeDds(allocator, file, limits, out);
        break;
    case ImageFileFormat::Tiff:
        decoded = detail::DecodeTiff(allocator, file, limits, out);
        break;
    case ImageFileFormat::Hdr:
        decoded = detail::DecodeHdr(allocator, file, limits, out);
        break;
    case ImageFileFormat::Exr:
        decoded = detail::DecodeExr(allocator, file, limits, out);
        break;
    case ImageFileFormat::Jpeg:
        decoded = detail::DecodeJpeg(allocator, file, limits, out);
        break;
    case ImageFileFormat::WebP:
        decoded = RejectWebP(file, out);
        break;
    case ImageFileFormat::Unknown:
        decoded = detail::Unsupported(out, 0);
        break;
    }

    if (!decoded) {
        out.image.pixels.Clear();
        out.image.format = TextureSourceFormat::Invalid;
        out.image.dimension = TextureDimension::Texture2D;
        out.image.encoding = ImageColorEncoding::Unknown;
        out.image.width = 0;
        out.image.height = 0;
        out.image.numSlices = 1;
        out.image.sourceMips = 1;
    }

    return decoded;
}

} // namespace rocks::image
