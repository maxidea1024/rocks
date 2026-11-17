#include <Core/Math/RandomStream.h>
#include <Core/Math/Transform.h>
#include <Platform/Clock.h>
#include <Platform/Memory.h>

#include <stdio.h>

// float·double Transform 마이크로벤치마크다(Docs/Implementation/Math.ko.md 5절). UE FTransform처럼
// 모든 성분이 double인 Transform과 대량 경로용 Transform3f의 비용을 같은 입력으로 잰다.
// - multiply: TryMultiply(local, parent) — 행렬 곱 뒤 TRS 분해(shear 판정 포함).
// - inverse: TryInverse — 역 affine 구성 뒤 분해.
// - point: TransformPoint — scale · 회전 · 이동.
// - matrix: ToMatrix 두 개의 곱(Mat4 float SIMD / Mat4d scalar) — 분해를 뺀 행렬 경로 참고값.
// 단일 스레드에서 변환 Count개 배열을 passes번 지나는 시간을 rounds번 재 가장 짧은 값을 연산당
// ns로 보고한다. 결과는 기기·빌드 구성에 따라 다르므로 같은 기기·Release에서 비교한다. 입력은
// 고정 seed라 매번 같다. 결과를 sink에 모아 출력해 계산이 지워지지 않게 한다.
//
// 사용:
//   MathBench                 기본(5회 중 최고값, 배열 4096개 × 64회)
//   MathBench --rounds 9      반복 횟수(최고값을 보고한다)
//   MathBench --passes 256    배열을 지나는 횟수
namespace {

using namespace rocks;

constexpr usize Count = 4096;

struct Options {
    uint32 rounds = 5;
    usize passes = 64;
};

struct Inputs {
    Transform parents[Count];
    Transform locals[Count];
    Transform3f parentsSingle[Count];
    Transform3f localsSingle[Count];
    Vec3d points[Count];
    Vec3 pointsSingle[Count];
};

// 부모는 균일 scale(합성이 shear 없이 TRS로 표현된다), 자식은 비균일 scale이다.
void Fill(Inputs& inputs) noexcept {
    RandomStream random(0x6d61746862656e63ull);
    auto range = [&random](double low, double high) {
        return low + (high - low) * random.NextDouble01();
    };
    auto rotation = [&]() {
        for (;;) {
            const Quatd candidate(range(-1.0, 1.0), range(-1.0, 1.0), range(-1.0, 1.0),
                                  range(-1.0, 1.0));
            auto normalized = candidate.TryNormalize();
            if (candidate.LengthSquared() > 0.01 && normalized) {
                return normalized.Value();
            }
        }
    };
    for (usize index = 0; index < Count; ++index) {
        const double uniform = range(0.5, 2.0);
        inputs.parents[index] = Transform(
            rotation(), Vec3d(range(-1.0e4, 1.0e4), range(-1.0e4, 1.0e4), range(-1.0e4, 1.0e4)),
            Vec3d::Splat(uniform));
        inputs.locals[index] = Transform(
            rotation(), Vec3d(range(-100.0, 100.0), range(-100.0, 100.0), range(0.0, 50.0)),
            Vec3d(range(0.5, 2.0), range(0.5, 2.0), range(0.5, 2.0)));
        inputs.parentsSingle[index] = inputs.parents[index].RoundToTransform3f();
        inputs.localsSingle[index] = inputs.locals[index].RoundToTransform3f();
        inputs.points[index] = Vec3d(range(-50.0, 50.0), range(-50.0, 50.0), range(-50.0, 50.0));
        inputs.pointsSingle[index] = Vec3(inputs.points[index]);
    }
}

double Seconds(uint64 start) {
    return static_cast<double>(platform::MonotonicNanoseconds() - start) * 1e-9;
}

// body를 rounds번 실행해 가장 짧은 시간을 연산당 ns로 돌려준다. body는 연산 수를 돌려준다.
template <typename Body>
double BestNanoseconds(uint32 rounds, Body&& body) {
    double best = NumericLimits<double>::Max();
    for (uint32 round = 0; round < rounds; ++round) {
        const uint64 start = platform::MonotonicNanoseconds();
        const usize operations = body();
        const double perOperation = Seconds(start) * 1e9 / static_cast<double>(operations);
        best = perOperation < best ? perOperation : best;
    }

    return best;
}

bool ParseNumber(const char* text, usize low, usize high, usize& value) {
    value = 0;
    for (const char* digit = text; *digit != '\0'; ++digit) {
        if (*digit < '0' || *digit > '9' || value > high) {
            return false;
        }

        value = value * 10 + static_cast<usize>(*digit - '0');
    }

    return value >= low && value <= high;
}

bool Parse(int argc, char** argv, Options& options) {
    for (int index = 1; index < argc; ++index) {
        const bool hasValue = index + 1 < argc;
        usize value = 0;
        if (platform::CStringEquals(argv[index], "--rounds") && hasValue) {
            if (!ParseNumber(argv[++index], 1, 100, value)) {
                return false;
            }

            options.rounds = static_cast<uint32>(value);
        } else if (platform::CStringEquals(argv[index], "--passes") && hasValue) {
            if (!ParseNumber(argv[++index], 1, 100000, value)) {
                return false;
            }

            options.passes = value;
        } else {
            return false;
        }
    }

    return true;
}

int Run(const Options& options) {
    static Inputs inputs;
    Fill(inputs);
    double sink = 0.0;
    usize failures = 0;
    const usize operations = Count * options.passes;

    const double multiplyDouble = BestNanoseconds(options.rounds, [&]() {
        for (usize pass = 0; pass < options.passes; ++pass) {
            for (usize index = 0; index < Count; ++index) {
                auto world = Transform::TryMultiply(inputs.locals[index], inputs.parents[index]);
                failures += world ? 0 : 1;
                sink += world ? world.Value().translation.x : 0.0;
            }
        }

        return operations;
    });
    const double multiplySingle = BestNanoseconds(options.rounds, [&]() {
        for (usize pass = 0; pass < options.passes; ++pass) {
            for (usize index = 0; index < Count; ++index) {
                auto world = Transform3f::TryMultiply(inputs.localsSingle[index],
                                                      inputs.parentsSingle[index]);
                failures += world ? 0 : 1;
                sink += world ? static_cast<double>(world.Value().translation.x) : 0.0;
            }
        }

        return operations;
    });
    const double inverseDouble = BestNanoseconds(options.rounds, [&]() {
        for (usize pass = 0; pass < options.passes; ++pass) {
            for (usize index = 0; index < Count; ++index) {
                auto inverse = inputs.parents[index].TryInverse();
                failures += inverse ? 0 : 1;
                sink += inverse ? inverse.Value().translation.y : 0.0;
            }
        }

        return operations;
    });
    const double inverseSingle = BestNanoseconds(options.rounds, [&]() {
        for (usize pass = 0; pass < options.passes; ++pass) {
            for (usize index = 0; index < Count; ++index) {
                auto inverse = inputs.parentsSingle[index].TryInverse();
                failures += inverse ? 0 : 1;
                sink += inverse ? static_cast<double>(inverse.Value().translation.y) : 0.0;
            }
        }

        return operations;
    });
    const double pointDouble = BestNanoseconds(options.rounds, [&]() {
        for (usize pass = 0; pass < options.passes; ++pass) {
            for (usize index = 0; index < Count; ++index) {
                sink += inputs.locals[index].TransformPoint(inputs.points[index]).z;
            }
        }

        return operations;
    });
    const double pointSingle = BestNanoseconds(options.rounds, [&]() {
        for (usize pass = 0; pass < options.passes; ++pass) {
            for (usize index = 0; index < Count; ++index) {
                sink += static_cast<double>(
                    inputs.localsSingle[index].TransformPoint(inputs.pointsSingle[index]).z);
            }
        }

        return operations;
    });
    const double matrixDouble = BestNanoseconds(options.rounds, [&]() {
        for (usize pass = 0; pass < options.passes; ++pass) {
            for (usize index = 0; index < Count; ++index) {
                const Mat4d product =
                    inputs.locals[index].ToMatrix() * inputs.parents[index].ToMatrix();
                sink += product.m[13];
            }
        }

        return operations;
    });
    const double matrixSingle = BestNanoseconds(options.rounds, [&]() {
        for (usize pass = 0; pass < options.passes; ++pass) {
            for (usize index = 0; index < Count; ++index) {
                const Mat4 product =
                    inputs.localsSingle[index].ToMatrix() * inputs.parentsSingle[index].ToMatrix();
                sink += static_cast<double>(product.m[13]);
            }
        }

        return operations;
    });

    printf("Transform (double, UE FTransform) vs Transform3f (float), %zu x %zu ops, best of %u, "
           "single thread\n",
           Count, options.passes, options.rounds);
    printf("%-10s %14s %14s %8s\n", "operation", "double ns/op", "float ns/op", "ratio");
    printf("%-10s %14.2f %14.2f %8.2f\n", "multiply", multiplyDouble, multiplySingle,
           multiplyDouble / multiplySingle);
    printf("%-10s %14.2f %14.2f %8.2f\n", "inverse", inverseDouble, inverseSingle,
           inverseDouble / inverseSingle);
    printf("%-10s %14.2f %14.2f %8.2f\n", "point", pointDouble, pointSingle,
           pointDouble / pointSingle);
    printf("%-10s %14.2f %14.2f %8.2f\n", "matrix", matrixDouble, matrixSingle,
           matrixDouble / matrixSingle);
    printf("failures %zu, sink %.6g\n", failures, sink);
    return failures == 0 ? 0 : 1;
}

} // namespace

int main(int argc, char** argv) {
    Options options;
    if (!Parse(argc, argv, options)) {
        printf("usage: MathBench [--rounds <1-100>] [--passes <1-100000>]\n");
        return 1;
    }

    return Run(options);
}
