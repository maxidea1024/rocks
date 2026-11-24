// msdfgen(Engine/Source/ThirdParty/Msdfgen)의 연결 시험이다(Docs/Design/Font 6.5·8.5절, FT1d).
// FreeType face의 윤곽을 msdfgen::Shape로 읽고(ext/import-font — UE SlateSdfGenerator와 같은 경로)
// 색 배정 뒤 MTSDF(4채널)를 만든다.
#include <Core/Containers/Array.h>
#include <Core/Memory/SystemAllocator.h>
#include <Platform/File.h>
#include <TestSupport/Test.h>
// import-font.h는 FreeType 헤더가 먼저 있어야 FT_Face를 받는 함수(adoptFreetypeFont)를 선언한다.
#include <freetype/freetype.h>
#include <freetype/ftmodapi.h>
// clang-format off
#include <ext/import-font.h>
// clang-format on
#include <msdfgen.h>
#include <stddef.h>
#include <string.h>

using namespace rocks;
namespace third_party_msdfgen_tests_file {

namespace {

constexpr const char* RobotoPath = "Engine/Source/ThirdParty/TestFontRoboto/"
                                   "roboto-3-classic-v3.016/hinted/static/Roboto-Regular.ttf";

// 거리장 한 변의 픽셀 수와 거리 범위(em 단위)다.
constexpr int FieldSize = 32;
constexpr double FieldRange = 0.125;

// MTSDF 판정 경계(거리 0 = 0.5)다.
constexpr float InsideThreshold = 0.5f;

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

struct FreeTypeMemory {
    FT_MemoryRec_ record{};
    SystemAllocator allocator;
};

void* FreeTypeAlloc(FT_Memory memory, long size) {
    Result<void*> block =
        static_cast<FreeTypeMemory*>(memory->user)
            ->allocator.Allocate(static_cast<usize>(size), FreeTypeBlockAlignment);
    return block ? block.Value() : nullptr;
}

void FreeTypeFree(FT_Memory memory, void* block) {
    if (block != nullptr) {
        (void)static_cast<FreeTypeMemory*>(memory->user)->allocator.Deallocate(block);
    }
}

void* FreeTypeRealloc(FT_Memory memory, long currentSize, long newSize, void* block) {
    void* resized = FreeTypeAlloc(memory, newSize);
    if (resized != nullptr && block != nullptr) {
        memcpy(resized, block, static_cast<size_t>(currentSize < newSize ? currentSize : newSize));
        FreeTypeFree(memory, block);
    }

    return resized;
}

} // namespace

TEST(ThirdParty_MsdfgenBuildsMtsdfFromFreeTypeOutline) {
    SystemAllocator allocator;
    Array<uint8> bytes(allocator);
    CHECK(ReadFile(RobotoPath, bytes));
    if (bytes.Size() == 0) {
        return;
    }

    FreeTypeMemory memory;
    memory.record.user = &memory;
    memory.record.alloc = FreeTypeAlloc;
    memory.record.free = FreeTypeFree;
    memory.record.realloc = FreeTypeRealloc;
    FT_Library library = nullptr;
    CHECK(FT_New_Library(&memory.record, &library) == FT_Err_Ok);
    if (library == nullptr) {
        return;
    }

    FT_Add_Default_Modules(library);
    FT_Face face = nullptr;
    CHECK(FT_New_Memory_Face(library, bytes.Data(), static_cast<FT_Long>(bytes.Size()), 0, &face) ==
          FT_Err_Ok);
    if (face != nullptr) {
        msdfgen::FontHandle* font = msdfgen::adoptFreetypeFont(face);
        CHECK(font != nullptr);
        msdfgen::Shape shape;
        double advance = 0;
        CHECK(msdfgen::loadGlyph(shape, font, static_cast<msdfgen::unicode_t>('O'),
                                 msdfgen::FONT_SCALING_EM_NORMALIZED, &advance));
        CHECK(advance > 0);
        // 'O'는 바깥·안쪽 윤곽 둘이다.
        CHECK(shape.contours.size() == 2u);
        shape.normalize();
        CHECK(shape.validate());
        msdfgen::edgeColoringSimple(shape, 3.0);

        // 글리프 경계 상자를 거리장 안에 맞춘다(가운데는 'O'의 구멍, 윤곽 위는 거리 0 근처).
        const msdfgen::Shape::Bounds bounds = shape.getBounds();
        const double width = bounds.r - bounds.l;
        const double height = bounds.t - bounds.b;
        const double scale = (FieldSize - 4) / (width > height ? width : height);
        const msdfgen::Vector2 translate(-bounds.l + 2 / scale, -bounds.b + 2 / scale);
        msdfgen::Bitmap<float, 4> field(FieldSize, FieldSize);
        msdfgen::generateMTSDF(field, shape,
                               msdfgen::SDFTransformation(msdfgen::Projection(scale, translate),
                                                          msdfgen::Range(FieldRange)));
        // 구석은 바깥(0.5 미만), 가운데(구멍)도 바깥, 고리 위 점은 안쪽이다(4번째 채널 = 실제
        // 거리).
        const float corner = field(0, 0)[3];
        const float center = field(FieldSize / 2, FieldSize / 2)[3];
        const float ring = field(FieldSize / 2, 3)[3];
        CHECK(corner < InsideThreshold);
        CHECK(center < InsideThreshold);
        CHECK(ring > InsideThreshold);
        msdfgen::destroyFont(font);
        CHECK(FT_Done_Face(face) == FT_Err_Ok);
    }

    CHECK(FT_Done_Library(library) == FT_Err_Ok);
}

} // namespace third_party_msdfgen_tests_file
