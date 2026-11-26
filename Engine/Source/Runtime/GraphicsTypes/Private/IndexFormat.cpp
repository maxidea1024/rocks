#include <GraphicsTypes/IndexFormat.h>

namespace rocks::graphics {

Result<IndexFormat> IndexFormatFromNumber(uint32 number) noexcept {
    if (number == 0 || number > IndexFormatCount) {
        return Result<IndexFormat>::Failure(ErrorCode::InvalidArgument);
    }

    return Result<IndexFormat>::Success(static_cast<IndexFormat>(number));
}

} // namespace rocks::graphics
