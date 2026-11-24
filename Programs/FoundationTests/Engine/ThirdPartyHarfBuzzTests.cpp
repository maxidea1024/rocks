// HarfBuzz(Engine/Source/ThirdParty/HarfBuzz)의 연결 시험이다(Docs/Design/Font 8.2·8.5절, FT1b).
// HarfBuzz는 할당을 hb_*_impl hook(RocksHarfBuzzMalloc 등, extern "C")으로만 하며 HarfBuzz를
// 링크하는 모듈이 정의한다. 글꼴 backend(TextLayoutBackend, FT2)가 생기면 이 정의는 그 모듈로
// 옮기고 여기서 뺀다.
#include <Core/Containers/Array.h>
#include <Core/Memory/SystemAllocator.h>
#include <Platform/File.h>
#include <TestSupport/Test.h>
#include <hb-ft.h>
#include <hb-icu.h>
#include <hb-ot.h>
#include <hb.h>
#include <new>
#include <stddef.h>
#include <string.h>

using namespace rocks;

namespace {

// HarfBuzz 블록 정렬(malloc과 같은 보장)이다.
constexpr usize HarfBuzzBlockAlignment = alignof(max_align_t);

// realloc hook은 이전 크기를 주지 않으므로 블록 앞에 크기를 적는다.
constexpr usize HarfBuzzBlockHeader = alignof(max_align_t);

// HarfBuzz의 지연 정적 객체는 프로세스 종료 때 풀리므로, hook의 할당기는 프로세스 끝까지
// 산다(소멸하지 않는다).
SystemAllocator& HarfBuzzAllocator() noexcept {
    alignas(SystemAllocator) static unsigned char storage[sizeof(SystemAllocator)];
    static SystemAllocator* allocator = ::new (static_cast<void*>(storage)) SystemAllocator();
    return *allocator;
}

int64 harfBuzzAllocations = 0;

void* AllocateBlock(size_t size) noexcept {
    Result<void*> block =
        HarfBuzzAllocator().Allocate(HarfBuzzBlockHeader + size, HarfBuzzBlockAlignment);
    if (!block) {
        return nullptr;
    }

    ++harfBuzzAllocations;
    auto* bytes = static_cast<uint8*>(block.Value());
    memcpy(bytes, &size, sizeof(size));
    return bytes + HarfBuzzBlockHeader;
}

} // namespace

extern "C" void* RocksHarfBuzzMalloc(size_t size) {
    return AllocateBlock(size);
}

extern "C" void* RocksHarfBuzzCalloc(size_t count, size_t size) {
    if (size != 0 && count > static_cast<size_t>(-1) / size) {
        return nullptr;
    }

    void* block = AllocateBlock(count * size);
    if (block != nullptr) {
        memset(block, 0, count * size);
    }

    return block;
}

extern "C" void RocksHarfBuzzFree(void* memory) {
    if (memory != nullptr) {
        (void)HarfBuzzAllocator().Deallocate(static_cast<uint8*>(memory) - HarfBuzzBlockHeader);
    }
}

extern "C" void* RocksHarfBuzzRealloc(void* memory, size_t size) {
    void* resized = AllocateBlock(size);
    if (resized == nullptr || memory == nullptr) {
        return resized;
    }

    size_t previous = 0;
    memcpy(&previous, static_cast<uint8*>(memory) - HarfBuzzBlockHeader, sizeof(previous));
    memcpy(resized, memory, previous < size ? previous : size);
    RocksHarfBuzzFree(memory);
    return resized;
}

namespace third_party_harfbuzz_tests_file {

namespace {

constexpr const char* RobotoPath = "Engine/Source/ThirdParty/TestFontRoboto/"
                                   "roboto-3-classic-v3.016/hinted/static/Roboto-Regular.ttf";

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

} // namespace

TEST(ThirdParty_HarfBuzzVersionAndIcuUnicodeFunctions) {
    CHECK(hb_version_atleast(15, 2, 0));
    // HAVE_ICU_BUILTIN: 기본 유니코드 함수가 ICU다(Docs/Design/Font FD15).
    CHECK(hb_unicode_funcs_get_default() == hb_icu_get_unicode_funcs());
    // 아랍어 문자열의 script·방향을 ICU 속성으로 추정한다(데이터 등록 없이 동작하는 내장 속성).
    hb_buffer_t* buffer = hb_buffer_create();
    hb_buffer_add_utf8(buffer, "\xD8\xB3\xD9\x84\xD8\xA7\xD9\x85", -1, 0, -1);
    hb_buffer_guess_segment_properties(buffer);
    CHECK(hb_buffer_get_script(buffer) == HB_SCRIPT_ARABIC);
    CHECK(hb_buffer_get_direction(buffer) == HB_DIRECTION_RTL);
    hb_buffer_destroy(buffer);
    CHECK(harfBuzzAllocations > 0);
}

TEST(ThirdParty_HarfBuzzShapesLigatureWithTestFont) {
    SystemAllocator allocator;
    Array<uint8> bytes(allocator);
    CHECK(ReadFile(RobotoPath, bytes));
    if (bytes.Size() == 0) {
        return;
    }

    hb_blob_t* blob = hb_blob_create(reinterpret_cast<const char*>(bytes.Data()),
                                     static_cast<unsigned int>(bytes.Size()),
                                     HB_MEMORY_MODE_READONLY, nullptr, nullptr);
    hb_face_t* face = hb_face_create(blob, 0);
    hb_font_t* font = hb_font_create(face);
    CHECK(hb_face_get_upem(face) == 2048u);

    // "fi" 합자: 글자 3개("fin")가 글리프 2개(fi, n)가 된다(liga 기본 켬).
    hb_buffer_t* buffer = hb_buffer_create();
    hb_buffer_add_utf8(buffer, "fin", -1, 0, -1);
    hb_buffer_guess_segment_properties(buffer);
    hb_shape(font, buffer, nullptr, 0);
    CHECK(hb_buffer_get_length(buffer) == 2u);

    // liga를 끄면(자간이 0이 아닐 때 — Docs/Design/Font 6.4절) 글자마다 글리프 하나다.
    hb_buffer_reset(buffer);
    hb_buffer_add_utf8(buffer, "fin", -1, 0, -1);
    hb_buffer_guess_segment_properties(buffer);
    const hb_feature_t noLigatures = {HB_TAG('l', 'i', 'g', 'a'), 0, HB_FEATURE_GLOBAL_START,
                                      HB_FEATURE_GLOBAL_END};
    hb_shape(font, buffer, &noLigatures, 1);
    CHECK(hb_buffer_get_length(buffer) == 3u);
    unsigned int count = 0;
    const hb_glyph_position_t* positions = hb_buffer_get_glyph_positions(buffer, &count);
    CHECK(count == 3u && positions != nullptr && positions[0].x_advance > 0);

    hb_buffer_destroy(buffer);
    hb_font_destroy(font);
    hb_face_destroy(face);
    hb_blob_destroy(blob);
}

TEST(ThirdParty_HarfBuzzShapesArabicRightToLeft) {
    SystemAllocator allocator;
    Array<uint8> bytes(allocator);
    CHECK(ReadFile("Engine/Source/ThirdParty/TestFontNotoNaskhArabic/google-fonts-06bfe0d62a98/ofl/"
                   "notonaskharabic/NotoNaskhArabic-wght.ttf",
                   bytes));
    if (bytes.Size() == 0) {
        return;
    }

    hb_blob_t* blob = hb_blob_create(reinterpret_cast<const char*>(bytes.Data()),
                                     static_cast<unsigned int>(bytes.Size()),
                                     HB_MEMORY_MODE_READONLY, nullptr, nullptr);
    hb_face_t* face = hb_face_create(blob, 0);
    hb_font_t* font = hb_font_create(face);
    // "سلام"(글자 4개). RTL 버퍼는 글리프를 시각 순서(왼쪽부터)로 내므로 첫 글리프의 cluster가
    // 마지막 글자다. lam-alef를 글리프 하나로 합칠지는 글꼴 설계에 달려 있어(이 글꼴은 문맥형 두
    // 글리프) 세지 않는다.
    hb_buffer_t* buffer = hb_buffer_create();
    hb_buffer_add_utf8(buffer, "\xD8\xB3\xD9\x84\xD8\xA7\xD9\x85", -1, 0, -1);
    hb_buffer_guess_segment_properties(buffer);
    CHECK(hb_buffer_get_direction(buffer) == HB_DIRECTION_RTL);
    hb_shape(font, buffer, nullptr, 0);
    unsigned int count = 0;
    const hb_glyph_info_t* infos = hb_buffer_get_glyph_infos(buffer, &count);
    CHECK(count >= 3u && count <= 4u);
    CHECK(count != 0 && infos[0].cluster > infos[count - 1].cluster);

    // 문맥형: 단독 'س'와 낱말 첫머리 'س'는 다른 글리프다.
    hb_buffer_t* alone = hb_buffer_create();
    hb_buffer_add_utf8(alone, "\xD8\xB3", -1, 0, -1);
    hb_buffer_guess_segment_properties(alone);
    hb_shape(font, alone, nullptr, 0);
    unsigned int aloneCount = 0;
    const hb_glyph_info_t* aloneInfo = hb_buffer_get_glyph_infos(alone, &aloneCount);
    CHECK(aloneCount == 1u && count != 0 && aloneInfo[0].codepoint != infos[count - 1].codepoint);

    hb_buffer_destroy(alone);
    hb_buffer_destroy(buffer);
    hb_font_destroy(font);
    hb_face_destroy(face);
    hb_blob_destroy(blob);
}

} // namespace third_party_harfbuzz_tests_file
