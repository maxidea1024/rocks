#include <Core/Text/EditDistance.h>

#include <Platform/Fatal.h>

namespace rocks {

usize EditDistanceWithin(StringView left, StringView right, usize limit) noexcept {
    if (limit > MaxEditDistanceLimit) {
        platform::Fatal("EditDistanceWithin limit exceeds MaxEditDistanceLimit");
    }

    const usize over = limit + 1;
    const usize n = left.ByteCount();
    const usize m = right.ByteCount();
    if ((n > m ? n - m : m - n) > limit) {
        return over;
    }

    // 행 i의 칸 j(|j - i| ≤ limit)를 띠 번호 d = j - i + limit에 둔다. 한도를 넘는 값은 over로
    // 묶는다.
    constexpr usize BandCapacity = 2 * MaxEditDistanceLimit + 1;
    const usize width = 2 * limit + 1;
    usize previous[BandCapacity];
    usize current[BandCapacity];
    const auto lim = static_cast<isize>(limit);
    const auto columns = static_cast<isize>(m);
    for (usize d = 0; d < width; ++d) {
        const isize j = static_cast<isize>(d) - lim;
        previous[d] = j < 0 || j > columns
                          ? over
                          : (static_cast<usize>(j) < over ? static_cast<usize>(j) : over);
    }

    for (usize i = 1; i <= n; ++i) {
        usize rowMinimum = over;
        for (usize d = 0; d < width; ++d) {
            const isize j = static_cast<isize>(i) + static_cast<isize>(d) - lim;
            if (j < 0 || j > columns) {
                current[d] = over;
                continue;
            }

            usize value = over;
            if (j == 0) {
                value = i < over ? i : over;
            } else {
                // (i-1, j-1)은 같은 띠 번호, (i-1, j)는 d + 1, (i, j-1)은 d - 1이다.
                const bool same = left.Data()[i - 1] == right.Data()[static_cast<usize>(j) - 1];
                value = previous[d] + (same ? 0 : 1);
                if (d + 1 < width && previous[d + 1] + 1 < value) {
                    value = previous[d + 1] + 1;
                }

                if (d > 0 && current[d - 1] + 1 < value) {
                    value = current[d - 1] + 1;
                }

                value = value < over ? value : over;
            }

            current[d] = value;
            rowMinimum = value < rowMinimum ? value : rowMinimum;
        }

        if (rowMinimum >= over) {
            return over;
        }

        for (usize d = 0; d < width; ++d) {
            previous[d] = current[d];
        }
    }

    // 마지막 행(n)의 칸 m은 띠 번호 m - n + limit이다.
    return previous[static_cast<usize>(columns - static_cast<isize>(n) + lim)];
}

} // namespace rocks
