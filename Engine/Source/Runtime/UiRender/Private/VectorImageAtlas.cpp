#include <UiRender/VectorImageAtlas.h>

#include <Base/NumericLimits.h>
#include <Core/Containers/HashMap.h>
#include <Core/FileError.h>
#include <Core/FileRead.h>
#include <Core/Algorithms.h>
#include <Core/Math/MathUtility.h>
#include <Core/Text/String.h>
#include <Core/Text/StringBuilder.h>
#include <Platform/File.h>
#include <TextLayout/FontAtlas.h>

#include "RocksNanoSvg.h"

// 벡터 이미지 atlas다(Docs/Design/UiToolkit 17.2절).
namespace rocks::ui {

namespace {

// RGBA 화소 byte 수다.
constexpr uint32 RgbaBytes = 4;

// 키 경로 확장자다.
constexpr StringView SvgExtension = ".svg";

// 디렉터리 경로 버퍼 byte 수다.
constexpr usize PathBufferBytes = 1024;

// 래스터 자리 키다(그림, 화소 크기).
struct RasterKey {
    uint64 image = 0;
    uint32 width = 0;
    uint32 height = 0;

    friend bool operator==(const RasterKey&, const RasterKey&) noexcept = default;
};

struct RasterKeyHash {
    uint64 operator()(const RasterKey& key) const noexcept {
        return HashCombine(HashCombine(MixHash(key.image), key.width), key.height);
    }
};

// 파싱한 그림을 지우는 삭제기다.
struct ImageDeleter {
    void operator()(RocksNanoSvgImage* image) const noexcept {
        RocksNanoSvgDelete(image);
    }
};

// 원문을 0으로 끝나는 쓰기 가능한 사본으로 파싱한다(nanosvg가 입력을 바꾼다).
RocksNanoSvgImage* ParseCopy(Allocator& allocator, Span<const uint8> source) noexcept {
    Array<char> text(allocator);
    if (!text.TryResize(source.Size() + 1)) {
        return nullptr;
    }

    for (usize i = 0; i < source.Size(); ++i) {
        text[i] = static_cast<char>(source[i]);
    }

    text[source.Size()] = '\0';
    return RocksNanoSvgParse(text.Data());
}

Vec2 ParsedSize(const RocksNanoSvgImage& image) noexcept {
    Vec2 size;
    RocksNanoSvgSize(&image, &size.x, &size.y);
    return size;
}

} // namespace

struct VectorImageAtlas::State {
    struct Image {
        String path;
        VectorImageKey key{};
        Array<uint8> source;
        UniquePtr<RocksNanoSvgImage, ImageDeleter> parsed;
    };

    // atlas 페이지(guillotine 칸) 또는 큰 그림 하나의 단독 텍스처다(UE 비atlas 텍스처 — 여백 없이
    // 정확한 크기, uv 0..1).
    struct Page {
        UniquePtr<text::FontAtlasPage> atlas;
        Array<uint8> single;
        uint32 width = 0;
        uint32 height = 0;
        UiTextureId slot{};
        bool dirty = true;

        bool IsStandalone() const noexcept {
            return !atlas;
        }

        Span<const uint8> Pixels() const noexcept {
            return atlas ? atlas->Pixels() : single.View();
        }
    };

    explicit State(Allocator& memory) noexcept
        : allocator(memory), images(memory), lookup(memory), pages(memory), rasters(memory) {
    }

    ~State() noexcept {
        if (rasterizer != nullptr) {
            RocksNanoSvgDeleteRasterizer(rasterizer);
        }
    }

    Allocator& allocator;
    Array<Image> images;
    HashMap<uint64, usize> lookup;
    Array<Page> pages;
    HashMap<RasterKey, ResolvedVectorImage, RasterKeyHash> rasters;
    RocksNanoSvgRasterizer* rasterizer = nullptr;
    uint64 revision = 0;
    uint64 rasterCount = 0;
};

VectorImageAtlas::VectorImageAtlas(Allocator& allocator, UiRenderer& renderer) noexcept
    : renderer_(renderer) {
    auto state = UniquePtr<State>::TryMake(allocator, allocator);
    if (state) {
        state_ = Move(state.Value());
    }
}

VectorImageAtlas::~VectorImageAtlas() noexcept {
    ReleasePages();
}

Result<VectorImageKey> VectorImageAtlas::Register(StringView path, Span<const uint8> svg) noexcept {
    if (!state_) {
        return Result<VectorImageKey>::Failure(ErrorCode::OutOfMemory);
    }

    if (path.IsEmpty()) {
        return Result<VectorImageKey>::Failure(ErrorCode::InvalidArgument);
    }

    if (svg.Size() > MaxVectorImageFileBytes) {
        return Result<VectorImageKey>::Failure(ErrorCode::Overflow);
    }

    State& state = *state_;
    const VectorImageKey key = VectorImageKey::FromPath(path);
    if (state.lookup.Contains(key.hash)) {
        return Result<VectorImageKey>::Failure(ErrorCode::AlreadyExists);
    }

    State::Image image{String(state.allocator), key, Array<uint8>(state.allocator), {}};
    if (auto copied = image.path.TryAppend(path); !copied) {
        return Result<VectorImageKey>::Failure(copied.Error());
    }

    if (!image.source.TryResize(svg.Size())) {
        return Result<VectorImageKey>::Failure(ErrorCode::OutOfMemory);
    }

    for (usize i = 0; i < svg.Size(); ++i) {
        image.source[i] = svg[i];
    }

    image.parsed =
        UniquePtr<RocksNanoSvgImage, ImageDeleter>(ParseCopy(state.allocator, svg), ImageDeleter{});
    const Vec2 parsedSize = image.parsed ? ParsedSize(*image.parsed) : Vec2{};
    if (!image.parsed || !(parsedSize.x > 0) || !(parsedSize.y > 0) || !parsedSize.IsFinite()) {
        return Result<VectorImageKey>::Failure(ErrorCode::InvalidArgument);
    }

    if (auto inserted = state.lookup.Insert(key.hash, state.images.Size()); !inserted) {
        return Result<VectorImageKey>::Failure(inserted.Error());
    }

    if (auto added = state.images.TryPushBack(Move(image)); !added) {
        (void)state.lookup.Erase(key.hash);
        return Result<VectorImageKey>::Failure(added.Error());
    }

    return Result<VectorImageKey>::Success(key);
}

namespace {

struct DirectoryWalk {
    platform::FileSystem* files = nullptr;
    Allocator* allocator = nullptr;
    // 지금 보는 디렉터리의 전체 경로와 키 경로 시작 위치다.
    char path[PathBufferBytes] = {};
    usize pathBytes = 0;
    usize rootBytes = 0;
    // 찾은 SVG 파일의 전체 경로다(열거 순서는 플랫폼마다 달라 등록 전에 정렬한다).
    Array<String>* found = nullptr;
    Result<void> status = Result<void>::Success();
};

Result<void> Walk(DirectoryWalk& walk) noexcept;

bool VisitEntry(const platform::DirectoryEntry& entry, void* context) noexcept {
    auto& walk = *static_cast<DirectoryWalk*>(context);
    const StringView name(entry.name, entry.nameBytes);
    const bool directory = entry.info.type == platform::FileType::Directory && !entry.info.isLink;
    const bool svg = entry.info.type == platform::FileType::File &&
                     name.EndsWith(SvgExtension, StringComparison::IgnoreAsciiCase);
    if (!directory && !svg) {
        return true;
    }

    const usize saved = walk.pathBytes;
    if (saved + 1 + name.ByteCount() + 1 > PathBufferBytes) {
        walk.status = Result<void>::Failure(ErrorCode::Overflow);
        return false;
    }

    walk.path[walk.pathBytes++] = '/';
    for (char byte : name) {
        walk.path[walk.pathBytes++] = byte;
    }

    walk.path[walk.pathBytes] = '\0';
    if (directory) {
        walk.status = Walk(walk);
    } else {
        String full(*walk.allocator);
        walk.status = full.TryAppend(StringView(walk.path, walk.pathBytes));
        if (walk.status) {
            walk.status = walk.found->TryPushBack(Move(full));
        }
    }

    walk.pathBytes = saved;
    walk.path[saved] = '\0';
    return static_cast<bool>(walk.status);
}

Result<void> Walk(DirectoryWalk& walk) noexcept {
    const auto listed =
        walk.files->EnumerateDirectory(walk.path, walk.pathBytes, &VisitEntry, &walk);
    if (!walk.status) {
        return Result<void>::Failure(walk.status.Error());
    }

    return listed ? Result<void>::Success() : Result<void>::Failure(FileErrorCode(listed));
}

} // namespace

Result<usize> VectorImageAtlas::RegisterDirectory(platform::FileSystem& files,
                                                  StringView root) noexcept {
    if (!state_) {
        return Result<usize>::Failure(ErrorCode::OutOfMemory);
    }

    if (root.IsEmpty() || root.ByteCount() + 1 > PathBufferBytes) {
        return Result<usize>::Failure(ErrorCode::InvalidArgument);
    }

    bool exists = false;
    if (auto checked = files.DirectoryExists(root.Data(), root.ByteCount(), exists); !checked) {
        return Result<usize>::Failure(FileErrorCode(checked));
    }

    if (!exists) {
        return Result<usize>::Failure(ErrorCode::NotFound);
    }

    Array<String> found(state_->allocator);
    DirectoryWalk walk;
    walk.files = &files;
    walk.allocator = &state_->allocator;
    walk.found = &found;
    for (char byte : root) {
        walk.path[walk.pathBytes++] = byte;
    }

    walk.path[walk.pathBytes] = '\0';
    walk.rootBytes = walk.pathBytes;
    if (auto walked = Walk(walk); !walked) {
        return Result<usize>::Failure(walked.Error());
    }

    // 경로 byte 순으로 등록한다(갤러리 목록·캡처가 플랫폼과 무관하게 같은 순서).
    algo::Sort(found.View(), [](const String& left, const String& right) noexcept {
        return StringView::CompareBytes(left.View(), right.View()) < 0;
    });
    usize registered = 0;
    for (const String& path : found) {
        Array<uint8> bytes(state_->allocator);
        bool existed = false;
        auto read = ReadWholeFile(files, path.View(), MaxVectorImageFileBytes, bytes, existed);
        if (!read) {
            return Result<usize>::Failure(read.Error());
        }

        if (!existed) {
            return Result<usize>::Failure(ErrorCode::NotFound);
        }

        // 키 경로는 root 뒤 '/'부터 ".svg" 앞까지다.
        const usize keyStart = root.ByteCount() + 1;
        const StringView key = path.View().SliceBytes(keyStart, path.View().ByteCount() - keyStart -
                                                                    SvgExtension.ByteCount());
        auto added = Register(key, bytes.View());
        if (!added) {
            return Result<usize>::Failure(added.Error());
        }

        ++registered;
    }

    return Result<usize>::Success(registered);
}

usize VectorImageAtlas::ImageCount() const noexcept {
    return state_ ? state_->images.Size() : 0;
}

StringView VectorImageAtlas::ImagePath(usize index) const noexcept {
    return state_ && index < state_->images.Size() ? state_->images[index].path.View()
                                                   : StringView{};
}

VectorImageKey VectorImageAtlas::ImageKey(usize index) const noexcept {
    return state_ && index < state_->images.Size() ? state_->images[index].key : VectorImageKey{};
}

Vec2 VectorImageAtlas::ImageSize(usize index) const noexcept {
    if (!state_ || index >= state_->images.Size() || !state_->images[index].parsed) {
        return {};
    }

    return ParsedSize(*state_->images[index].parsed);
}

Result<ResolvedVectorImage> VectorImageAtlas::Resolve(VectorImageKey key, uint32 width,
                                                      uint32 height) noexcept {
    if (!state_) {
        return Result<ResolvedVectorImage>::Failure(ErrorCode::OutOfMemory);
    }

    if (width == 0 || height == 0 || width > MaxVectorImagePixels ||
        height > MaxVectorImagePixels) {
        return Result<ResolvedVectorImage>::Failure(ErrorCode::InvalidArgument);
    }

    State& state = *state_;
    const usize* index = state.lookup.Find(key.hash);
    if (index == nullptr) {
        return Result<ResolvedVectorImage>::Failure(ErrorCode::NotFound);
    }

    const RasterKey rasterKey{key.hash, width, height};
    if (const ResolvedVectorImage* found = state.rasters.Find(rasterKey)) {
        return Result<ResolvedVectorImage>::Success(*found);
    }

    // 래스터화: 가로·세로 배율을 따로 둔다(UE nsvgRasterizeFull과 같은 늘림). 같으면 nanosvg 배율
    // 인자로, 다르면 다시 파싱한 사본의 점에 곱한다. 그라데이션이 있는 그림은 비균등 변환을 하지
    // 않고 작은 배율로 가운데에 둔다(그라데이션 변환까지 늘리지 않는다 — 17.6절).
    const State::Image& image = state.images[*index];
    if (state.rasterizer == nullptr) {
        state.rasterizer = RocksNanoSvgCreateRasterizer();
        if (state.rasterizer == nullptr) {
            return Result<ResolvedVectorImage>::Failure(ErrorCode::OutOfMemory);
        }
    }

    Array<uint8> pixels(state.allocator);
    if (!pixels.TryResize(static_cast<usize>(width) * height * RgbaBytes)) {
        return Result<ResolvedVectorImage>::Failure(ErrorCode::OutOfMemory);
    }

    for (uint8& byte : pixels) {
        byte = 0;
    }

    const Vec2 source = ParsedSize(*image.parsed);
    const float sx = static_cast<float>(width) / source.x;
    const float sy = static_cast<float>(height) / source.y;
    const int stride = static_cast<int>(width * RgbaBytes);
    if (sx == sy) {
        RocksNanoSvgRasterize(state.rasterizer, image.parsed.Get(), 0, 0, sx, pixels.Data(),
                              static_cast<int>(width), static_cast<int>(height), stride);
    } else if (RocksNanoSvgHasGradient(image.parsed.Get()) != 0) {
        const float scale = math::Min(sx, sy);
        const float tx = (static_cast<float>(width) - source.x * scale) * 0.5f;
        const float ty = (static_cast<float>(height) - source.y * scale) * 0.5f;
        RocksNanoSvgRasterize(state.rasterizer, image.parsed.Get(), tx, ty, scale, pixels.Data(),
                              static_cast<int>(width), static_cast<int>(height), stride);
    } else {
        UniquePtr<RocksNanoSvgImage, ImageDeleter> scaled(
            ParseCopy(state.allocator, image.source.View()), ImageDeleter{});
        if (!scaled) {
            return Result<ResolvedVectorImage>::Failure(ErrorCode::OutOfMemory);
        }

        RocksNanoSvgScale(scaled.Get(), sx, sy);
        RocksNanoSvgRasterize(state.rasterizer, scaled.Get(), 0, 0, 1.0f, pixels.Data(),
                              static_cast<int>(width), static_cast<int>(height), stride);
    }

    ++state.rasterCount;

    // 텍스처 id는 지금 받고 화소는 Upload가 올린다(17.2절 프레임 순서).
    const auto addPage = [&](State::Page&& entry) noexcept -> Result<usize> {
        UiTextureDesc desc;
        desc.width = entry.width;
        desc.height = entry.height;
        desc.format = UiTextureFormat::Rgba8Srgb;
        desc.usage = UiTextureUsage::Dynamic;
        desc.sampling = UiTextureSampling::Image;
        auto registered = renderer_.CreateTexture(desc, {});
        if (!registered) {
            return Result<usize>::Failure(registered.Error());
        }

        entry.slot = registered.Value();
        if (auto pushed = state.pages.TryPushBack(Move(entry)); !pushed) {
            renderer_.ReleaseTexture(registered.Value());
            return Result<usize>::Failure(pushed.Error());
        }

        return Result<usize>::Success(state.pages.Size() - 1);
    };
    ResolvedVectorImage resolved;
    const uint32 padded = 2 * text::FontAtlasPadding;
    if (width + padded > VectorAtlasPageSize || height + padded > VectorAtlasPageSize) {
        // 페이지에 들지 않는 그림은 정확한 크기의 단독 텍스처다(UE CreateNonAtlasedTexture).
        State::Page entry{{}, Move(pixels), width, height, {}, true};
        auto added = addPage(Move(entry));
        if (!added) {
            return Result<ResolvedVectorImage>::Failure(added.Error());
        }

        resolved.resource = state.pages[added.Value()].slot;
        resolved.uv = {{0, 0}, {1, 1}};
    } else {
        text::AtlasRect slot;
        usize pageIndex = MaxSize;
        for (usize i = 0; i < state.pages.Size(); ++i) {
            if (state.pages[i].IsStandalone()) {
                continue;
            }

            auto placed = state.pages[i].atlas->Add(width, height, pixels.View(), slot);
            if (!placed) {
                return Result<ResolvedVectorImage>::Failure(placed.Error());
            }

            if (placed.Value()) {
                pageIndex = i;
                break;
            }
        }

        if (pageIndex == MaxSize) {
            // 화소는 straight alpha RGBA다. 내용 종류는 byte 수(4)만 쓴다.
            auto page =
                text::FontAtlasPage::TryCreate(state.allocator, VectorAtlasPageSize,
                                               VectorAtlasPageSize, text::FontAtlasContent::Color);
            if (!page) {
                return Result<ResolvedVectorImage>::Failure(page.Error());
            }

            auto placed = page.Value()->Add(width, height, pixels.View(), slot);
            if (!placed || !placed.Value()) {
                return Result<ResolvedVectorImage>::Failure(placed ? ErrorCode::Overflow
                                                                   : placed.Error());
            }

            State::Page entry{Move(page.Value()),
                              Array<uint8>(state.allocator),
                              VectorAtlasPageSize,
                              VectorAtlasPageSize,
                              {},
                              true};
            auto added = addPage(Move(entry));
            if (!added) {
                return Result<ResolvedVectorImage>::Failure(added.Error());
            }

            pageIndex = added.Value();
        }

        State::Page& page = state.pages[pageIndex];
        page.dirty = true;
        const float pageSize = static_cast<float>(VectorAtlasPageSize);
        const float left = static_cast<float>(slot.x + text::FontAtlasPadding);
        const float top = static_cast<float>(slot.y + text::FontAtlasPadding);
        resolved.resource = page.slot;
        resolved.uv = {{left / pageSize, top / pageSize},
                       {(left + static_cast<float>(width)) / pageSize,
                        (top + static_cast<float>(height)) / pageSize}};
    }

    if (auto inserted = state.rasters.Insert(rasterKey, resolved); !inserted) {
        return Result<ResolvedVectorImage>::Failure(inserted.Error());
    }

    return Result<ResolvedVectorImage>::Success(resolved);
}

uint64 VectorImageAtlas::Revision() const noexcept {
    return state_ ? state_->revision : 0;
}

Result<void> VectorImageAtlas::Upload() noexcept {
    if (!state_) {
        return Result<void>::Success();
    }

    for (State::Page& page : state_->pages) {
        if (!page.dirty) {
            continue;
        }

        if (page.atlas) {
            text::AtlasRect region;
            (void)page.atlas->TakeDirtyRegion(region);
        }

        if (auto updated = renderer_.UpdateTexture(page.slot, page.Pixels()); !updated) {
            return updated;
        }

        page.dirty = false;
    }

    return Result<void>::Success();
}

bool VectorImageAtlas::AdvanceFrame() noexcept {
    if (!state_) {
        return false;
    }

    usize standalone = 0;
    for (const State::Page& page : state_->pages) {
        standalone += page.IsStandalone() ? 1 : 0;
    }

    const usize atlasPages = state_->pages.Size() - standalone;
    if (atlasPages <= MaxVectorAtlasPages && standalone <= MaxVectorStandaloneImages) {
        return false;
    }

    ReleasePages();
    state_->rasters.Clear();
    ++state_->revision;
    return true;
}

usize VectorImageAtlas::PageCount() const noexcept {
    return state_ ? state_->pages.Size() : 0;
}

uint64 VectorImageAtlas::RasterCount() const noexcept {
    return state_ ? state_->rasterCount : 0;
}

Span<const uint8> VectorImageAtlas::PagePixels(usize index, uint32& width,
                                               uint32& height) const noexcept {
    width = 0;
    height = 0;
    if (!state_ || index >= state_->pages.Size()) {
        return {};
    }

    const State::Page& page = state_->pages[index];
    width = page.width;
    height = page.height;
    return page.Pixels();
}

void VectorImageAtlas::ReleasePages() noexcept {
    if (!state_) {
        return;
    }

    for (State::Page& page : state_->pages) {
        renderer_.ReleaseTexture(page.slot);
    }

    state_->pages.Clear();
}

} // namespace rocks::ui
