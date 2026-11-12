# Core 데이터 기반 보강(A0-C) 설계

기준일: 2026-11-12, 초안 3(교차검증 2건·최종 검증 반영, C1 구현 반영). [에셋 시스템 설계](AssetSystem.ko.md) 1절 A0-C 단계의 상세다. 에셋 경로 키(Unicode 정규화·case folding), 쿠킹 결정성(결정적 수학), 설정 파일(JSON), 패키지 Record 목록(`RecordList`)이 이 기반을 쓴다. [Core 상세 사양](CoreDetailedSpec.ko.md) T03(Unicode normalization·case folding 후속)을 이 문서가 구체화한다. SHA-256은 구현되어 있다(`Core/Encoding/Sha256.h`).

원칙: 표준 라이브러리 없이 Core에 구현한다. 외부 데이터(UCD·시험 데이터)는 고정 버전 원본을 저장소에 두고 생성기로 표를 만든다(라이선스 고지 포함). 결과가 호스트·CRT·locale에 따라 달라질 수 있는 연산은 쓰지 않는다.

## 1. Unicode

### 1.1 버전과 원본 데이터

- Unicode **18.0.0**(2026-09-01 배포, `https://www.unicode.org/Public/18.0.0/ucd/`).
- 원본 파일: `UnicodeData.txt`(General_Category·정규 분해·결합 등급), `CompositionExclusions.txt`(교차 확인용), `DerivedNormalizationProps.txt`(Full_Composition_Exclusion·NFC_QC·NFD_QC), `CaseFolding.txt`(상태 C·S), `DerivedCoreProperties.txt`(Default_Ignorable_Code_Point), `NormalizationTest.txt`(시험), Unicode 라이선스 원문(`https://www.unicode.org/license.txt` — UCD 디렉터리 밖에 있다).
- 위치: `Core/Source/ThirdParty/UnicodeData/ucd-18.0.0/`(원본 파일 + `license.txt`) + `ThirdParty.json`. 출처는 `UCD.zip`(URL·SHA-256)이고 라이선스 원문은 별도 출처이므로 manifest에 선택 필드 `licenseSource { url, sha256 }`를 추가한다(라이선스 파일은 원본 디렉터리에 두고 `files`에 등록한다 — 기존 "라이선스 파일은 등록된 원본 파일" 규칙 유지).
- 버전 올리기: 원본 교체 + 생성기 재실행 + 시험 갱신 한 묶음. 정규화·case pair 안정성 정책 덕분에 이미 할당된 문자의 정규화·folding 결과는 바뀌지 않지만 **General_Category와 Default_Ignorable은 안정성 정책 대상이 아니다** — 이전 버전에서 유효한 경로 이름이 무효가 될 수 있다. 그래서 버전을 올릴 때 생성기가 경로 허용 집합의 이전·이후 차이를 보고하고, `asset-index-verify`로 프로젝트 이름을 다시 검사하는 절차를 둔다. AssetIndex 캐시는 도구 빌드 ID로 이미 무효화된다.

### 1.2 데이터 전용 ThirdParty 종류

기존 manifest는 C 번역 단위가 있는 라이브러리만 받았다. 다음을 추가했다(C1, 구현됨 — `Policy.ThirdPartyDataLibrary` 시험).

- 최상위 필드 `kind`(`"code"` 기본·`"data"`). `data`면 `translationUnits`·`includeDirectories`·`defines`·`platformDefines`·`consumerDefines`·`adapterHeaders`·`pragmaDirectives`가 비어야 하고(`nonEmpty` 검사를 kind에 따라 바꾼다) `<Lib>.Build.cs`가 없어야 한다. `files` SHA-256·출처·라이선스 검증은 같다. 지시문 스캔(정책 검사·refresh)은 data 종류를 건너뛴다.
- 최상위 필드 `embeddedIn`(모듈 이름 목록): 이 데이터의 파생물(생성 표)이 들어가는 모듈. SDK 내보내기(`InstalledSdk.cs` 라이선스 복사)는 packaged 모듈 이름이 아니라 `embeddedIn`에 packaged 모듈이 있으면 라이선스를 `Licenses/<Lib>/`로 복사한다. 시험 전용 데이터(JSONTestSuite)는 `embeddedIn`이 비어 배포하지 않는다. 게임 패키징의 고지 경로는 패키징 단계(후속)에서 같은 필드를 쓴다.
- 바꿀 곳: `ThirdPartyManifest.cs`(필드·검증), `PolicyChecks.ThirdParty.cs`, `ThirdPartyManifestRefresh.cs`, `InstalledSdk.cs`, 시험(`ThirdPartyPolicyTests.cs`, `NativeTests.cs`의 fixture JSON), 기존 manifest 3개는 `kind` 생략(기본 code)으로 그대로 둔다. Rocks.Tooling은 다른 세션이 함께 쓰므로 착수 전에 조율한다.
- 라이선스: UCD는 SPDX `Unicode-3.0`, 고지 조건(사본 또는 문서에 저작권·허가 고지)을 지키려고 `binaryNoticeRequired: true`.

### 1.3 생성기

- 명령: `Rocks.ToolTests unicode-tables --root . [--write]`(third-party refresh와 같은 형태 — 기본은 차이만 보고하고 종료 코드 1, `--write`면 고친다).
- 출력: `Core/Source/Runtime/Core/Private/Text/UnicodeTables.generated.h` 하나(확장자를 `.h`로 해 정책·서식 검사 범위에 든다 — 생성기는 clang-format 결과와 같은 서식으로 쓴다). 머리 주석에 Unicode 버전, 원본 파일별 SHA-256, 생성기 버전을 쓴다. 출력은 결정적이다(정렬된 순회, 고정 서식, LF). 시험: 두 번 실행한 결과가 같은 byte.
- 정책 검사 `unicode-tables`: 원본 SHA-256이 manifest와 같고, 생성 결과를 메모리에서 다시 만들어 저장소 파일과 byte 단위로 같은지 본다. `policy` 명령과 self-test에 등록한다.
- 표 구조(코드 포인트 0–0x10FFFF, 2단계 표: 상위 `cp >> 8`로 블록 번호, 블록 안 256칸. 같은 내용의 블록은 하나로 합친다):
  - 결합 등급(ccc, u8), NFC_QC·NFD_QC(2비트: Yes·No·Maybe — 파일에 없는 코드 포인트는 Yes), General_Category(5비트, Cn 포함 30값), Default_Ignorable(1비트).
  - 정규 분해: 코드 포인트 → (시작, 길이) 색인 + 분해 배열(완전 정규 분해를 미리 펼쳐 저장 — 분해 단계용). Hangul 음절은 표 없이 알고리즘(Unicode 본문 §3.12: SBase·LBase·VBase·TBase).
  - 정규 합성: UnicodeData 5필드의 **1단계 두 문자 정규 분해**에서 (앞, 뒤) → 합성 쌍을 만들고 Full_Composition_Exclusion 문자는 뺀다. 키 `(uint64(first) << 21) | second` 정렬 배열 + 이분 탐색. Hangul은 알고리즘. 생성기는 FCE = CompositionExclusions ∪ singleton 분해 ∪ 비시작 문자 분해와 DerivedNormalizationProps의 FCE가 같은지 교차 확인한다.
  - 단순 case folding: 상태 C·S의 1:1 대응을 (시작, 끝, stride, 차이) 구간 표로 둔다(대·소문자가 한 칸씩 번갈아 나오는 구간을 stride 2로 묶는다). F·T(길이가 바뀌는 full folding·터키어 규칙)는 쓰지 않는다.

### 1.4 API(`Core/Text/Unicode.h`)

```cpp
enum class NormalizationForm : uint8 { NFC, NFD };
enum class GeneralCategory : uint8 { Lu, Ll, Lt, Lm, Lo, Mn, Mc, Me, Nd, Nl, No, Pc, Pd, Ps, Pe, Pi, Pf, Po,
                                     Sm, Sc, Sk, So, Zs, Zl, Zp, Cc, Cf, Cs, Co, Cn };   // 30값
inline constexpr StringView UnicodeVersion = "18.0.0";

// input(유효한 UTF-8)을 정규화해 out 뒤에 붙인다. 잘못된 UTF-8은 InvalidArgument, 입력 상한(64 MiB) 초과는
// Overflow, 할당 실패는 OutOfMemory이며 실패하면 out은 호출 전 길이로 돌아간다.
Result<void> TryNormalize(NormalizationForm form, StringView input, StringBuilderBase& out) noexcept;
// 이미 그 형식인지 본다. 잘못된 UTF-8은 InvalidArgument, Maybe 경로의 할당 실패는 allocator 오류.
Result<bool> IsNormalized(NormalizationForm form, StringView input, Allocator& allocator) noexcept;
// 단순 case folding(코드 포인트 수 불변, UTF-8 byte 수는 바뀔 수 있다 — 예: U+023A → U+2C65).
Result<void> TrySimpleCaseFold(StringView input, StringBuilderBase& out) noexcept;
// 대소문자를 무시하는 비교 키 = NFC(SimpleCaseFold(NFD(input))). Unicode §3.13 D145 정준 caseless 일치의
// simple folding 판이다(NFD를 먼저 해야 U+0130 = NFC("İ") 같은 경우와 ypogegrammeni가 같은 키가 된다).
Result<void> TryMakeCaselessKey(StringView input, StringBuilderBase& out) noexcept;
GeneralCategory GetGeneralCategory(char32 codePoint) noexcept;    // surrogate는 Cs, 범위 밖은 Cn
uint8 GetCanonicalCombiningClass(char32 codePoint) noexcept;
bool IsDefaultIgnorable(char32 codePoint) noexcept;
```

- 알고리즘(UAX #15): 완전 정규 분해 → 정규 순서(결합 등급이 0이 아닌 연속 구간을 결합 등급 계수 정렬로 안정 정렬 — O(n)) → (NFC면) 정규 합성(차단 규칙 포함).
- 스트리밍 조각 경계: 분해한 결과의 문자가 **결합 등급 0이고 NFC_QC = Yes**일 때 그 앞에서 조각을 끊는다(NFD는 결합 등급 0이면 끊는다). 입력 문자의 결합 등급만 보면 안 된다 — U+0F73·U+0F75·U+0F81은 ccc 0이지만 분해 결과가 비시작 문자이고, Hangul L+V·LV+T, U+0B47+U+0B3E 같은 시작 문자끼리의 합성(NFC_QC = Maybe)도 있다. 조각 버퍼는 입력 상한 안에서 늘리며 조각 길이 상한을 따로 두지 않는다(UAX #15의 Stream-Safe 형식은 텍스트 형식 규칙이지 알고리즘 요구가 아니다).
- `IsNormalized`: quick check(NFC_QC·NFD_QC)와 결합 등급 순서 검사(ccc ≠ 0이고 직전 ccc > ccc이면 No)를 함께 하고, Maybe면 그 조각만 정규화해 비교한다.
- 출력 크기: UTF-8 기준 NFC·NFD 모두 최대 3배(예: U+1D160), folding도 byte 수를 늘릴 수 있다. 최악값을 미리 확보하지 않고 입력 상한 안에서 필요할 때 늘린다.
- 에셋 경로 정책(AssetSystem 4.2절)은 `TryNormalize(NFC)`·`TryMakeCaselessKey`·`GetGeneralCategory`·`IsDefaultIgnorable`을 쓴다(보이지 않는 Default_Ignorable 문자 — U+3164·U+115F·U+FFA0(Lo)·U+FE00–FE0F·U+E0100–E01EF·U+180B–180F·U+034F·U+17B4–17B5 등 — 는 L*·M*이어도 거부). UAX #31 식별자 속성(XID_Start·XID_Continue)으로 바꾸는 방안은 경로 정책 개정 때 검토한다.

### 1.5 시험

- `NormalizationTest.txt` 전체(Part 0–3): 각 줄의 c1–c5에 대해 UAX #15 표의 등식(NFC: c2 = toNFC(c1) = toNFC(c2) = toNFC(c3), c4 = toNFC(c4) = toNFC(c5), NFD도 같은 방식). Part 1에 없는 모든 코드 포인트 X는 toNFC(X) = toNFD(X) = X. 각 열에 대해 `IsNormalized`가 (정규화 결과 == 입력)과 같은지도 본다.
- 시험 데이터는 ThirdParty 원본을 실행 시 읽는다(빌드 산출물에 넣지 않는다). Android 기기 시험은 데이터 배포 경로를 기기 시험 도입 때 정한다(지금 Android는 빌드 확인만).
- caseless 키: 모든 코드 포인트와 NormalizationTest의 결합 문자열에 대해 멱등성 K(K(x)) = K(x)를 망라 검사하고, U+0130·U+0345·U+1E96·U+01F0·U+1F80(ᾀ ≡ ἀι)을 고정 사례로 둔다.
- case folding: `CaseFolding.txt`의 C·S 줄 전부. General_Category: `UnicodeData.txt`의 모든 줄(구간 표기 `First>`·`Last>` 포함)과 미할당 Cn. Default_Ignorable: `DerivedCoreProperties.txt` 전부.
- 잘못된 UTF-8(overlong·surrogate·잘림), 출력 실패 시 out 복원, 상한, Hangul 경계, 생성기 결정성, 정책 검사 `unicode-tables`.

## 2. 결정적 수학

### 2.1 범위와 이유

쿠킹 결과는 같은 도구 빌드라면 호스트와 무관하게 같은 바이트여야 한다(AssetSystem 10절). CRT의 `pow`·`exp`·`log`·`sin`은 구현과 CPU 경로(예: UCRT의 FMA3 분기)에 따라 결과가 달라질 수 있다(지금 `LinearColor::ToSRGB`가 이것을 쓴다 — Color.h 주석). 빌드는 이미 `-ffp-contract=off`이고 fast-math를 쓰지 않으며 x87을 쓰지 않으므로, IEEE 기본 연산(+ − × ÷ √, 정수 bit 연산)과 float↔double 변환만 쓰는 구현은 x64·ARM64에서 같은 결과를 낸다. 단 다음 전제를 지킨다.

- 부동소수점 환경: 반올림 모드 nearest, 비정규 수 보존(FTZ·DAZ 끔), 예외 trap 꺼짐. 검사는 상태 플래그를 빼고 한다: x64는 `(MXCSR & ~0x3F) == 0x1F80`(bit 0–5는 보통 연산이 켜는 상태 플래그), ARM64는 FPCR의 RMode·FZ·DN·AH·FIZ·NEP와 trap enable 비트가 모두 0. 에디터 프로세스 안에서는 드라이버·미들웨어가 이 값을 바꿀 수 있으므로 PlatformBase에 `FloatingPointEnvironmentGuard`(확인 + 저장·복원)를 추가하고(A0-P 보강) 쿠킹·결정적 계산 작업이 진입할 때 건다.
- 암묵적 libm 호출 금지: clang-cl x64는 `floor`·`trunc`·`round`·`fmod`와 음수 분기의 `sqrt`를 UCRT 호출로, Android arm64는 `fmod`를 bionic 호출로 만든다(실측). 그래서 Floor·Trunc·RoundHalfEven·지수 조정(ldexp 대응)을 bit 연산으로 직접 구현하고, 모듈에 `-fno-math-errno`를 준다(sqrt가 명령 하나가 된다). Rocks.Build는 compile 인자를 모듈 단위로만 받으므로 결정적 수학은 **별도 모듈 `DeterministicMath`**(Core/Source/Runtime/DeterministicMath, Base에만 의존)로 두어 이 인자와 외부 심볼 검사가 그 모듈에만 걸리게 한다. Core(`LinearColor`)는 이 모듈에 의존한다.
- 정수 변환: 범위 밖 값·NaN의 float → int 변환 결과가 x64(0x8000…)와 ARM64(포화)에서 다르므로, 정수 추출은 범위를 먼저 검사하고 shifter 기법(x + 0x1.8p52)으로 한다.
- `long double` 금지(Android arm64에서는 binary128 soft-float라 compiler-rt를 부른다) — 정책 검사.
- NaN: 기본 NaN 비트가 x64(0xFFF8…)와 ARM64(0x7FF8…)에서 다르고, 두 NaN 피연산자의 결과는 순서에 따른다. 그래서 모든 함수는 출구에서 결과가 NaN이면 고정 quiet NaN(0x7FF8000000000000, float는 0x7FC00000)을 반환한다.

### 2.2 API(모듈 `DeterministicMath`, `DeterministicMath/DeterministicMath.h`, namespace `rocks::deterministic`)

```cpp
inline constexpr uint32 Version = 1;   // 결과 비트가 바뀌면 올린다 — 빌드 캐시 키·cookKey에 들어간다
double Floor(double x) noexcept;  double Trunc(double x) noexcept;  double RoundHalfEven(double x) noexcept;
double Exp2(double x) noexcept;   double Log2(double x) noexcept;
double Pow(double base, double exponent) noexcept;   // base ≥ 0만(음수는 고정 NaN). Pow(0, 0) = 1, Pow(0, y<0) = +Inf, Pow(+Inf, y>0) = +Inf, Pow(x, ±0) = 1
double SinPi(double x) noexcept;  double CosPi(double x) noexcept;   // sin(πx)·cos(πx)
double Sin(double x) noexcept;    double Cos(double x) noexcept;     // |x| ≤ 2^20, 넘으면 고정 NaN
double Sinc(double x) noexcept;   double Lanczos(double x, int lobes) noexcept;   // SinPi로 정의
double SrgbToLinear(double encoded) noexcept;   double LinearToSrgb(double linear) noexcept;   // IEC 61966-2-1 구간 함수
double Gamma22ToLinear(double encoded) noexcept;                                             // x^2.2(bUseLegacyGamma)
float SrgbByteToLinear(uint8 value) noexcept;    // 생성 표 256칸(정확히 반올림한 값)
uint8 LinearToSrgbByte(float linear) noexcept;   // 경계 표 이분 탐색. NaN·0 이하 → 0, 1 이상 → 255
```

- 구현:
  - `SinPi`·`CosPi`: x − RoundHalfEven(2x)/2는 오차 없이 계산되므로 [−¼, ¼]로 정확히 줄인 뒤 고정 계수 다항식. Lanczos·Sinc는 이것을 쓴다(π·x를 먼저 곱하면 정수 근처 0점에서 정확도를 잃는다).
  - `Sin`·`Cos`: 3단 Cody-Waite π/2 축소(fdlibm 방식, |x| ≤ 2^20) + 다항식. 축소 최악 입력은 생성기가 고정밀 계산으로 찾아 시험에 넣는다.
  - `Log2`: 128칸 표 + double-double 보정(약 0.52 ulp), `Exp2`: 표 + 다항식, `Pow` = Exp2(y · Log2 x)를 double-double로 이어 계산한다. 오차 상한은 |y · log2 x|에 따라 시험으로 측정해 문서에 표로 기록한다(sRGB 범위 |t| ≤ 8.3에서 목표 2 ulp 이하).
  - 계수는 16진 실수 리터럴로 고정한다. 정확도와 무관하게 **결과 비트의 결정성**이 계약이다.
- 표 생성(`Rocks.ToolTests math-tables --root . [--write]`, 정책 검사로 갱신 여부 확인): x^2.4·x^(1/2.4)·x^2.2는 유리수가 아니므로 반올림 경계 판정을 정수 비교로 한다. 지수 12/5이면 후보 값 m에 대해 m^5와 q^12를 BigInteger 분수로 정확히 비교한다(2.2 = 11/5, 1/2.4 = 5/12도 같은 방식).
  - `SrgbByteToLinear` 표: decode(k/255)를 정확히 반올림한 float.
  - `LinearToSrgbByte` 경계: t_k = decode((k + 0.5)/255)(부호화 공간의 중간점 — AssetSystem 12.2절의 round-half-up 양자화와 같다)이고, 표에는 t_k 이상인 가장 작은 float를 저장한다(가장 가까운 float가 아니다). 그래서 8비트 경로는 정확한 반올림이고, 16비트·float 경로(`LinearToSrgb`)는 근사 Pow를 쓰는 다른 의미다(문서화).
- Core `LinearColor::FromSRGB`·`ToSRGB`는 이 표·함수로 바꾼다(Core 안 sRGB 구현을 하나로 — 런타임과 쿠킹 결과가 어긋나지 않게).

### 2.3 시험

- 기준값: 각 함수의 고정 입력 표(범위 경계·특수값·고정 시드 PCG32 표본 수천 개)의 출력 비트를 저장하고, Windows x64·macOS arm64(CI)·Android arm64(arm64 기기 또는 arm64 호스트 — x64 에뮬레이터는 해당 없음)에서 같은지 본다. float 입력 함수(`LinearToSrgbByte`)는 2^32 전수 출력의 SHA-256을 호스트끼리 비교한다.
- 정확도: C#에서 BigInteger 고정밀 계산(급수·유리수)으로 만든 참값과의 ulp 차이를 재서 표로 기록한다.
- 버전 강제: 기준값 표의 SHA-256을 `Version`과 함께 시험에 고정해, 결과가 바뀌었는데 Version을 올리지 않으면 실패한다.
- 외부 심볼 허용 목록(정책 검사, 이 단계가 소유): Development 구성의 `DeterministicMath` 모듈 기계어 오브젝트(Release ThinLTO bitcode는 backend libcall이 보이지 않으므로 대상이 아니다)에 툴체인의 `llvm-nm`을 돌려 외부 심볼이 허용 목록(Base 심볼)뿐인지 본다. 회귀: aarch64에서 `a*b+c`가 fmadd가 되지 않는지.
- 환경 guard: FTZ를 강제로 켠 상태에서 guard가 바로잡고 결과가 기준값과 같은지.
- `LinearToSrgbByte(SrgbByteToLinear(b)) == b`(256값), 단조성, 경계 바로 아래·위 값.

## 3. JSON(RFC 8259)

### 3.1 용도와 요구

`Config/AssetAliases.json`·`AssetSources.json`·`CookSettings.json`·`BuildCache.json`, `Engine/Config/TargetProfiles/*.json`, 도구 보고서. 버전 관리되는 파일을 도구가 다시 쓰고 그 SHA-256이 쿠킹 키에 들어가므로 **결정적 출력**(CRT·locale 무관)이 필요하고, 사람이 고친 파일을 읽으므로 **엄격하고 친절한 오류**가 필요하다.

### 3.2 숫자 변환(CRT 없이)

- 지금 `TryParseDouble`은 strtod·locale 사본을 쓰고(1023 byte 초과 토큰 거부), `FormatShortestFloatingPoint`는 snprintf·strtod 이분 탐색을 쓴다. 둘 다 CRT 구현에 의존하므로 Core에 CRT 없는 구현을 넣는다: 정확히 반올림하는 10진 → 2진(Eisel-Lemire + 큰 정수 보조 경로, 길이 제한 없음 — 입력 상한 안), 최단 왕복 2진 → 10진(Ryu 계열). NumberText·StringBuilder가 이것을 쓰도록 바꾼다(공개 계약은 같다).
- 시험: .NET(Core 3.0+)의 `double.Parse`·`"R"` 서식과 고정 시드 무작위·경계 사례 수백만 개를 비교하고, 알려진 어려운 사례 표를 고정한다.

### 3.3 읽기(`Core/Text/Json.h`)

- 결과: `JsonReadResult { document, warnings }`(경고 목록 — `JsonBomIgnored` 등) 또는 `JsonError`.
- 입력은 UTF-8. 맨 앞 BOM 하나는 건너뛰고 `JsonBomIgnored` 경고를 남긴다(RFC 8259 §8.1의 MAY — PowerShell 5.1·Visual Studio가 기본으로 붙인다). 주석·끝 쉼표·작은따옴표·NaN/Infinity 리터럴·잘못된 UTF-8·짝 없는 surrogate 이스케이프(`\uD800`)는 거부.
- 같은 객체 안 중복 키는 오류다(조용히 덮어쓰지 않는다). 판정은 이스케이프를 푼 뒤 byte 비교이며 Unicode 정규화는 하지 않는다(키 `a`와 이스케이프로 쓴 키 `a`(역슬래시·u·0061)은 같은 키, NFC·NFD 차이는 다른 키).
- 숫자 모델: 소수부·지수가 없는 토큰만 정수 종류(int64, 넘으면 uint64, 그보다 크면 double)이고 나머지는 double(3.2절 파서). double로도 표현할 수 없는 크기(예: `1e400`)는 `NumberOutOfRange` 오류다(±Inf로 바꾸지 않는다). `-0`은 double −0.0. 원문 토큰도 보관한다(Preserve 쓰기용). 값 비교는 수치 기준.
- 위치: 줄·열은 1부터, CRLF는 줄바꿈 1개, CR 단독도 줄바꿈, 열은 코드 포인트 단위(tab은 1칸), byte offset을 함께 보고한다. 오류 `JsonError { code, line, column, byteOffset, message }`.
- 한도(`JsonReadOptions`로 조정, 기본): 입력 64 MiB, 깊이 128, 문자열 하나 16 MiB, 노드 수 4,194,304. 파서는 반복형(명시 스택)이다(`n_structure_100000_opening_arrays`).
- DOM: `JsonDocument`(allocator 소유 arena) — `JsonValue`(Null·Bool·Number·String·Array·Object), 객체는 원문 순서를 보존하는 (키, 값) 배열 + 조회용 해시.

### 3.4 쓰기

- `JsonWriter`(StringBuilder 대상)의 byte 단위 고정 규칙: 들여쓰기 2칸, 줄바꿈 LF, 끝에 줄바꿈 1개, 키 구분자 `": "`, 빈 컨테이너 `{}`·`[]`, 키 순서는 호출자 순서(정렬이 필요한 파일은 호출자가 AssetSystem 4.2절 정렬로 넣는다). 문자열 이스케이프는 `"`·`\\`·제어 문자만(U+0008·U+000C·U+000A·U+000D·U+0009는 `\b\f\n\r\t`, 그 밖은 소문자 16진 `\u00xx` — 키 안의 U+0000 포함), 그 밖의 문자는 UTF-8 그대로. 호출자가 준 문자열은 UTF-8을 검증한다. BOM은 쓰지 않는다.
- 모드: **Canonical**(기본 — 정수 종류는 10진, double은 최단 왕복 표기이며 결과에 `.`·`e`가 없으면 `.0`을 붙여 다시 읽어도 double이 된다, −0.0은 `-0.0`) — 읽기 → 쓰기 → 읽기가 같은 DOM이고 쓰기 결과는 고정점이다. **Preserve**(원문 숫자 토큰 그대로 — 사람이 쓴 표기를 유지해야 하는 편집 도구용, 고정점 보장 없음). NaN·Inf는 쓰기 오류.
- 원자적 파일 기록은 호출자가 `FileSystem::Replace` 흐름으로 한다(AssetSystem 5.5절과 같은 임시 파일 규칙).

### 3.5 시험

- JSONTestSuite(nst/JSONTestSuite, MIT, 릴리스가 없어 commit 고정)의 y_·n_ 사례 전체. 이 문서의 선택과 다른 사례는 표로 고정한다: `y_object_duplicated_key`·`y_object_duplicated_key_and_value`(중복 키 거부 — 의도한 예외), i_ 사례 각각의 결정(BOM은 받음 등). 데이터는 `Core/Source/ThirdParty/JsonTestSuite/JSONTestSuite-<commit>/`에 데이터 전용 라이브러리(1.2절)로 둔다(`.gitattributes`의 `-text` 규칙이 `Core/Source/ThirdParty/*/*/**`에 걸려 CR·NUL·BOM·잘못된 UTF-8 파일이 보존된다).
- 왕복·고정점(Canonical), 중복 키, 한도, 오류 위치(CRLF·CR·tab), 숫자 경계.

## 4. RecordList(Serialization)

- 형식: Record에 새 wire type `WireType::RecordList`를 추가한다(범용 도구·리플렉션이 목록을 구분한다). payload = `u32 count + count × (u64 length + 중첩 Record)`. 각 항목의 length는 중첩 Record의 전체 길이(헤더 44 byte + payload)와 같아야 한다. 빈 목록도 count 0으로 기록한다(Record에는 "없으면 기본값" 규칙이 없고 ObjectSerialization은 아는 필드가 없으면 NotFound다 — 결정적이고 단순하다).
- 한도: `RecordListLimits { maxItems, maxItemBytes }`를 두되 상위 Record 한도(`maxValueBytes`·`maxTotalBytes`)를 넘지 않게 맞춘다. 읽기는 할당 전에 count ≤ 남은 byte / 52(항목 최소 크기)를 확인한다. 패키지처럼 큰 Record를 쓰는 곳은 상위 Record 한도를 그 용도에 맞게 올려 넘긴다(AssetSystem 5.1절 상한).
- 예산: 항목별 하위 reader는 목록 reader와 **누적 예산을 공유**한다(Serialization README의 "예산은 환급하지 않는다" 원칙 — 항목마다 새 예산을 주지 않는다).
- API: `RecordListBuilder`의 `AddItem(Span<const uint8> builtRecord)`(완성된 Record 바이트를 받는다 — RecordBuilder는 중첩 스트리밍 기록이 없다)와 `Finish(RecordBuilder& parent, Guid fieldId)`. AddField가 깊은 복사를 하므로 메모리 피크는 항목·목록·부모의 세 벌까지 커질 수 있다(문서화, 큰 목록은 항목을 순서대로 만들어 바로 AddItem한다). `RecordListReader`는 항목의 schema와 버전 범위를 인자로 받아 항목별 `RecordReader`를 길이 범위로 제한해 넘긴다.
- 오류 코드는 Archive 규약을 따른다: 잘림·길이 불일치·남은 바이트는 `InvalidArgument`, 한도 초과는 `CapacityExceeded`. 실패하면 builder는 부분 결과를 남기지 않는다(호출 전 상태로).
- 시험: 빈 목록(count 0), 중첩 목록, 잘린 길이·count 과대·남은 바이트·length 불일치, 한도, 예산 공유, 실패 원자성.

## 5. 단계와 완료 기준

| 순서 | 내용 | 완료 기준 |
|---|---|---|
| C1(구현됨) | 데이터 전용 ThirdParty 종류(`kind`·`embeddedIn`·`licenseSource`, 도구·정책·SDK 라이선스 복사·시험), UCD 18.0.0 원본 추가(JSONTestSuite는 C5와 함께) | 정책 검사·self-test·SDK 통합 시험 통과 |
| C2(구현됨) | Unicode 생성기·표·API, 정책 검사 `unicode-tables` | NormalizationTest 전체(각 열의 IsNormalized 포함), caseless 키 망라 멱등성, CaseFolding·General_Category·Default_Ignorable, 생성기 결정성 |
| C3 | `DeterministicMath` 모듈·표 생성기·외부 심볼 허용 목록 검사, `LinearColor` sRGB 교체(`FloatingPointEnvironmentGuard`는 A0-P가 PlatformBase에 구현하고 여기서 쓴다) | 기준값·정확도 표·버전 강제·FTZ guard·심볼 검사, macOS arm64 비트 비교 |
| C4 | CRT 없는 숫자 변환(파서·최단 표기)과 NumberText·StringBuilder 교체 | .NET 대조·어려운 사례 표 |
| C5 | JSON 읽기·쓰기 | JSONTestSuite·왕복·고정점·한도·위치 시험 |
| C6 | RecordList(`WireType::RecordList`) | 형식·예산·실패 원자성 시험 |
