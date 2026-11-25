#include <UiRender/VectorImageAtlas.h>

#include <Base/NumericLimits.h>
#include <Core/Containers/HashMap.h>
#include <Core/FileError.h>
#include <Core/FileRead.h>
#include <Core/Math/MathUtility.h>
#include <Core/Text/String.h>
#include <Core/Text/StringBuilder.h>
#include <Platform/File.h>
#include <TextLayout/FontAtlas.h>

#include <RocksNanoSvg.h>

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

Vec2 ImageSize(const RocksNanoSvgImage& image) noexcept {
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

    struct Page {
        UniquePtr<text::FontAtlasPage> pixels;
        SlotHandle slot{};
        graphics::TextureHandle texture{};
        bool dirty = true;
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

VectorImageAtlas::VectorImageAtlas(Allocator& allocator, graphics::Device& device,
                                   SlotArray<BrushTexture>& textures) noexcept
    : device_(device), textures_(textures) {
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
    const Vec2 parsedSize = image.parsed ? ImageSize(*image.parsed) : Vec2{};
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
    VectorImageAtlas* atlas = nullptr;
    platform::FileSystem* files = nullptr;
    Allocator* allocator = nullptr;
    // 지금 보는 디렉터리의 전체 경로와 키 경로 시작 위치다.
    char path[PathBufferBytes] = {};
    usize pathBytes = 0;
    usize rootBytes = 0;
    usize registered = 0;
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
        Array<uint8> bytes(*walk.allocator);
        bool existed = false;
        const StringView full(walk.path, walk.pathBytes);
        walk.status = ReadWholeFile(*walk.files, full, MaxVectorImageFileBytes, bytes, existed);
        if (walk.status && !existed) {
            walk.status = Result<void>::Failure(ErrorCode::NotFound);
        }

        if (walk.status) {
            // 키 경로는 root 뒤 '/'부터 ".svg" 앞까지다.
            const usize keyStart = walk.rootBytes + 1;
            const StringView key(walk.path + keyStart,
                                 walk.pathBytes - keyStart - SvgExtension.ByteCount());
            auto added = walk.atlas->Register(key, bytes.View());
            if (added) {
                ++walk.registered;
            } else {
                walk.status = Result<void>::Failure(added.Error());
            }
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

    DirectoryWalk walk;
    walk.atlas = this;
    walk.files = &files;
    walk.allocator = &state_->allocator;
    for (char byte : root) {
        walk.path[walk.pathBytes++] = byte;
    }

    walk.path[walk.pathBytes] = '\0';
    walk.rootBytes = walk.pathBytes;
    if (auto walked = Walk(walk); !walked) {
        return Result<usize>::Failure(walked.Error());
    }

    return Result<usize>::Success(walk.registered);
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

    const Vec2 source = ImageSize(*image.parsed);
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

    // 자리 찾기: 기존 페이지 → 새 페이지(한 변이 페이지보다 크면 그 크기의 단독 페이지).
    text::AtlasRect slot;
    usize pageIndex = MaxSize;
    for (usize i = 0; i < state.pages.Size(); ++i) {
        auto added = state.pages[i].pixels->Add(width, height, pixels.View(), slot);
        if (!added) {
            return Result<ResolvedVectorImage>::Failure(added.Error());
        }

        if (added.Value()) {
            pageIndex = i;
            break;
        }
    }

    if (pageIndex == MaxSize) {
        const uint32 padded = 2 * text::FontAtlasPadding;
        const uint32 pageWidth =
            width + padded > VectorAtlasPageSize ? width + padded : VectorAtlasPageSize;
        const uint32 pageHeight =
            height + padded > VectorAtlasPageSize ? height + padded : VectorAtlasPageSize;
        // 화소는 straight alpha RGBA다. 내용 종류는 byte 수(4)만 쓴다.
        auto page = text::FontAtlasPage::TryCreate(state.allocator, pageWidth, pageHeight,
                                                   text::FontAtlasContent::Color);
        if (!page) {
            return Result<ResolvedVectorImage>::Failure(page.Error());
        }

        auto added = page.Value()->Add(width, height, pixels.View(), slot);
        if (!added || !added.Value()) {
            return Result<ResolvedVectorImage>::Failure(added ? ErrorCode::Overflow
                                                              : added.Error());
        }

        // 자리는 지금 받고 텍스처는 Sync가 채운다.
        auto registered = textures_.TryAdd({});
        if (!registered) {
            return Result<ResolvedVectorImage>::Failure(registered.Error());
        }

        State::Page entry{Move(page.Value()), registered.Value(), {}, true};
        if (auto pushed = state.pages.TryPushBack(Move(entry)); !pushed) {
            (void)textures_.TryRemove(registered.Value());
            return Result<ResolvedVectorImage>::Failure(pushed.Error());
        }

        pageIndex = state.pages.Size() - 1;
    }

    State::Page& page = state.pages[pageIndex];
    page.dirty = true;
    const float pageWidth = static_cast<float>(page.pixels->Width());
    const float pageHeight = static_cast<float>(page.pixels->Height());
    const float left = static_cast<float>(slot.x + text::FontAtlasPadding);
    const float top = static_cast<float>(slot.y + text::FontAtlasPadding);
    ResolvedVectorImage resolved;
    resolved.resource = page.slot;
    resolved.uv = {{left / pageWidth, top / pageHeight},
                   {(left + static_cast<float>(width)) / pageWidth,
                    (top + static_cast<float>(height)) / pageHeight}};
    if (auto inserted = state.rasters.Insert(rasterKey, resolved); !inserted) {
        return Result<ResolvedVectorImage>::Failure(inserted.Error());
    }

    return Result<ResolvedVectorImage>::Success(resolved);
}

uint64 VectorImageAtlas::Revision() const noexcept {
    return state_ ? state_->revision : 0;
}

Result<void> VectorImageAtlas::Sync(graphics::PipelineHandle imagePipeline) noexcept {
    if (imagePipeline.IsNull()) {
        return Result<void>::Failure(ErrorCode::InvalidArgument);
    }

    if (!state_) {
        return Result<void>::Success();
    }

    for (State::Page& page : state_->pages) {
        if (!page.dirty) {
            continue;
        }

        text::AtlasRect region;
        (void)page.pixels->TakeDirtyRegion(region);
        if (!page.texture.IsNull()) {
            if (auto updated = device_.UpdateTexture(page.texture, 0, page.pixels->Pixels());
                !updated) {
                return updated;
            }

            page.dirty = false;
            continue;
        }

        graphics::TextureDesc desc;
        desc.width = page.pixels->Width();
        desc.height = page.pixels->Height();
        desc.format = graphics::PixelFormat::RGBA8UnormSrgb;
        desc.usage = graphics::TextureUsage::Sampled;
        auto created = device_.CreateTexture(desc, page.pixels->Pixels());
        if (!created) {
            return Result<void>::Failure(created.Error());
        }

        BrushTexture* entry = textures_.Find(page.slot);
        if (entry == nullptr) {
            device_.Retire(created.Value());
            return Result<void>::Failure(ErrorCode::StaleHandle);
        }

        *entry = {created.Value(), imagePipeline};
        page.texture = created.Value();
        page.dirty = false;
    }

    return Result<void>::Success();
}

bool VectorImageAtlas::AdvanceFrame() noexcept {
    if (!state_ || state_->pages.Size() <= MaxVectorAtlasPages) {
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

    const text::FontAtlasPage& page = *state_->pages[index].pixels;
    width = page.Width();
    height = page.Height();
    return page.Pixels();
}

void VectorImageAtlas::ReleasePages() noexcept {
    if (!state_) {
        return;
    }

    for (State::Page& page : state_->pages) {
        (void)textures_.TryRemove(page.slot);
        if (!page.texture.IsNull()) {
            device_.Retire(page.texture);
        }
    }

    state_->pages.Clear();
}

} // namespace rocks::ui
