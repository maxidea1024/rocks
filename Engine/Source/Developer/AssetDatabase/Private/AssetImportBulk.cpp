#include <AssetDatabase/AssetImportBulk.h>

// 임포트 서비스의 bulk 옮기기다(AssetImportBulk.h).
namespace rocks {

Result<void> TransferImportBulk(AssetImportContext& context, ObjectHandle output,
                                MemoryBulkSource& bulk, BulkDataRef& ref,
                                Allocator& allocator) noexcept {
    auto bytes = bulk.ReadBulk(ref);
    if (!bytes) {
        return Result<void>::Failure(bytes.Error());
    }

    Array<uint8> copy(allocator);
    if (auto copied = copy.TryAppend(bytes.Value()); !copied) {
        return copied;
    }

    auto added = context.AddBulk(output, Move(copy));
    if (!added) {
        return Result<void>::Failure(added.Error());
    }

    ref = added.Value();
    return Result<void>::Success();
}

} // namespace rocks
