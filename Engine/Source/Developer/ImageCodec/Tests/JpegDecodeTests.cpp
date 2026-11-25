#include <Core/Memory/SystemAllocator.h>
#include <ImageCodec/ImageDecode.h>
#include <Platform/Memory.h>
#include <Platform/Thread.h>
#include <TestSupport/NFailAllocator.h>
#include <TestSupport/Test.h>
#include <stdio.h>
#include <jpeglib.h>
#include <rocksjpeg.h>
#include <setjmp.h>

// JPEG 디코더 시험이다(Docs/Design/AssetSystem 12.11절 A5g — libjpeg-turbo). 파일은 libjpeg 부호화
// API로 시험 안에서 만든다: 회색·YCbCr 4:4:4·4:2:0·RGB(Adobe transform 0)·progressive·산술
// 부호·리셋 마커는 같은 계수를 푸는 경로라 baseline과 픽셀이 같다. 정밀도 8 이하 lossless(원본과
// 정확히 같고 값을 늘리지 않음), 비표준 sampling, 12비트·CMYK·YCCK·성분 2개·YCbCr lossless 거부,
// tables-only·머리 단계 경고·절단 손상, scan 상한, 한도, 할당 실패, 스레드별 묶음을 본다.
using namespace rocks;
using namespace rocks::image;
namespace jpeg_decode_tests_file {

namespace {

// ---- 시험 쪽 libjpeg 할당(jmemrocks 묶음) ----

constexpr usize TestBlockHeader = 16;

void* TestJpegAllocate(size_t bytes) noexcept {
    void* block = platform::AlignedAllocate(bytes + TestBlockHeader, TestBlockHeader);
    return block == nullptr ? nullptr : static_cast<uint8*>(block) + TestBlockHeader;
}

void TestJpegRelease(void* address) noexcept {
    if (address != nullptr) {
        platform::AlignedFree(static_cast<uint8*>(address) - TestBlockHeader);
    }
}

const RocksJpegAllocator testJpegAllocator = {TestJpegAllocate, TestJpegRelease};

class TestJpegBinding {
public:
    TestJpegBinding() noexcept : previous_(RocksJpegBindAllocator(&testJpegAllocator)) {
    }

    ~TestJpegBinding() noexcept {
        RocksJpegBindAllocator(previous_);
    }

private:
    const RocksJpegAllocator* previous_;
};

// ---- 부호화 오류: error_exit가 EncodeJpeg로 longjmp한다(기본 처리기의 exit()로 실행기를 끝내지
// 않는다) ----

// longjmp는 libjpeg(C) 프레임과 이 처리기만 건너뛴다 — setjmp를 둔 EncodeJpeg 프레임의 비자명
// 소멸자 객체는 setjmp 앞에 만들어 건너뛰지 않는다.
struct TestJpegError {
    jpeg_error_mgr manager;
    jmp_buf jump;
};

[[noreturn]] void TestJpegErrorExit(j_common_ptr cinfo) noexcept {
    longjmp(reinterpret_cast<TestJpegError*>(cinfo->err)->jump, 1);
}

void TestJpegOutputMessage(j_common_ptr) noexcept {
}

// ---- 메모리 목적지 ----

constexpr usize DestinationChunk = 4096;

struct MemoryDestination {
    jpeg_destination_mgr manager;
    Array<uint8>* data;
    usize written;
};

void DestinationInit(j_compress_ptr cinfo) noexcept {
    auto* destination = reinterpret_cast<MemoryDestination*>(cinfo->dest);
    CHECK(destination->data->TryResize(DestinationChunk));
    destination->written = 0;
    destination->manager.next_output_byte = destination->data->Data();
    destination->manager.free_in_buffer = destination->data->Size();
}

boolean DestinationEmpty(j_compress_ptr cinfo) noexcept {
    auto* destination = reinterpret_cast<MemoryDestination*>(cinfo->dest);
    const usize used = destination->data->Size();
    CHECK(destination->data->TryResize(used + DestinationChunk));
    destination->manager.next_output_byte = destination->data->Data() + used;
    destination->manager.free_in_buffer = DestinationChunk;
    return TRUE;
}

void DestinationTerm(j_compress_ptr cinfo) noexcept {
    auto* destination = reinterpret_cast<MemoryDestination*>(cinfo->dest);
    const usize used = destination->data->Size() - destination->manager.free_in_buffer;
    CHECK(destination->data->TryResize(used));
}

// 부호화할 그림이다. pixels는 행 우선 표본(components개 — precision 9–12면 uint16)이다.
struct JpegSpec {
    uint32 width = 8;
    uint32 height = 8;
    int components = 3;
    J_COLOR_SPACE input = JCS_RGB;
    J_COLOR_SPACE stored = JCS_UNKNOWN;
    bool setStored = false;
    bool progressive = false;
    bool arithmetic = false;
    unsigned restartInterval = 0;
    bool lossless = false;
    int precision = 8;
    bool fullChroma = false;
    int lumaHorizontal = 0;
    bool tablesOnly = false;
    const jpeg_scan_info* scans = nullptr;
    int scanCount = 0;
    Span<const uint8> pixels;
};

// setjmp와 longjmp 사이에 libjpeg가 건드리는 상태다. setjmp를 부르는 함수의 지역 변수로 두면 그
// 사이에 바뀐 값이 longjmp 뒤 불확정이므로(C17 7.13.2.1 — rocksjpeg.c와 같은 이유, Docs/Design/
// AssetSystem 12.11절) EncodeJpeg가 갖고 EncodeJpegWith는 참조로만 다룬다.
struct TestJpegEncoder {
    jpeg_compress_struct cinfo;
    TestJpegError error;
    MemoryDestination destination;
};

// spec을 out에 부호화한다. 부호화 오류(error_exit의 longjmp)면 거짓이다. 이 함수의 지역 변수는
// setjmp 뒤 바뀌어도 오류 경로에서 읽지 않는다.
bool EncodeJpegWith(TestJpegEncoder& encoder, const JpegSpec& spec, Array<uint8>& out) noexcept {
    jpeg_compress_struct& cinfo = encoder.cinfo;
    if (setjmp(encoder.error.jump) != 0) {
        jpeg_destroy_compress(&cinfo);
        return false;
    }

    jpeg_create_compress(&cinfo);
    MemoryDestination& destination = encoder.destination;
    destination.manager.init_destination = DestinationInit;
    destination.manager.empty_output_buffer = DestinationEmpty;
    destination.manager.term_destination = DestinationTerm;
    destination.data = &out;
    cinfo.dest = &destination.manager;
    cinfo.image_width = spec.width;
    cinfo.image_height = spec.height;
    cinfo.input_components = spec.components;
    cinfo.in_color_space = spec.input;
    cinfo.data_precision = spec.precision;
    jpeg_set_defaults(&cinfo);
    cinfo.data_precision = spec.precision;
    if (spec.setStored) {
        jpeg_set_colorspace(&cinfo, spec.stored);
    }

    if (spec.fullChroma) {
        cinfo.comp_info[0].h_samp_factor = 1;
        cinfo.comp_info[0].v_samp_factor = 1;
    }

    if (spec.lumaHorizontal != 0) {
        cinfo.comp_info[0].h_samp_factor = spec.lumaHorizontal;
        cinfo.comp_info[0].v_samp_factor = 1;
    }

    if (spec.lossless) {
        jpeg_enable_lossless(&cinfo, 1, 0);
    }

    if (spec.progressive) {
        jpeg_simple_progression(&cinfo);
    }

    if (spec.scans != nullptr) {
        cinfo.scan_info = spec.scans;
        cinfo.num_scans = spec.scanCount;
    }

    cinfo.arith_code = spec.arithmetic ? TRUE : FALSE;
    cinfo.restart_interval = spec.restartInterval;
    if (spec.tablesOnly) {
        jpeg_write_tables(&cinfo);
        jpeg_destroy_compress(&cinfo);
        return true;
    }

    jpeg_start_compress(&cinfo, TRUE);
    const usize rowSamples = static_cast<usize>(spec.width) * spec.components;
    while (cinfo.next_scanline < cinfo.image_height) {
        if (spec.precision > 8) {
            const auto* rows = reinterpret_cast<const uint16*>(spec.pixels.Data());
            J12SAMPROW row = const_cast<J12SAMPROW>(reinterpret_cast<const J12SAMPLE*>(rows) +
                                                    cinfo.next_scanline * rowSamples);
            jpeg12_write_scanlines(&cinfo, &row, 1);
        } else {
            JSAMPROW row =
                const_cast<JSAMPROW>(spec.pixels.Data() + cinfo.next_scanline * rowSamples);
            jpeg_write_scanlines(&cinfo, &row, 1);
        }
    }

    jpeg_finish_compress(&cinfo);
    jpeg_destroy_compress(&cinfo);
    return true;
}

Array<uint8> EncodeJpeg(Allocator& allocator, const JpegSpec& spec) noexcept {
    TestJpegBinding binding;
    Array<uint8> out(allocator);
    TestJpegEncoder encoder{};
    encoder.cinfo.err = jpeg_std_error(&encoder.error.manager);
    encoder.error.manager.error_exit = TestJpegErrorExit;
    encoder.error.manager.output_message = TestJpegOutputMessage;
    if (!EncodeJpegWith(encoder, spec, out)) {
        // 부호화 실패는 시험 실패로 보고하고 빈 결과를 돌려준다.
        CHECK(false);
        out.Clear();
    }

    return out;
}

struct Decoded {
    explicit Decoded(Allocator& allocator) noexcept : result(allocator) {
    }

    bool ok = false;
    ImageDecodeResult result;
};

Decoded Decode(Allocator& allocator, Span<const uint8> file,
               ImageDecodeLimits limits = {}) noexcept {
    Decoded out(allocator);
    out.ok = DecodeImage(allocator, file, ImageFileFormat::Unknown, limits, out.result).IsOk();
    return out;
}

bool Fails(const Decoded& decoded, AssetDiagnosticCode code) noexcept {
    return !decoded.ok && decoded.result.failure.code == code;
}

bool SamePixels(const Decoded& left, const Decoded& right) noexcept {
    return left.ok && right.ok &&
           left.result.image.pixels.Size() == right.result.image.pixels.Size() &&
           platform::MemoryEquals(left.result.image.pixels.Data(), right.result.image.pixels.Data(),
                                  left.result.image.pixels.Size());
}

// 8×8 RGB 그림이다(부드러운 기울기 — 양자화 뒤에도 값이 의미 있다).
struct RgbPicture {
    RgbPicture() noexcept {
        for (uint32 y = 0; y < 8; ++y) {
            for (uint32 x = 0; x < 8; ++x) {
                uint8* p = samples + (y * 8 + x) * 3;
                p[0] = static_cast<uint8>(x * 30);
                p[1] = static_cast<uint8>(y * 30);
                p[2] = static_cast<uint8>(128 + x * 8 - y * 8);
            }
        }
    }

    uint8 samples[8 * 8 * 3] = {};
};

} // namespace

// 같은 계수를 푸는 경로(baseline·progressive·산술·리셋)는 같은 픽셀이고, 회색·RGB 저장·4:4:4도
// 받는다.
TEST(JpegDecode_Variants) {
    SystemAllocator allocator;
    const RgbPicture picture;
    JpegSpec spec;
    spec.pixels = picture.samples;
    const Decoded baseline = Decode(allocator, EncodeJpeg(allocator, spec).View());
    CHECK(baseline.ok && baseline.result.image.format == TextureSourceFormat::RGBA8 &&
          baseline.result.image.width == 8 && baseline.result.image.height == 8 &&
          baseline.result.image.encoding == ImageColorEncoding::Unknown);
    if (baseline.ok) {
        // 알파는 255이고 값은 원본 근처다(양자화 오차).
        const uint8* p = baseline.result.image.pixels.Data();
        CHECK(p[3] == 255 && p[(7 * 8 + 7) * 4 + 0] > 160);
    }

    JpegSpec variant = spec;
    variant.progressive = true;
    CHECK(SamePixels(baseline, Decode(allocator, EncodeJpeg(allocator, variant).View())));
    variant = spec;
    variant.arithmetic = true;
    CHECK(SamePixels(baseline, Decode(allocator, EncodeJpeg(allocator, variant).View())));
    variant.progressive = true;
    CHECK(SamePixels(baseline, Decode(allocator, EncodeJpeg(allocator, variant).View())));
    variant = spec;
    variant.restartInterval = 1;
    CHECK(SamePixels(baseline, Decode(allocator, EncodeJpeg(allocator, variant).View())));

    // 4:4:4, RGB 저장(Adobe transform 0), 비표준 sampling(UE는 TJSAMP_UNKNOWN으로 실패 — Rocks
    // 상위집합).
    variant = spec;
    variant.fullChroma = true;
    CHECK(Decode(allocator, EncodeJpeg(allocator, variant).View()).ok);
    variant = spec;
    variant.setStored = true;
    variant.stored = JCS_RGB;
    CHECK(Decode(allocator, EncodeJpeg(allocator, variant).View()).ok);
    variant = spec;
    variant.width = 24;
    variant.lumaHorizontal = 3;
    uint8 wide[24 * 8 * 3] = {};
    variant.pixels = wide;
    CHECK(Decode(allocator, EncodeJpeg(allocator, variant).View()).ok);

    // 회색 → G8.
    uint8 gray[64] = {};
    for (uint32 index = 0; index < 64; ++index) {
        gray[index] = static_cast<uint8>(index * 4);
    }

    JpegSpec grayscale;
    grayscale.components = 1;
    grayscale.input = JCS_GRAYSCALE;
    grayscale.pixels = gray;
    const Decoded grayDecoded = Decode(allocator, EncodeJpeg(allocator, grayscale).View());
    CHECK(grayDecoded.ok && grayDecoded.result.image.format == TextureSourceFormat::G8 &&
          grayDecoded.result.image.pixels.Size() == 64);
}

// lossless: 정밀도 8은 원본과 정확히 같고, 정밀도 5는 값을 늘리지 않는다. YCbCr lossless·12비트는
// 받지 않는다.
TEST(JpegDecode_Lossless) {
    SystemAllocator allocator;
    const RgbPicture picture;
    JpegSpec spec;
    spec.pixels = picture.samples;
    spec.lossless = true;
    spec.setStored = true;
    spec.stored = JCS_RGB;
    const Decoded rgb = Decode(allocator, EncodeJpeg(allocator, spec).View());
    CHECK(rgb.ok && rgb.result.image.format == TextureSourceFormat::RGBA8);
    if (rgb.ok) {
        bool exact = true;
        for (uint32 texel = 0; texel < 64; ++texel) {
            for (uint32 c = 0; c < 3; ++c) {
                exact = exact &&
                        rgb.result.image.pixels[texel * 4 + c] == picture.samples[texel * 3 + c];
            }
        }

        CHECK(exact);
    }

    uint8 small[64] = {};
    for (uint32 index = 0; index < 64; ++index) {
        small[index] = static_cast<uint8>(index % 32);
    }

    JpegSpec five;
    five.components = 1;
    five.input = JCS_GRAYSCALE;
    five.lossless = true;
    five.precision = 5;
    five.pixels = small;
    const Decoded fiveDecoded = Decode(allocator, EncodeJpeg(allocator, five).View());
    CHECK(fiveDecoded.ok && fiveDecoded.result.image.pixels.Size() == 64 &&
          platform::MemoryEquals(fiveDecoded.result.image.pixels.Data(), small, 64));

    // YCbCr lossless(색 변환이 없어 RGB로 풀 수 없다 — UE도 실패).
    JpegSpec ycc = spec;
    ycc.input = JCS_YCbCr;
    ycc.stored = JCS_YCbCr;
    CHECK(Fails(Decode(allocator, EncodeJpeg(allocator, ycc).View()),
                AssetDiagnosticCode::UnsupportedSourceFormat));

    // 12비트(손실·lossless)는 8비트 API 밖이다.
    uint16 twelve[64] = {};
    for (uint32 index = 0; index < 64; ++index) {
        twelve[index] = static_cast<uint16>(index * 60);
    }

    JpegSpec deep;
    deep.components = 1;
    deep.input = JCS_GRAYSCALE;
    deep.precision = 12;
    deep.pixels = Span<const uint8>(reinterpret_cast<const uint8*>(twelve), sizeof(twelve));
    CHECK(Fails(Decode(allocator, EncodeJpeg(allocator, deep).View()),
                AssetDiagnosticCode::UnsupportedSourceFormat));
    deep.lossless = true;
    CHECK(Fails(Decode(allocator, EncodeJpeg(allocator, deep).View()),
                AssetDiagnosticCode::UnsupportedSourceFormat));
}

// 받지 않는 색 구성과 손상(tables-only·머리 단계 경고·절단), scan 상한, 한도.
TEST(JpegDecode_RejectionsAndDamage) {
    SystemAllocator allocator;
    uint8 cmyk[8 * 8 * 4] = {};
    JpegSpec spec;
    spec.components = 4;
    spec.input = JCS_CMYK;
    spec.pixels = cmyk;
    CHECK(Fails(Decode(allocator, EncodeJpeg(allocator, spec).View()),
                AssetDiagnosticCode::UnsupportedSourceFormat));
    spec.setStored = true;
    spec.stored = JCS_YCCK;
    CHECK(Fails(Decode(allocator, EncodeJpeg(allocator, spec).View()),
                AssetDiagnosticCode::UnsupportedSourceFormat));
    uint8 two[8 * 8 * 2] = {};
    JpegSpec pair;
    pair.components = 2;
    pair.input = JCS_UNKNOWN;
    pair.pixels = two;
    CHECK(Fails(Decode(allocator, EncodeJpeg(allocator, pair).View()),
                AssetDiagnosticCode::UnsupportedSourceFormat));

    // tables-only 스트림(require_image).
    const RgbPicture picture;
    JpegSpec tables;
    tables.pixels = picture.samples;
    tables.tablesOnly = true;
    CHECK(Fails(Decode(allocator, EncodeJpeg(allocator, tables).View()),
                AssetDiagnosticCode::InvalidSource));

    // 머리 단계 경고: JFIF 주 판 번호 이상(SOI 2 + 마커 2 + 길이 2 + "JFIF\0" 5 = 11번째 byte),
    // APP0 뒤 남는 byte.
    JpegSpec base;
    base.pixels = picture.samples;
    Array<uint8> file = EncodeJpeg(allocator, base);
    CHECK(Decode(allocator, file.View()).ok);
    constexpr usize JfifMajorOffset = 11;
    constexpr usize App0End = 20;
    Array<uint8> version(allocator);
    CHECK(version.TryAppend(file.View()));
    version[JfifMajorOffset] = 2;
    CHECK(Fails(Decode(allocator, version.View()), AssetDiagnosticCode::InvalidSource));
    Array<uint8> extra(allocator);
    CHECK(extra.TryAppend(file.View().Slice(0, App0End)));
    CHECK(extra.TryPushBack(0));
    CHECK(extra.TryAppend(file.View().Slice(App0End, file.Size() - App0End)));
    CHECK(Fails(Decode(allocator, extra.View()), AssetDiagnosticCode::InvalidSource));

    // 절단 전수(이른 끝은 경고 — InvalidSource).
    for (usize length = 0; length < file.Size(); ++length) {
        CHECK(!Decode(allocator, file.View().Slice(0, length)).ok);
    }

    // 한 변 한도.
    ImageDecodeLimits tight;
    tight.maxDimension = 4;
    const Decoded limited = Decode(allocator, file.View(), tight);
    CHECK(!limited.ok && limited.result.failure.error == ErrorCode::CapacityExceeded);

    // scan 상한: DC 11 scan + 성분마다 계수 63개 × (첫 scan + 다듬기 10) = 2090 scan(8비트 Ah·Al
    // 상한 10 — jcmaster.c 357).
    Array<jpeg_scan_info> scans(allocator);
    const auto add = [&scans](int count, int component, int ss, int se, int ah, int al) noexcept {
        jpeg_scan_info scan{};
        scan.comps_in_scan = count;
        for (int c = 0; c < count; ++c) {
            scan.component_index[c] = component < 0 ? c : component;
        }

        scan.Ss = ss;
        scan.Se = se;
        scan.Ah = ah;
        scan.Al = al;
        CHECK(scans.TryPushBack(scan));
    };
    constexpr int MaxSuccessiveApproximation = 10;
    add(3, -1, 0, 0, 0, MaxSuccessiveApproximation);
    for (int al = MaxSuccessiveApproximation - 1; al >= 0; --al) {
        add(3, -1, 0, 0, al + 1, al);
    }

    for (int component = 0; component < 3; ++component) {
        for (int k = 1; k < 64; ++k) {
            add(1, component, k, k, 0, MaxSuccessiveApproximation);
            for (int al = MaxSuccessiveApproximation - 1; al >= 0; --al) {
                add(1, component, k, k, al + 1, al);
            }
        }
    }

    JpegSpec many;
    many.pixels = picture.samples;
    many.fullChroma = true;
    many.scans = scans.Data();
    many.scanCount = static_cast<int>(scans.Size());
    const Decoded scanLimited = Decode(allocator, EncodeJpeg(allocator, many).View());
    CHECK(!scanLimited.ok &&
          scanLimited.result.failure.code == AssetDiagnosticCode::InvalidSource &&
          scanLimited.result.failure.error == ErrorCode::CapacityExceeded);
}

namespace {

struct ThreadJob {
    Span<const uint8> file;
    test::NFailAllocator* allocator = nullptr;
    bool ok = false;
};

void DecodeOnThread(void* context) noexcept {
    auto* job = static_cast<ThreadJob*>(context);
    ImageDecodeResult result(*job->allocator);
    job->ok = DecodeImage(*job->allocator, job->file, ImageFileFormat::Jpeg, {}, result).IsOk();
}

} // namespace

// 할당 실패(모든 시도가 OutOfMemory거나 같은 결과로 성공)와 스레드별 묶음(할당 횟수가 단독 실행과
// 같다).
TEST(JpegDecode_AllocationAndThreads) {
    SystemAllocator allocator;
    const RgbPicture picture;
    JpegSpec spec;
    spec.pixels = picture.samples;
    spec.progressive = true;
    const Array<uint8> file = EncodeJpeg(allocator, spec);
    const Decoded reference = Decode(allocator, file.View());
    CHECK(reference.ok);
    uint32 failures = 0;
    for (uint64 attempt = 1;; ++attempt) {
        test::NFailAllocator failing(allocator);
        failing.FailOnAttempt(attempt);
        ImageDecodeResult result(failing);
        auto decoded = DecodeImage(failing, file.View(), ImageFileFormat::Jpeg, {}, result);
        const bool exhausted = failing.Attempts() < attempt;
        failing.DisableFailure();
        if (exhausted) {
            CHECK(decoded);
            break;
        }

        if (decoded) {
            CHECK(platform::MemoryEquals(result.image.pixels.Data(),
                                         reference.result.image.pixels.Data(),
                                         reference.result.image.pixels.Size()));
            continue;
        }

        CHECK(decoded.Error() == ErrorCode::OutOfMemory &&
              result.failure.code == AssetDiagnosticCode::None);
        ++failures;
    }

    CHECK(failures > 0);

    JpegSpec other = spec;
    other.progressive = false;
    other.arithmetic = true;
    const Array<uint8> second = EncodeJpeg(allocator, other);
    test::NFailAllocator soloFirst(allocator);
    test::NFailAllocator soloSecond(allocator);
    ThreadJob solo[2] = {{file.View(), &soloFirst}, {second.View(), &soloSecond}};
    DecodeOnThread(&solo[0]);
    DecodeOnThread(&solo[1]);
    test::NFailAllocator sharedFirst(allocator);
    test::NFailAllocator sharedSecond(allocator);
    ThreadJob jobs[2] = {{file.View(), &sharedFirst}, {second.View(), &sharedSecond}};
    platform::Thread threads[2];
    for (uint32 index = 0; index < 2; ++index) {
        CHECK(threads[index].Start(DecodeOnThread, &jobs[index]) ==
              platform::ThreadStatus::Success);
    }

    for (platform::Thread& thread : threads) {
        CHECK(thread.Join() == platform::ThreadStatus::Success);
    }

    CHECK(solo[0].ok && solo[1].ok && jobs[0].ok && jobs[1].ok);
    CHECK(sharedFirst.Attempts() == soloFirst.Attempts() &&
          sharedSecond.Attempts() == soloSecond.Attempts());
}

} // namespace jpeg_decode_tests_file
