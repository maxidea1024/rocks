#include <Core/Text/EditDistance.h>

namespace rocks {

namespace {

// 비교 방식에 맞춘 byte다(ASCII 대소문자 무시면 영문 대문자를 소문자로).
char Fold(char value, EditDistanceCase textCase) noexcept {
    if (textCase == EditDistanceCase::AsciiInsensitive && value >= 'A' && value <= 'Z') {
        return static_cast<char>(value - 'A' + 'a');
    }

    return value;
}

} // namespace

usize EditDistance(StringView left, StringView right, usize limit,
                   EditDistanceCase textCase) noexcept {
    if (limit > MaxEditDistanceLimit) {
        limit = MaxEditDistanceLimit;
    }

    const usize over = limit + 1;
    const usize n = left.ByteCount();
    const usize m = right.ByteCount();
    if ((n > m ? n - m : m - n) > limit) {
        return over;
    }

    // 행 i의 칸 j(|j - i| ≤ limit)를 띠 번호 d = j - i + limit에 둔다. 한도를 넘는 값은 over로
    // 묶는다(띠 밖 칸의 거리는 길이 차이만으로 한도를 넘는다). 맞바꾸기 항은 두 행 앞(i - 2, j -
    // 2)을 보므로 세 행을 돌려 쓴다.
    constexpr usize BandCapacity = 2 * MaxEditDistanceLimit + 1;
    const usize width = 2 * limit + 1;
    usize rows[3][BandCapacity];
    usize* before = rows[0];
    usize* previous = rows[1];
    usize* current = rows[2];
    const auto lim = static_cast<isize>(limit);
    const auto columns = static_cast<isize>(m);
    for (usize d = 0; d < width; ++d) {
        const isize j = static_cast<isize>(d) - lim;
        previous[d] = j < 0 || j > columns
                          ? over
                          : (static_cast<usize>(j) < over ? static_cast<usize>(j) : over);
        before[d] = over;
    }

    const char* a = left.Data();
    const char* b = right.Data();
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
                // (i-1, j-1)과 (i-2, j-2)는 같은 띠 번호, (i-1, j)는 d + 1, (i, j-1)은 d - 1이다.
                const auto column = static_cast<usize>(j);
                const char leftChar = Fold(a[i - 1], textCase);
                const char rightChar = Fold(b[column - 1], textCase);
                value = previous[d] + (leftChar == rightChar ? 0 : 1);
                if (d + 1 < width && previous[d + 1] + 1 < value) {
                    value = previous[d + 1] + 1;
                }

                if (d > 0 && current[d - 1] + 1 < value) {
                    value = current[d - 1] + 1;
                }

                if (i >= 2 && column >= 2 && leftChar == Fold(b[column - 2], textCase) &&
                    Fold(a[i - 2], textCase) == rightChar && before[d] + 1 < value) {
                    value = before[d] + 1;
                }

                value = value < over ? value : over;
            }

            current[d] = value;
            rowMinimum = value < rowMinimum ? value : rowMinimum;
        }

        // 한 행이 모두 한도를 넘으면 뒤 행도 넘는다(맞바꾸기 항도 대각선 앞 칸 이상이다).
        if (rowMinimum >= over) {
            return over;
        }

        usize* const recycled = before;
        before = previous;
        previous = current;
        current = recycled;
    }

    // 마지막 행(n)의 칸 m은 띠 번호 m - n + limit이다.
    return previous[static_cast<usize>(columns - static_cast<isize>(n) + lim)];
}

} // namespace rocks
