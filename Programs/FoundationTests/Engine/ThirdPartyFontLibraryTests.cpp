// 글꼴 외부 라이브러리(Engine/Source/ThirdParty — FreeType·LibPng)의 연결 시험이다(Docs/Design/Font
// 8.5절, FT1). 라이브러리를 엔진 경고 규칙으로 빌드해 이 실행 파일에 링크하고, adapter 헤더를 경고
// 0으로 include한다. 시험 글꼴은 저장소의 고정 파일(TestFontRoboto)만 읽는다(OS 설치 글꼴 미사용).
#include <Core/Containers/Array.h>
#include <Core/Memory/SystemAllocator.h>
#include <Platform/File.h>
#include <TestSupport/Test.h>
#include <freetype/freetype.h>
#include <freetype/ftglyph.h>
#include <freetype/ftmodapi.h>
#include <freetype/ftstroke.h>
#include <freetype/ftsynth.h>
#include <png.h>
#include <stddef.h>
#include <string.h>

using namespace rocks;
namespace third_party_font_library_tests_file {

namespace {

constexpr const char* RobotoPath = "Engine/Source/ThirdParty/TestFontRoboto/"
                                   "roboto-3-classic-v3.016/hinted/static/Roboto-Regular.ttf";

// 96 DPI에서 24 포인트(Docs/Design/Font 6.1절 크기 단위)다. FreeType 크기는 26.6 고정소수다.
constexpr FT_F26Dot6 TestCharSize = 24 * 64;
constexpr FT_UInt TestDpi = 96;

// 외곽선 시험의 stroke 반지름(2 px, 26.6 고정소수)이다.
constexpr FT_Fixed TestStrokeRadius = 2 * 64;

// libpng 1.6.59의 png_access_version_number 값이다.
constexpr png_uint_32 ExpectedLibPngVersion = 10659u;

// FreeType 블록 정렬(malloc과 같은 보장)이다.
constexpr usize FreeTypeBlockAlignment = alignof(max_align_t);

bool ReadFile(const char* path, Array<uint8>& out) noexcept {
    platform::File file;
    if (!file.Open(path, platform::FileOpenMode::ReadExisting)) {
        return false;
    }

    uint64 size = 0;
    bool ok = static_cast<bool>(file.Size(size)) &&
              static_cast<bool>(out.TryResize(static_cast<usize>(size)));
    if (ok && size != 0) {
        ok = static_cast<bool>(file.ReadExact(out.Data(), static_cast<usize>(size)).status);
    }

    const bool closed = static_cast<bool>(file.Close());
    return ok && closed;
}

// FT_Memory hook을 Rocks 할당기로 잇고, hook이 실제로 쓰이는지와 짝이 맞는지 센다(엔진 backend와
// 같은 연결 방식 — Docs/Design/Font 8.2절).
struct CountingMemory {
    FT_MemoryRec_ record{};
    SystemAllocator allocator;
    int64 live = 0;
    int64 total = 0;
};

void* CountingAlloc(FT_Memory memory, long size) {
    auto* counter = static_cast<CountingMemory*>(memory->user);
    Result<void*> block =
        counter->allocator.Allocate(static_cast<usize>(size), FreeTypeBlockAlignment);
    if (!block) {
        return nullptr;
    }

    ++counter->live;
    ++counter->total;
    return block.Value();
}

void CountingFree(FT_Memory memory, void* block) {
    auto* counter = static_cast<CountingMemory*>(memory->user);
    if (block == nullptr) {
        return;
    }

    --counter->live;
    (void)counter->allocator.Deallocate(block);
}

// FreeType이 이전 크기를 주므로 새 블록에 옮기고 이전 블록을 놓는다.
void* CountingRealloc(FT_Memory memory, long currentSize, long newSize, void* block) {
    void* resized = CountingAlloc(memory, newSize);
    if (resized == nullptr) {
        return nullptr;
    }

    if (block != nullptr) {
        memcpy(resized, block, static_cast<size_t>(currentSize < newSize ? currentSize : newSize));
        CountingFree(memory, block);
    }

    return resized;
}

// FT_New_Library + 기본 모듈 등록(FT_Init_FreeType의 malloc 기본 메모리 대신)이다.
bool CreateLibrary(CountingMemory& memory, FT_Library& library) {
    memory.record.user = &memory;
    memory.record.alloc = CountingAlloc;
    memory.record.free = CountingFree;
    memory.record.realloc = CountingRealloc;
    if (FT_New_Library(&memory.record, &library) != FT_Err_Ok) {
        return false;
    }

    FT_Add_Default_Modules(library);
    FT_Set_Default_Properties(library);
    return true;
}

} // namespace

TEST(ThirdParty_FreeTypeVersionAndModules) {
    CountingMemory memory;
    FT_Library library = nullptr;
    CHECK(CreateLibrary(memory, library));
    if (library == nullptr) {
        return;
    }

    FT_Int major = 0;
    FT_Int minor = 0;
    FT_Int patch = 0;
    FT_Library_Version(library, &major, &minor, &patch);
    CHECK(major == 2 && minor == 15 && patch == 0);
    // 수정한 ftmodule.h의 모듈만 등록된다(Docs/Design/Font 8.2절).
    CHECK(FT_Get_Module(library, "truetype") != nullptr);
    CHECK(FT_Get_Module(library, "cff") != nullptr);
    CHECK(FT_Get_Module(library, "autofitter") != nullptr);
    CHECK(FT_Get_Module(library, "smooth") != nullptr);
    CHECK(FT_Get_Module(library, "type1") == nullptr);
    CHECK(FT_Get_Module(library, "bdf") == nullptr);
    CHECK(FT_Get_Module(library, "svg") == nullptr);
    CHECK(memory.total > 0);
    CHECK(FT_Done_Library(library) == FT_Err_Ok);
    CHECK(memory.live == 0);
}

TEST(ThirdParty_FreeTypeInitWithoutRocksMemoryFails) {
    // Rocks system layer(RocksFtSystem.c)는 CRT 힙 메모리를 주지 않으므로 FT_Init_FreeType은
    // 실패하고, 모든 library는 FT_New_Library + Rocks FT_Memory로 만든다(Docs/Design/Font 8.2절).
    FT_Library library = nullptr;
    CHECK(FT_Init_FreeType(&library) != FT_Err_Ok);
    CHECK(library == nullptr);
}

TEST(ThirdParty_FreeTypeErrorStrings) {
    // FT_CONFIG_OPTION_ERROR_STRINGS를 켰으므로 오류 문구가 있다(로그에 원본 오류를 남길 때 쓴다).
    const char* text = FT_Error_String(FT_Err_Unknown_File_Format);
    CHECK(text != nullptr);
    CHECK(text != nullptr && strcmp(text, "unknown file format") == 0);
}

TEST(ThirdParty_FreeTypeOpensAndRastersTestFont) {
    SystemAllocator allocator;
    Array<uint8> bytes(allocator);
    CHECK(ReadFile(RobotoPath, bytes));
    if (bytes.Size() == 0) {
        return;
    }

    CountingMemory memory;
    FT_Library library = nullptr;
    CHECK(CreateLibrary(memory, library));
    if (library == nullptr) {
        return;
    }

    FT_Face face = nullptr;
    CHECK(FT_New_Memory_Face(library, bytes.Data(), static_cast<FT_Long>(bytes.Size()), 0, &face) ==
          FT_Err_Ok);
    if (face != nullptr) {
        CHECK(face->num_faces == 1);
        CHECK(face->num_glyphs > 0);
        CHECK(face->units_per_EM == 2048);
        CHECK(FT_IS_SCALABLE(face));
        CHECK(face->family_name != nullptr && strcmp(face->family_name, "Roboto") == 0);
        CHECK(FT_Set_Char_Size(face, 0, TestCharSize, TestDpi, TestDpi) == FT_Err_Ok);

        const FT_UInt glyph = FT_Get_Char_Index(face, 'A');
        CHECK(glyph != 0);
        CHECK(FT_Load_Glyph(face, glyph, FT_LOAD_TARGET_NORMAL | FT_LOAD_NO_BITMAP) == FT_Err_Ok);
        CHECK(FT_Render_Glyph(face->glyph, FT_RENDER_MODE_NORMAL) == FT_Err_Ok);
        CHECK(face->glyph->bitmap.pixel_mode == FT_PIXEL_MODE_GRAY);
        CHECK(face->glyph->bitmap.width > 0 && face->glyph->bitmap.rows > 0);

        // 단색(안티앨리어싱 끔) 경로: TARGET_MONO + FORCE_AUTOHINT(Docs/Design/Font 6.2절 표).
        CHECK(FT_Load_Glyph(face, glyph, FT_LOAD_TARGET_MONO | FT_LOAD_FORCE_AUTOHINT) ==
              FT_Err_Ok);
        CHECK(FT_Render_Glyph(face->glyph, FT_RENDER_MODE_MONO) == FT_Err_Ok);
        CHECK(face->glyph->bitmap.pixel_mode == FT_PIXEL_MODE_MONO);

        // 외곽선(FT_Stroker)과 합성 굵게(ftsynth)가 링크되어 동작한다.
        CHECK(FT_Load_Glyph(face, glyph, FT_LOAD_NO_BITMAP) == FT_Err_Ok);
        FT_Glyph outline = nullptr;
        CHECK(FT_Get_Glyph(face->glyph, &outline) == FT_Err_Ok);
        FT_Stroker stroker = nullptr;
        CHECK(FT_Stroker_New(library, &stroker) == FT_Err_Ok);
        if (outline != nullptr && stroker != nullptr) {
            FT_Stroker_Set(stroker, TestStrokeRadius, FT_STROKER_LINECAP_ROUND,
                           FT_STROKER_LINEJOIN_ROUND, 0);
            CHECK(FT_Glyph_Stroke(&outline, stroker, 1) == FT_Err_Ok);
            CHECK(FT_Glyph_To_Bitmap(&outline, FT_RENDER_MODE_NORMAL, nullptr, 1) == FT_Err_Ok);
            CHECK(outline->format == FT_GLYPH_FORMAT_BITMAP);
        }

        FT_Stroker_Done(stroker);
        FT_Done_Glyph(outline);
        CHECK(FT_Load_Glyph(face, glyph, FT_LOAD_NO_BITMAP) == FT_Err_Ok);
        const FT_Pos width = face->glyph->metrics.width;
        FT_GlyphSlot_Embolden(face->glyph);
        CHECK(face->glyph->metrics.width > width);
        CHECK(FT_Done_Face(face) == FT_Err_Ok);
    }

    CHECK(FT_Done_Library(library) == FT_Err_Ok);
    CHECK(memory.live == 0);
}

TEST(ThirdParty_FreeTypeRejectsNonFontBytes) {
    CountingMemory memory;
    FT_Library library = nullptr;
    CHECK(CreateLibrary(memory, library));
    if (library == nullptr) {
        return;
    }

    static const uint8 garbage[64] = {'n', 'o', 't', ' ', 'a', ' ', 'f', 'o', 'n', 't'};
    FT_Face face = nullptr;
    CHECK(FT_New_Memory_Face(library, garbage, sizeof(garbage), 0, &face) ==
          FT_Err_Unknown_File_Format);
    CHECK(face == nullptr);
    CHECK(FT_Done_Library(library) == FT_Err_Ok);
    CHECK(memory.live == 0);
}

TEST(ThirdParty_LibPngLinked) {
    // FreeType의 컬러 비트맵 글리프 경로(sfnt/pngshim.c)가 쓰는 libpng가 링크된다.
    CHECK(png_access_version_number() == ExpectedLibPngVersion);
}

namespace {

constexpr const char* GeneratedFonts =
    "Engine/Source/ThirdParty/TestFontGenerated/generated-2026-11-24/";
constexpr const char* NanumGothicPath =
    "Engine/Source/ThirdParty/TestFontNanumGothic/"
    "google-fonts-06bfe0d62a98/ofl/nanumgothic/NanumGothic-Regular.ttf";

// 생성 시험 글꼴 하나를 열어 fn(face)를 부르고 정리한다. 열기 실패는 CHECK로 알린다.
template <typename Function>
void WithFace(const char* path, FT_Long faceIndex, Function&& fn) {
    SystemAllocator allocator;
    Array<uint8> bytes(allocator);
    CHECK(ReadFile(path, bytes));
    if (bytes.Size() == 0) {
        return;
    }

    CountingMemory memory;
    FT_Library library = nullptr;
    CHECK(CreateLibrary(memory, library));
    if (library == nullptr) {
        return;
    }

    FT_Face face = nullptr;
    CHECK(FT_New_Memory_Face(library, bytes.Data(), static_cast<FT_Long>(bytes.Size()), faceIndex,
                             &face) == FT_Err_Ok);
    if (face != nullptr) {
        fn(face);
        CHECK(FT_Done_Face(face) == FT_Err_Ok);
    }

    CHECK(FT_Done_Library(library) == FT_Err_Ok);
    CHECK(memory.live == 0);
}

// 경로 앞부분과 파일 이름을 이은 경로다(시험 글꼴 경로용).
constexpr usize PathCapacity = 256;

const char* Join(char (&out)[PathCapacity], const char* directory, const char* name) {
    const size_t used = strlen(directory);
    memcpy(out, directory, used);
    memcpy(out + used, name, strlen(name) + 1);
    return out;
}

} // namespace

TEST(ThirdParty_FreeTypeLoadsColorBitmapGlyph) {
    // CBDT(PNG) 컬러 글리프: 고정 크기 strike를 고르고 FT_LOAD_COLOR로 읽으면 libpng가 PNG를 풀어
    // BGRA 비트맵이 된다(Docs/Design/Font 6.5절).
    char path[PathCapacity];
    WithFace(Join(path, GeneratedFonts, "NotoColorEmoji-U1F600.ttf"), 0, [](FT_Face face) {
        CHECK(FT_HAS_COLOR(face));
        CHECK(!FT_IS_SCALABLE(face) && face->num_fixed_sizes > 0);
        CHECK(FT_Select_Size(face, 0) == FT_Err_Ok);
        const FT_UInt glyph = FT_Get_Char_Index(face, 0x1F600);
        CHECK(glyph != 0);
        CHECK(FT_Load_Glyph(face, glyph, FT_LOAD_COLOR) == FT_Err_Ok);
        CHECK(face->glyph->bitmap.pixel_mode == FT_PIXEL_MODE_BGRA);
        CHECK(face->glyph->bitmap.width > 0 && face->glyph->bitmap.rows > 0);
    });
}

TEST(ThirdParty_FreeTypeOpensCollectionAndWebFonts) {
    char path[PathCapacity];
    // TTC 하위 face 둘(Roboto, Noto Naskh Arabic).
    WithFace(Join(path, GeneratedFonts, "Roboto-NotoNaskhArabic.ttc"), 0, [](FT_Face face) {
        CHECK(face->num_faces == 2);
        CHECK(face->family_name != nullptr && strcmp(face->family_name, "Roboto") == 0);
    });
    WithFace(path, 1, [](FT_Face face) {
        CHECK(face->family_name != nullptr && strcmp(face->family_name, "Noto Naskh Arabic") == 0);
    });
    // WOFF(zlib)·WOFF2(Brotli)는 FreeType이 열 때 sfnt로 푼다(Docs/Design/Font FD10, FT8).
    WithFace(Join(path, GeneratedFonts, "Roboto-Regular.woff"), 0, [](FT_Face face) {
        CHECK(face->family_name != nullptr && strcmp(face->family_name, "Roboto") == 0);
        CHECK(FT_Get_Char_Index(face, 'A') != 0);
    });
    WithFace(Join(path, GeneratedFonts, "Roboto-Regular.woff2"), 0, [](FT_Face face) {
        CHECK(face->family_name != nullptr && strcmp(face->family_name, "Roboto") == 0);
        CHECK(FT_Get_Char_Index(face, 'A') != 0);
    });
}

TEST(ThirdParty_FreeTypeOpensKoreanFont) {
    WithFace(NanumGothicPath, 0, [](FT_Face face) {
        // '한'(U+D55C)과 자모 'ㄱ'(U+3131)이 있다.
        CHECK(FT_Get_Char_Index(face, 0xD55C) != 0);
        CHECK(FT_Get_Char_Index(face, 0x3131) != 0);
        CHECK(FT_Set_Char_Size(face, 0, TestCharSize, TestDpi, TestDpi) == FT_Err_Ok);
        CHECK(FT_Load_Char(face, 0xD55C, FT_LOAD_RENDER) == FT_Err_Ok);
        CHECK(face->glyph->bitmap.width > 0);
    });
}

} // namespace third_party_font_library_tests_file
