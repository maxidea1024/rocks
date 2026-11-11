#include <Core/Memory/SystemAllocator.h>
#include <Core/Path/Path.h>
#include <Tests/Support/Test.h>

using namespace rocks;

TEST(PathUnsupportedStyleFailsClosed) {
    SystemAllocator allocator;

    const PathStyle unsupported = PathStyle::Unsupported;
    auto root = Path::Root("a", unsupported);
    auto normalized = Path::Normalize(allocator, "a", unsupported);
    auto parent = Path::Parent("a", unsupported);
    auto relative = Path::MakeRelative(allocator, "/a", "/b", unsupported);
    auto contains = Path::IsUnder(allocator, "a", ".", unsupported);

    CHECK(!root && root.Error() == ErrorCode::InvalidArgument);
    CHECK(!normalized && normalized.Error() == ErrorCode::InvalidArgument);
    CHECK(!parent && parent.Error() == ErrorCode::InvalidArgument);
    CHECK(!relative && relative.Error() == ErrorCode::InvalidArgument);
    CHECK(!contains && contains.Error() == ErrorCode::InvalidArgument);

    const auto unknown = static_cast<PathStyle>(255);
    auto unknownRoot = Path::Root("a", unknown);
    CHECK(!unknownRoot && unknownRoot.Error() == ErrorCode::InvalidArgument);
}
