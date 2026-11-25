#include "EngineShaderProbe.h"

#include <Core/Math/Matrix4.h>
#include <Core/Math/MatrixFactory.h>
#include <Core/Math/Quaternion.h>
#include <Core/Math/Vector3.h>
#include <Core/Math/Vector4.h>

// A02 행렬·quaternion의 CPU 기준 대조다(Docs/Design/EngineShaders 7.3절). 기준은 Core
// Matrix4·Quaternion (double — UE 규약: 행 벡터, UE 회전 방향, EIP 2.2절)이다. 행렬은 Core가 만든
// 값을 float로 반올림해 루트 상수로 보내고, 기준은 같은 float 값을 double로 계산한다.
namespace sandbox::engine_shaders {

namespace {

using Matrix = rocks::Matrix4<double>;
using Quat = rocks::Quaternion<double>;
using Vec3 = rocks::Vector3<double>;
using Vec4 = rocks::Vector4<double>;

constexpr uint32 Width = 128;
constexpr uint32 Height = 128;
constexpr double HalfPi = 1.57079632679489661923;
// 회전·크기 행렬 원소 곱 합의 산술 상한(곱 3·덧셈 3 — 7번)이다.
constexpr uint32 TransformOperations = 7;

// 행렬을 float로 반올림해 요청에 넣고, 반올림한 값의 double 행렬을 돌려준다.
Matrix StoreMatrix(const Matrix& source, ProbeRequest& request) noexcept {
    Matrix rounded;
    for (int index = 0; index < 16; ++index) {
        request.values[index] = static_cast<float>(source.m[index]);
        rounded.m[index] = request.values[index];
    }

    return rounded;
}

// 시험 world 행렬 넷(크기·회전·이동, 음수 크기 하나)이다.
Matrix TestWorld(uint32 index) noexcept {
    const Vec3 axes[4] = {Vec3(0.0, 0.0, 1.0), Vec3(1.0, 2.0, 3.0), Vec3(-0.3, 0.9, 0.1),
                          Vec3(0.0, 1.0, 0.0)};
    const double angles[4] = {0.7, -2.1, 3.0, 1.2};
    const Vec3 scales[4] = {Vec3(1.0, 1.0, 1.0), Vec3(2.0, 0.5, 3.0), Vec3(-1.5, 1.0, 0.25),
                            Vec3(10.0, 10.0, 0.1)};
    const Vec3 translations[4] = {Vec3(10.0, -20.0, 5.0), Vec3(1.0e4, 2.5e3, -7.0e3),
                                  Vec3(0.0, 0.0, 0.0), Vec3(-300.0, 400.0, 120.0)};
    auto rotation = Quat::TryFromAxisAngle(axes[index], angles[index]);
    const Quat q = rotation ? rotation.Value() : Quat::Identity();
    return Matrix::Scale(scales[index]) *
           rocks::MakeRotationTranslationMatrix(q, translations[index]);
}

// 시험 쪽 행렬 3×3(ProbeMatrix3과 같은 식 — 대각에 ±2를 float로 더한다)이다.
void ProbeMatrix3(uint32 c, double out[3][3]) noexcept {
    for (uint32 row = 0; row < 3; ++row) {
        for (uint32 column = 0; column < 3; ++column) {
            out[row][column] = ProbeSigned(c, row * 3 + column, 0, 0);
        }
    }

    out[0][0] = static_cast<float>(static_cast<float>(out[0][0]) + 2.0f);
    out[1][1] = static_cast<float>(static_cast<float>(out[1][1]) + 2.0f);
    out[2][2] = static_cast<float>(static_cast<float>(out[2][2]) - 2.0f);
}

Matrix Embed3(const double m[3][3]) noexcept {
    Matrix result = Matrix::Identity();
    for (int row = 0; row < 3; ++row) {
        for (int column = 0; column < 3; ++column) {
            result.At(row, column) = m[row][column];
        }
    }

    return result;
}

double MaxAbs3(const Matrix& m) noexcept {
    double result = 0.0;
    for (int row = 0; row < 3; ++row) {
        double sum = 0.0;
        for (int column = 0; column < 3; ++column) {
            sum += fabs(m.At(row, column));
        }

        result = fmax(result, sum);
    }

    return result;
}

Quat ProbeQuat(uint32 c, uint32 stream) noexcept {
    const Double3 v = ProbeVector(c, stream, 0);
    const float w = ProbeSigned(c, stream + 3, 0, 0) + 0.01f;
    const double length = sqrt(Dot(v, v) + static_cast<double>(w) * w);
    return Quat(v.x / length, v.y / length, v.z / length, w / length);
}

void RunMatrixProbes(ProbeContext& context) noexcept {
    char label[96];
    for (uint32 index = 0; index < 4; ++index) {
        ProbeRequest request = MakeProbeRequest(ProbeMatrixTransform, 0, Width, Height);
        const Matrix world = StoreMatrix(TestWorld(index), request);
        for (uint32 selector = 0; selector < 3; ++selector) {
            request.selector = selector;
            snprintf(label, sizeof(label), "MatrixMath.%s[%u]",
                     selector == 0
                         ? "TransformPosition"
                         : (selector == 1 ? "TransformDirection" : "TransformPositionHomogeneous"),
                     index);
            CheckFloatProbe(context, label, request, UniformTolerance(0.0, 0.0),
                            [&world, selector](uint32 c, const float*, ProbeExpectation& e) {
                                const Double3 p = Scale(ProbeVector(c, 0, 0), 512.0);
                                const Vec4 input(p.x, p.y, p.z, selector == 1 ? 0.0 : 1.0);
                                const Vec4 out = world.TransformVector4(input);
                                const double values[4] = {out.x, out.y, out.z, out.w};
                                for (int k = 0; k < 4; ++k) {
                                    double magnitude = 0.0;
                                    const double in[4] = {input.x, input.y, input.z, input.w};
                                    for (int r = 0; r < 4; ++r) {
                                        magnitude += fabs(in[r] * world.At(r, k));
                                    }

                                    e.value[k] = values[k];
                                    e.absolute[k] = Arithmetic(TransformOperations) * magnitude;
                                }

                                e.mask = selector == 2 ? 0xFu : 0x7u;
                            });
        }

        // 법선 행렬 (M₃⁻¹)ᵀ, 이동·크기 분리(1 × 1 — 값은 행렬만으로 정해진다).
        ProbeRequest single = request;
        single.width = 1;
        single.height = 1;
        single.function = ProbeNormalMatrix;
        Matrix upper = Matrix::Identity();
        for (int row = 0; row < 3; ++row) {
            for (int column = 0; column < 3; ++column) {
                upper.At(row, column) = world.At(row, column);
            }
        }

        auto inverse = upper.TryInverse();
        for (uint32 row = 0; row < 3 && inverse; ++row) {
            single.selector = row;
            snprintf(label, sizeof(label), "MatrixMath.NormalMatrixFromWorld[%u].row%u", index,
                     row);
            const Matrix normal = inverse.Value().Transposed();
            CheckFloatProbe(context, label, single,
                            UniformTolerance(0.0, 32.0 * FloatUlp * MaxAbs3(normal)),
                            [&normal, row](uint32, const float*, ProbeExpectation& e) {
                                for (int k = 0; k < 3; ++k) {
                                    e.value[k] = normal.At(static_cast<int>(row), k);
                                    e.absolute[k] = 32.0 * FloatUlp * MaxAbs3(normal) + 1.0e-7;
                                }

                                e.mask = 0x7u;
                            });
        }

        single.function = ProbeExtractTransform;
        for (uint32 selector = 0; selector < 2; ++selector) {
            single.selector = selector;
            snprintf(label, sizeof(label), "MatrixMath.%s[%u]",
                     selector == 0 ? "ExtractTranslation" : "ExtractScale", index);
            CheckFloatProbe(context, label, single, UniformTolerance(0.0, Arithmetic(5)),
                            [&world, selector](uint32, const float*, ProbeExpectation& e) {
                                for (int k = 0; k < 3; ++k) {
                                    const Vec4 row = world.Row(k);
                                    e.value[k] =
                                        selector == 0
                                            ? world.At(3, k)
                                            : sqrt(row.x * row.x + row.y * row.y + row.z * row.z);
                                }

                                e.mask = 0x7u;
                            });
        }
    }

    // 3×3 역행렬·행렬식(조건수에 비례한 허용 오차).
    ProbeRequest inverse = MakeProbeRequest(ProbeInverse3x3, 0, Width, Height);
    for (uint32 selector = 0; selector < 4; ++selector) {
        inverse.selector = selector;
        snprintf(label, sizeof(label),
                 selector < 3 ? "MatrixMath.InverseMatrix3x3.row%u" : "MatrixMath.Determinant3x3",
                 selector);
        CheckFloatProbe(context, label, inverse, UniformTolerance(0.0, 0.0),
                        [selector](uint32 c, const float*, ProbeExpectation& e) {
                            double m[3][3];
                            ProbeMatrix3(c, m);
                            const Matrix embedded = Embed3(m);
                            auto inverted = embedded.TryInverse();
                            if (!inverted) {
                                e.mask = 0;
                                return;
                            }

                            const double condition = MaxAbs3(embedded) * MaxAbs3(inverted.Value());
                            if (selector == 3) {
                                double magnitude = 0.0;
                                const int p[6][3] = {{0, 1, 2}, {1, 2, 0}, {2, 0, 1},
                                                     {0, 2, 1}, {1, 0, 2}, {2, 1, 0}};
                                for (const auto& perm : p) {
                                    magnitude +=
                                        fabs(m[0][perm[0]] * m[1][perm[1]] * m[2][perm[2]]);
                                }

                                e.value[0] = embedded.Determinant();
                                e.absolute[0] = Arithmetic(5) * magnitude;
                                e.mask = 0x1u;
                                return;
                            }

                            for (int k = 0; k < 3; ++k) {
                                e.value[k] = inverted.Value().At(static_cast<int>(selector), k);
                                e.absolute[k] =
                                    Arithmetic(12) * condition * MaxAbs3(inverted.Value());
                            }

                            e.mask = 0x7u;
                        });
    }

    // reversed-Z 무한 원평면 투영(Core TryMakePerspective — UE FReversedZPerspectiveMatrix)의
    // near·FOV 읽기와 깊이 → view 깊이.
    const double halfFovs[3] = {0.25 * HalfPi, 0.5 * HalfPi, 0.9 * HalfPi};
    const double aspects[3] = {16.0 / 9.0, 1.0, 0.5};
    const double nears[3] = {10.0, 1.0, 37.5};
    for (uint32 index = 0; index < 3; ++index) {
        auto projection =
            rocks::TryMakePerspective(halfFovs[index], aspects[index], 1.0, nears[index]);
        if (!projection) {
            ReportProbe(context, "MatrixMath.Projection", false, "Core TryMakePerspective failed");
            continue;
        }

        ProbeRequest request = MakeProbeRequest(ProbeProjection, 0, Width, Height);
        const Matrix rounded = StoreMatrix(projection.Value(), request);
        const double tanX = tan(halfFovs[index]);
        const double tanY = tanX / aspects[index];
        snprintf(label, sizeof(label), "MatrixMath.ProjectionRead[%u]", index);
        CheckFloatProbe(context, label, request, UniformTolerance(0.0, Arithmetic(2)),
                        [&rounded, tanX, tanY](uint32 c, const float*, ProbeExpectation& e) {
                            const double depth = ProbeUnit(c, 0, 0);
                            const double nearDistance = rounded.At(3, 2);
                            e.value[0] = nearDistance;
                            // 1/M[0][0]·1/M[1][1]이 Core가 만든 tan(반화각)과 같은가(float 반올림
                            // 2번).
                            e.value[1] = tanX;
                            e.value[2] = tanY;
                            e.value[3] = depth > 0.0 ? nearDistance / depth : 3.402823466e+38;
                            e.absolute[1] = Arithmetic(3) * tanX;
                            e.absolute[2] = Arithmetic(3) * tanY;
                        });
    }
}

void RunQuaternionProbes(ProbeContext& context) noexcept {
    // Core Quaternion(UE 규약)과 대조한다. 입력은 GPU 정규화 오차(≈ 1e-7)를 포함한다.
    const char* names[8] = {"QuatMultiply",      "QuatRotateVector",  "QuatToMatrix.row0",
                            "QuatToMatrix.row1", "QuatToMatrix.row2", "MatrixToQuat",
                            "QuatSlerp",         "QuatConjugate"};
    char label[96];
    for (uint32 selector = 0; selector < 8; ++selector) {
        ProbeRequest request = MakeProbeRequest(ProbeQuaternion, selector, Width, Height);
        snprintf(label, sizeof(label), "Quaternion.%s", names[selector]);
        CheckFloatProbe(
            context, label, request, UniformTolerance(2.0e-6, 0.0),
            [selector](uint32 c, const float* gpu, ProbeExpectation& e) {
                const Quat a = ProbeQuat(c, 0);
                const Quat b = ProbeQuat(c, 4);
                switch (selector) {
                case 0: {
                    const Quat q = a * b;
                    e.value[0] = q.x;
                    e.value[1] = q.y;
                    e.value[2] = q.z;
                    e.value[3] = q.w;
                    break;
                }
                case 1: {
                    const Double3 v = Scale(ProbeVector(c, 8, 0), 100.0);
                    const Vec3 r = a.RotateVector(Vec3(v.x, v.y, v.z));
                    e.value[0] = r.x;
                    e.value[1] = r.y;
                    e.value[2] = r.z;
                    e.mask = 0x7u;
                    // 입력 quaternion 정규화 오차(≈ 1e-7)가 |v|배로 간다.
                    const double length = sqrt(Dot(v, v));
                    e.absolute[0] = e.absolute[1] = e.absolute[2] = 1.0e-6 + 8.0e-7 * length;
                    break;
                }
                case 2:
                case 3:
                case 4: {
                    const Matrix m = a.ToMatrix();
                    for (int k = 0; k < 3; ++k) {
                        e.value[k] = m.At(static_cast<int>(selector - 2), k);
                    }

                    e.mask = 0x7u;
                    break;
                }
                case 5: {
                    // q와 −q는 같은 회전이다: GPU 값 쪽 부호로 맞춘다.
                    const double sign =
                        gpu[0] * a.x + gpu[1] * a.y + gpu[2] * a.z + gpu[3] * a.w >= 0.0 ? 1.0
                                                                                         : -1.0;
                    e.value[0] = sign * a.x;
                    e.value[1] = sign * a.y;
                    e.value[2] = sign * a.z;
                    e.value[3] = sign * a.w;
                    for (int k = 0; k < 4; ++k) {
                        e.absolute[k] = 1.0e-5;
                    }

                    break;
                }
                case 6: {
                    const double t = ProbeUnit(c, 11, 0);
                    const Quat q = rocks::Slerp(a, b, t);
                    e.value[0] = q.x;
                    e.value[1] = q.y;
                    e.value[2] = q.z;
                    e.value[3] = q.w;
                    // acos·sin 사양 상한(절대 2^-11)을 sin θ로 나눈 전파(nlerp 문턱 0.9995 → sin θ
                    // ≥ 0.0316).
                    double cosine = fabs(a.x * b.x + a.y * b.y + a.z * b.z + a.w * b.w);
                    const double sine = sqrt(fmax(1.0 - cosine * cosine, 0.0316 * 0.0316));
                    for (int k = 0; k < 4; ++k) {
                        e.absolute[k] = cosine > 0.9995 ? 4.0e-6 : 3.0 * SpecTrigAbsolute / sine;
                    }

                    break;
                }
                default:
                    e.value[0] = -a.x;
                    e.value[1] = -a.y;
                    e.value[2] = -a.z;
                    e.value[3] = a.w;
                    break;
                }
            });
    }

    // tangent 기저 압축(QTangent): 반사 부호가 w 부호에 남고 왕복이 원래 기저다.
    ProbeRequest frame = MakeProbeRequest(ProbeTangentFrame, 0, Width, Height);
    Array<float> q(context.allocator);
    Array<float> t(context.allocator);
    Array<float> b(context.allocator);
    Array<float> n(context.allocator);
    bool ran = RunFloatProbe(context, "Quaternion.EncodeTangentFrame", frame, q);
    frame.selector = 1;
    ran = ran && RunFloatProbe(context, "Quaternion.DecodeTangentFrame.t", frame, t);
    frame.selector = 2;
    ran = ran && RunFloatProbe(context, "Quaternion.DecodeTangentFrame.b", frame, b);
    frame.selector = 3;
    ran = ran && RunFloatProbe(context, "Quaternion.DecodeTangentFrame.n", frame, n);
    if (!ran) {
        return;
    }

    double worst = 0.0;
    uint32 wrongSign = 0;
    double worstLength = 0.0;
    for (uint32 c = 0; c < CaseCount(frame); ++c) {
        const Double3 normal = ProbeDirection(c, 0, 0);
        const Double3 seedAxis =
            fabs(normal.x) < 0.5 ? Double3{1.0, 0.0, 0.0} : Double3{0.0, 1.0, 0.0};
        const Double3 base = Normalize(Cross(seedAxis, normal));
        const double angle = ProbeUnit(c, 40, 0) * 6.28318530718;
        const Double3 tangent =
            Add(Scale(base, cos(angle)), Scale(Cross(normal, base), sin(angle)));
        const bool positive = (ProbeBits(c, 41, 0) & 1u) != 0;
        const Double3 bitangent = Scale(Cross(normal, tangent), positive ? 1.0 : -1.0);
        const float* qv = q.Data() + c * 4u;
        const double length = sqrt(static_cast<double>(qv[0]) * qv[0] + qv[1] * qv[1] +
                                   qv[2] * qv[2] + qv[3] * qv[3]);
        worstLength = fmax(worstLength, fabs(length - 1.0));
        if ((qv[3] > 0.0f) != positive || fabs(qv[3]) < 1.0 / 32767.0 * 0.999) {
            ++wrongSign;
        }

        const Double3 pairs[3][2] = {{tangent, ToDouble3(t.Data() + c * 4u)},
                                     {bitangent, ToDouble3(b.Data() + c * 4u)},
                                     {normal, ToDouble3(n.Data() + c * 4u)}};
        for (const auto& pair : pairs) {
            const Double3 d = Subtract(pair[0], pair[1]);
            worst = fmax(worst, sqrt(Dot(d, d)));
        }
    }

    char detail[160];
    snprintf(detail, sizeof(detail),
             "%u frames, max round-trip error %.3g, max |q| - 1 %.3g, sign errors %u",
             CaseCount(frame), worst, worstLength, wrongSign);
    ReportProbe(context, "Quaternion.TangentFrameRoundTrip",
                worst <= 2.0e-5 && worstLength <= 2.0e-6 && wrongSign == 0, detail);
}

} // namespace

void RunMatrixQuaternionProbes(ProbeContext& context) noexcept {
    RunMatrixProbes(context);
    RunQuaternionProbes(context);
}

} // namespace sandbox::engine_shaders
