#include <Core/Containers/Array.h>
#include <Core/Memory/SystemAllocator.h>
#include <Crypto/Aead.h>
#include <Crypto/AesModes.h>
#include <Platform/Clock.h>
#include <Platform/Memory.h>

#include <stdio.h>

// AES-GCM 봉인 처리량 측정 도구다(Docs/Design/Crypto.ko.md 6.4절 성능 gate, 결정 D4). 단일
// 스레드에서 메시지 크기 64 B·1 KiB·64 KiB·1 MiB마다 잰다.
// - libsodium-256: AES-256-GCM 일회성(hazmat::TrySealDetached, libsodium aesni·armcrypto).
// - rocks-256 msg: AES 경량 계층 AES-256-GCM. 공개 경로가 멀티파트뿐이라 메시지마다
//   AesGcmEncryptor 생성(키 저장소 슬롯 할당 + 키 스케줄 복사) + TryUpdate 한 번 + TryFinish다.
//   작은 메시지에서는 생성 비용이 함께 잡힌다.
// - rocks-256 stream: 암호기 하나에 그 크기 조각을 이어 넣는 TryUpdate 처리량(조각당 고정 비용).
// - rocks-128: AES 경량 계층 AES-128-GCM 일회성(참고, 라운드 수가 달라 gate 비교 대상이 아니다).
// gate는 rocks-256 msg / libsodium-256 비율이다(64 KiB 이상에서 생성 비용은 무시할 만하다).
//
// 사용:
//   CryptoBench                 기본(크기마다 약 64 MiB씩, 5회 중 최고값)
//   CryptoBench --rounds 9      반복 횟수(최고값을 보고한다)
//   CryptoBench --mib 256       크기마다 처리할 양(MiB)
//
// 같은 nonce를 반복해 쓰는 것은 측정용이다(실제 사용에서는 금지다). 결과는 기기·빌드 구성에 따라
// 달라지므로 같은 기기·Release에서 비교한다.
namespace {

using namespace rocks;
using namespace rocks::crypto;

struct Options {
    uint32 rounds = 5;
    usize mebibytes = 64;
};

double Seconds(uint64 start) {
    return static_cast<double>(platform::MonotonicNanoseconds() - start) * 1e-9;
}

// body를 rounds번 실행해 가장 짧은 시간을 돌려준다. body가 false면 음수다.
template <typename Body>
double Best(uint32 rounds, Body&& body) {
    double best = 1e30;
    for (uint32 round = 0; round < rounds; ++round) {
        const uint64 start = platform::MonotonicNanoseconds();
        if (!body()) {
            return -1.0;
        }

        const double elapsed = Seconds(start);
        best = elapsed < best ? elapsed : best;
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
        } else if (platform::CStringEquals(argv[index], "--mib") && hasValue) {
            if (!ParseNumber(argv[++index], 1, 4096, value)) {
                return false;
            }

            options.mebibytes = value;
        } else {
            return false;
        }
    }

    return true;
}

int Run(const Options& options) {
    if (!TryGetAeadInfo(AeadAlgorithm::Aes256Gcm).Value().available ||
        !TryGetAeadInfo(AeadAlgorithm::Aes128Gcm).Value().available) {
        printf("AES is unavailable on this CPU (Unsupported), nothing to measure\n");
        return 1;
    }

    uint8 keyBytes[32];
    for (usize i = 0; i < sizeof(keyBytes); ++i) {
        keyBytes[i] = static_cast<uint8>(i * 7 + 1);
    }

    auto key256 = AeadKey::TryImport(AeadAlgorithm::Aes256Gcm, Span<const uint8>(keyBytes));
    auto key128 =
        AeadKey::TryImport(AeadAlgorithm::Aes128Gcm, Span<const uint8>(keyBytes).Left(16));
    if (!key256 || !key128) {
        printf("cannot create keys\n");
        return 1;
    }

    constexpr usize MaxMessage = usize{1} << 20;
    SystemAllocator allocator;
    Array<uint8> input(allocator);
    Array<uint8> output(allocator);
    if (!input.TryResize(MaxMessage) || !output.TryResize(MaxMessage)) {
        printf("out of memory\n");
        return 1;
    }

    for (usize i = 0; i < MaxMessage; ++i) {
        input[i] = static_cast<uint8>(i * 13 + 5);
    }

    const uint8 nonce[12] = {1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12};
    const uint8 associatedData[13] = {};
    const Span<const uint8> nonceView(nonce);
    const Span<const uint8> ad(associatedData);
    const usize sizes[] = {64, 1024, usize{64} << 10, usize{1} << 20};
    const StringView name = TryGetAeadInfo(AeadAlgorithm::Aes128Gcm).Value().implementation;
    printf("%.*s, %.0f MiB per size, best of %u rounds, single thread, AD 13 B\n",
           static_cast<int>(name.ByteCount()), name.Data(), static_cast<double>(options.mebibytes),
           options.rounds);
    printf("%-9s %16s %16s %16s %16s %8s\n", "size", "libsodium-256", "rocks-256 msg",
           "rocks-256 stream", "rocks-128", "ratio");
    for (usize size : sizes) {
        const usize total = options.mebibytes << 20;
        const usize count = total / size;
        const Span<const uint8> message = input.View().Left(size);
        const Span<uint8> sealed = output.View().Left(size);
        uint8 tag[16];
        const double sodium = Best(options.rounds, [&]() {
            for (usize i = 0; i < count; ++i) {
                if (!hazmat::TrySealDetached(key256.Value(), nonceView, ad, message, sealed,
                                             Span<uint8>(tag))) {
                    return false;
                }
            }

            return true;
        });
        const double multipart = Best(options.rounds, [&]() {
            for (usize i = 0; i < count; ++i) {
                auto encryptor = hazmat::AesGcmEncryptor::TryCreate(key256.Value(), nonceView);
                if (!encryptor || !encryptor.Value().TryUpdateAssociatedData(ad) ||
                    !encryptor.Value().TryUpdate(message, sealed) ||
                    !encryptor.Value().TryFinish(Span<uint8>(tag))) {
                    return false;
                }
            }

            return true;
        });
        const double stream = Best(options.rounds, [&]() {
            auto encryptor = hazmat::AesGcmEncryptor::TryCreate(key256.Value(), nonceView);
            if (!encryptor) {
                return false;
            }

            for (usize i = 0; i < count; ++i) {
                if (!encryptor.Value().TryUpdate(message, sealed)) {
                    return false;
                }
            }

            return static_cast<bool>(encryptor.Value().TryFinish(Span<uint8>(tag)));
        });
        const double rocks128 = Best(options.rounds, [&]() {
            for (usize i = 0; i < count; ++i) {
                if (!hazmat::TrySealDetached(key128.Value(), nonceView, ad, message, sealed,
                                             Span<uint8>(tag))) {
                    return false;
                }
            }

            return true;
        });
        if (sodium < 0 || multipart < 0 || stream < 0 || rocks128 < 0) {
            printf("%-9u seal failed\n", static_cast<unsigned>(size));
            return 1;
        }

        const double megabytes = static_cast<double>(count * size) / (1024.0 * 1024.0);
        printf("%-9u %11.1f MB/s %11.1f MB/s %11.1f MB/s %11.1f MB/s %8.3f\n",
               static_cast<unsigned>(size), megabytes / sodium, megabytes / multipart,
               megabytes / stream, megabytes / rocks128, sodium / multipart);
    }

    return 0;
}

} // namespace

int main(int argc, char** argv) {
    Options options;
    if (!Parse(argc, argv, options)) {
        printf("usage: CryptoBench [--rounds <1-100>] [--mib <1-4096>]\n");
        return 1;
    }

    return Run(options);
}
