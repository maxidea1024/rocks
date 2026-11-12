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

쿠킹 결과는 같은 도구 빌드라면 호스트와 무관하게 같은 바이트여야 한다(AssetSystem 10절). CRT의 `pow`·`exp`·`log`·`sin`은 구현과 CPU 경로(예: UCRT의 FMA3 분기)에 따라 결과가 달라질 수 있다(C3 전에는 `LinearColor::ToSRGB`가 UCRT `pow`를 썼다). 빌드는 이미 `-ffp-contract=off`이고 fast-math를 쓰지 않으며 x87을 쓰지 않으므로, IEEE 기본 연산(+ − × ÷ √, 정수 bit 연산)과 float↔double 변환만 쓰는 구현은 x64·ARM64에서 같은 결과를 낸다. 단 다음 전제를 지킨다.

- 부동소수점 환경: 반올림 모드 nearest, 비정규 수 보존(FTZ·DAZ 끔), 예외 trap 꺼짐. 검사는 상태 플래그를 빼고 한다: x64는 `(MXCSR & ~0x3F) == 0x1F80`(bit 0–5는 보통 연산이 켜는 상태 플래그), ARM64는 FPCR의 FIZ·AH·NEP·trap enable·EBF·FZ16·RMode·FZ·DN·AHP 비트가 모두 0(기능이 없는 CPU에서는 RES0). 에디터 프로세스 안에서는 드라이버·미들웨어가 이 값을 바꿀 수 있으므로 쿠킹·결정적 계산은 PlatformBase의 `RunWithDeterministicFloatingPoint(function, context)` 안에서 실행한다(구현됨). 제어 설정을 기대 값으로 바꾸고 function을 실행한 뒤 제어 설정을 호출 전 값으로 되돌린다(안에서 바뀌었어도). 실행 중 쌓인 상태 플래그는 남긴다.
  - RAII guard로 하지 않는 이유: compiler는 부동소수점 연산이 환경과 무관하다고 보고(FENV_ACCESS off) 같은 함수 안의 연산을 제어 레지스터 쓰기 앞뒤로 옮기거나 합친다. 검증에서 guard 안의 곱셈이 guard 앞의 FTZ 결과로 합쳐지는 것을 재현했다. `#pragma STDC FENV_ACCESS ON`도 inline된 다른 함수의 연산을 `round.tonearest`/`fpexcept.ignore` constrained 연산으로 바꿀 뿐이라 MXCSR 쓰기를 가로질러 합쳐지고, LLVM은 FTZ 변화를 모델링하지 않는다(clang 21 IR로 확인). 그래서 환경을 바꾼 뒤 inline asm으로 세탁한 함수 포인터(ThinLTO도 대상을 알 수 없다)를 호출해 그 안의 연산이 환경 변경 뒤에만 실행되게 한다.
- 암묵적 libm 호출 금지: clang-cl x64는 `floor`·`trunc`·`round`·`fmod`와 음수 분기의 `sqrt`를 UCRT 호출로, Android arm64는 `fmod`를 bionic 호출로 만든다(실측). 그래서 Floor·Trunc·RoundHalfEven·2의 거듭제곱 곱(ldexp 대응)을 bit 연산으로 직접 구현하고, sqrt·fmod를 포함한 CRT 수학 함수를 전혀 부르지 않는다(그러면 math-errno 설정과 무관하다 — clang-cl에서 `-fno-math-errno`는 compile 인자 검증이 거부하는 `/clang:` 형태라 쓰지 않는다). 결정적 수학은 **별도 모듈 `DeterministicMath`**(Core/Source/Runtime/DeterministicMath, PlatformBase의 `BitCast`에만 의존)로 두어 외부 심볼 검사가 그 모듈에만 걸리게 한다. Core(`LinearColor`)는 이 모듈에 의존한다.
- 정수 변환: 범위 밖 값·NaN의 float → int 변환 결과가 x64(0x8000…)와 ARM64(포화)에서 다르므로, 정수 추출은 범위를 먼저 검사하고 shifter 기법(x + 0x1.8p52)으로 한다.
- `long double` 금지(Android arm64에서는 binary128 soft-float라 compiler-rt를 부르고, x86-64 macOS에서는 x87이다) — 정책 검사 `deterministic-math-sources`(아래 2.3).
- NaN: 기본 NaN 비트가 x64(0xFFF8…)와 ARM64(0x7FF8…)에서 다르고, 두 NaN 피연산자의 결과는 순서에 따른다. 그래서 모든 함수는 출구에서 결과가 NaN이면 고정 quiet NaN(0x7FF8000000000000, float는 0x7FC00000)을 반환한다.

### 2.2 API(모듈 `DeterministicMath`, `DeterministicMath/DeterministicMath.h`, namespace `rocks::deterministic`)

```cpp
inline constexpr uint32 Version = 1;   // 결과 비트가 바뀌면 올린다 — 빌드 캐시 키·cookKey에 들어간다
double Floor(double x) noexcept;  double Trunc(double x) noexcept;  double RoundHalfEven(double x) noexcept;
double Exp2(double x) noexcept;   double Log2(double x) noexcept;
double Pow(double base, double exponent) noexcept;   // base ≥ 0만(음수·−Inf는 고정 NaN). Pow(x, ±0) = Pow(1, y) = 1(NaN 입력 포함), Pow(±0, y<0) = +Inf, Pow(+Inf, y>0) = +Inf
double SinPi(double x) noexcept;  double CosPi(double x) noexcept;   // sin(πx)·cos(πx)
double Sinc(double x) noexcept;   double Lanczos(double x, int lobes) noexcept;   // SinPi로 정의
double SrgbToLinear(double encoded) noexcept;   double LinearToSrgb(double linear) noexcept;   // IEC 61966-2-1 구간 함수
double Gamma22ToLinear(double encoded) noexcept;                                             // x^(11/5)(bUseLegacyGamma, 정확한 2.2)
float SrgbByteToLinear(uint8 value) noexcept;    // 생성 표 256칸(정확히 반올림한 값)
uint8 LinearToSrgbByte(float linear) noexcept;   // 경계 표 이분 탐색. NaN·0 이하 → 0, 1 이상 → 255
```

- 구현:
  - `SinPi`·`CosPi`: x − RoundHalfEven(2x)/2는 오차 없이 계산되므로 [−¼, ¼]로 정확히 줄인 뒤 21·22차 Taylor(cos(πx)는 4분면 번호를 하나 옮겨 sin으로). 0의 부호는 IEEE 754-2019 권고(sinPi(±n) = ±0, cosPi(n + ½) = +0). Lanczos·Sinc는 이것을 쓴다(π·x를 먼저 곱하면 정수 근처 0점에서 정확도를 잃는다). Lanczos는 |x| ≥ L/2에서 창 함수 분자를 sin(π(L − |x|)/L)로 계산한다(L − |x|는 Sterbenz로 정확 — 지지 경계 근처의 상대 오차 증폭을 없앤다).
  - 일반 `Sin`·`Cos`는 쓰는 곳(텍스처 파이프라인은 SinPi만 쓴다)이 생길 때 3단 Cody-Waite π/2 축소(fdlibm 방식)로 추가한다.
  - 정밀도가 필요한 곳은 오차 없는 변환(Knuth TwoSum, Dekker 분할 곱 — FMA 없이 기본 연산만)으로 두 double의 합(double-double)을 쓴다.
  - `Log2`: 지수와 m ∈ (√½, √2]로 나누고 f = m − 1(정확), s = f/(2 + f)를 double-double로 나눈 뒤 ln m = 2s + (2/3)s³ + 2s·s⁴·(1/5 + s²/7 + …)의 앞 두 항을 double-double로, 나머지를 double로 계산한다(상대 오차 약 2^−66). 1/ln2도 double-double 상수.
  - `Exp2`(double-double 지수 t): n = RoundHalfEven(t.hi), r = t − n(정확), a = r·ln2(double-double), e^a = 1 + a + a²·Q(a)(Q는 18차 Taylor)에서 큰 항을 정확히 더하고 작은 항을 모아 한 번 반올림한 뒤 2^n을 곱한다(정규 결과는 정확, 비정규 결과는 한 번 더 반올림).
  - `Pow` = Exp2(y · Log2 x)를 지수까지 double-double로 계산한다(|y·log2 x|가 1100을 넘으면 +Inf·0이 확실). sRGB 함수는 상수 0.055·1.055와 밑 (x + 0.055)/1.055를 double-double로, 지수는 정확한 12/5·5/12로 계산하고, LinearToSrgb는 거듭제곱을 double-double로 남겨 1.055·p − 0.055의 상쇄가 오차를 키우지 않게 한다. Gamma22는 정확한 11/5 지수.
  - 계수는 1/n!·1/(2k+1)을 컴파일 시점 IEEE 나눗셈으로 만든 상수다(clang의 상수 접기는 대상과 무관하다). ln2·1/ln2·0.055·1.055(각 hi·lo)·π·√2는 비트 패턴으로 고정한다. 정확도와 무관하게 **결과 비트의 결정성**이 계약이다.
  - 정확도(가장 가까운 double까지의 비트 거리, 2.3의 생성 기준값 표로 시험): 상한 Exp2·Log2 1, Pow·SinPi·CosPi·sRGB·Gamma22 2(비정규 결과는 1 더). 측정 최대: Log2 0, 나머지 모두 1(교차검증에서 찾은 최악 입력 — 예: Pow(0.77…, 2701.28…) 이전 9 ulp — 포함).
- 표 생성(`Rocks.ToolTests math-tables --root . [--write]`, 정책 검사로 갱신 여부 확인): x^2.4·x^(1/2.4)·x^2.2는 유리수가 아니므로 반올림 경계 판정을 정수 비교로 한다. 지수 12/5이면 후보 값 m에 대해 m^5와 q^12를 BigInteger 분수로 정확히 비교한다(2.2 = 11/5, 1/2.4 = 5/12도 같은 방식).
  - `SrgbByteToLinear` 표: decode(k/255)를 정확히 반올림한 float.
  - `LinearToSrgbByte` 경계: t_k = decode((k + 0.5)/255)(부호화 공간의 중간점 — AssetSystem 12.2절의 round-half-up 양자화와 같다)이고, 표에는 t_k 이상인 가장 작은 float를 저장한다(가장 가까운 float가 아니다). 그래서 8비트 경로는 정확한 반올림이고, 16비트·float 경로(`LinearToSrgb`)는 근사 Pow를 쓰는 다른 의미다(문서화).
- Core `LinearColor::FromSRGB`·`ToSRGB`는 이 표·함수로 바꾼다(Core 안 sRGB 구현을 하나로 — 런타임과 쿠킹 결과가 어긋나지 않게).

### 2.3 시험

- 기준값: 범위 경계·특수값·정확한 경우(비정규 포함 2^n의 Exp2·Log2, 정수·반정수의 SinPi·CosPi와 0의 부호, Pow 특수값, Lanczos 비트 대칭 등)는 개별 시험으로, 나머지는 아래 digest로 고정한다. 같은 foundation 시험이 Windows x64·macOS arm64(CI)·Android arm64(arm64 기기 — x64 에뮬레이터는 해당 없음)에서 돌므로 호스트 사이 비트 비교가 된다. Windows는 Debug와 Release(ThinLTO)의 digest가 같음을 확인했다. float 입력 함수(`LinearToSrgbByte`)는 float 비교만 하고 반올림이 없어(경계 표의 이분 탐색) 구성상 호스트와 무관하다(교차검증에서 2^32 전수를 기본 환경과 FTZ+DAZ+RZ 환경에서 비교해 차이 0).
- 정확도: `Rocks.ToolTests math-tables`가 BigInteger 400비트 고정소수점으로 계산한 기준값 표(`Tests/DeterministicMathReference.generated.h` — 함수별 고정 시드 xorshift 표본 150–230개와 교차검증의 최악 입력, 정책 검사로 갱신 여부 확인)와 비교해 2.2의 상한을 검사한다. 생성기 값은 Python decimal 독립 계산과 대조했다.
- 버전 강제: 모든 함수와 분기(비정규 결과·입력, |x| ≥ 2^52의 SinPi·CosPi, 여러 lobes, 범위 밖 sRGB, Gamma22·Sinc, byte 표 256칸, LinearToSrgbByte)를 덮는 고정 시드 LCG 표본 4096개의 출력 비트 FNV-1a digest와 비정규 결과 digest를 `Version`과 함께 시험에 고정한다(`DeterministicMath_GoldenDigest`). 결과가 바뀌었는데 Version을 올리지 않으면 실패한다.
- 소스 정책(`deterministic-math-sources`, `Policy.DeterministicMathSources`): Private은 생성 표·자기 공개 헤더·`Platform/Memory.h`·`Base/Types.h`만 include한다(PlatformMath·CRT 수학 헤더는 header inline 근사 명령을 끌어와 외부 심볼에 보이지 않는다). 공개 헤더는 `Base/Types.h`만 include하고 함수 본문을 두지 않는다. long double을 쓰지 않는다.
- 오브젝트 검사(`DeterministicMathObjectCheck`, 도구 시험 `Native.DeterministicMathObjects`, Android 교차 빌드에서는 host 전용 `objects` 명령): 이번 구성의 compile DB에서 `DeterministicMath/Private` 번역 단위 오브젝트를 찾는다. Release ThinLTO bitcode는 compiler 옆 clang으로 `-O2 -x ir` 기계어를 만든 뒤 검사한다(backend가 libcall·명령을 고르기 전이라 bitcode로는 볼 수 없다). 도구는 compiler 옆 → PATH → (macOS) Xcode `nm`·`objdump` 순으로 찾는다.
  - 외부 심볼(`llvm-nm -u -j`)이 코드 생성 지원 심볼뿐인지: `_fltused`·`/GS` cookie·`__chkstk`(`__chkstk_darwin`)·stack protector·`memcpy`/`memmove`/`memset`·`bzero`(Darwin), Asan 구성이면 sanitizer 계측(`__asan_` 등)과 Darwin의 module dtor 등록 `__cxa_atexit`·`__dso_handle`. CRT 수학 함수와 다른 모듈 함수는 허용하지 않는다. 측정: Windows Debug는 `_fltused`·cookie 2개, O2·Release는 `_fltused`만.
  - 금지 명령(`llvm-objdump -d`, 아키텍처는 출력의 file format으로): 융합 곱셈-덧셈(x86 `vfmadd…`·`vfnmsub…`, AArch64 `fmadd`·`fnmsub`·`fmla`·`fmls`, SVE `fmad`·`fmsb`), AArch64 근사·보조 명령(`frecpe`·`frsqrte`·`frecps`·`frsqrts`·`frecpx`), x86 근사 명령(`rcp`·`rsqrt` SSE·AVX-512)과 x87 명령. `-ffp-contract=off`가 빠지거나 무시되는 회귀와 근사 명령 유입을 잡는다.
  - 음성 대조(`Native.DeterministicMathObjectsNegativeControl`): `sin` 호출과 `__builtin_fma`(x64는 `/arch:AVX2`·`-mfma`)가 든 probe를 같은 compiler로 만들어 두 위반이 모두 보고되는지 본다(도구 출력 형식이 바뀌어 검사가 아무것도 못 보는 경우). 규칙 단위 시험은 `Policy.DeterministicMathObjectRules`.
- 환경 실행(`FloatingPointEnvironmentTests`·`DeterministicMath_EnvironmentRunRestoresResultsUnderFlushToZero`): FTZ·DAZ·toward-zero 반올림을 켠 상태에서 실행 안의 결과가 기본 환경과 같은 비트이고, 실행 뒤 원래 제어 설정이 돌아오며, 안에서 바꾼 설정도 되돌리고, 중첩되며, x64 상태 플래그는 남는지 본다. 비정규 결과 Exp2 digest는 실행 밖(FTZ)에서 달라져 민감도를 확인한다.
- `LinearToSrgbByte(SrgbByteToLinear(b)) == b`(256값), 단조성, 모든 경계 t_k와 바로 아래 float의 byte(k + 1·k)와 부호화 값(k + 0.5 이상·미만).

## 3. JSON(RFC 8259)

### 3.1 용도와 요구

`Config/AssetAliases.json`·`AssetSources.json`·`CookSettings.json`·`BuildCache.json`, `Engine/Config/TargetProfiles/*.json`, 도구 보고서. 버전 관리되는 파일을 도구가 다시 쓰고 그 SHA-256이 쿠킹 키에 들어가므로 **결정적 출력**(CRT·locale 무관)이 필요하고, 사람이 고친 파일을 읽으므로 **엄격하고 친절한 오류**가 필요하다.

### 3.2 숫자 변환(CRT 없이)

- 지금 `TryParseDouble`은 strtod·locale 사본을 쓰고(1023 byte 초과 토큰 거부), `FormatShortestFloatingPoint`는 snprintf·strtod 이분 탐색을 쓴다. 둘 다 CRT 구현에 의존하므로 Core에 CRT 없는 구현을 넣는다: 정확히 반올림하는 10진 → 2진(Eisel-Lemire + 큰 정수 보조 경로, 길이 제한 없음 — 입력 상한 안), 최단 왕복 2진 → 10진(Ryu 계열). NumberText·StringBuilder가 이것을 쓰도록 바꾼다(공개 계약은 같다).
- 구현(구현됨, `Core/Private/Text/DecimalConversion`, 정수 연산만 — CRT·locale·부동소수점 환경과 무관):
  - 해석: 유효 숫자를 769자리까지 보관하고(그 뒤는 0이 아닌지만 — double 두 이웃 값의 중간점은 768자리 이하, float는 113자리 이하) 지수는 10^17 이상이 되면 더 읽지 않는다(자릿수에서 오는 소수점 이동은 입력 byte 수 이하라, 상쇄하려면 입력이 10^17 byte를 넘어야 한다 — 10^9로 두었을 때 1 GiB 입력이 상쇄하는 것을 교차검증에서 재현해 고쳤다). 10^309 이상은 무한대(Overflow), 10^−324 미만은 0으로 먼저 판정해 표 범위 q ∈ [−342, 308]를 보장한다. 앞 19자리 w와 5^q·2^s의 128비트 내림 표 X(`number-tables` 생성기, BigInteger)로 참값을 [w·X, w′·(X + 1))(잘렸으면 w′ = w + 1, q ∈ [0, 55]면 X가 정확) 사이에 두고 두 경계를 반올림한 결과가 같으면 그것이 답이다(반올림의 단조성 — Eisel-Lemire의 무폴백 증명에 기대지 않는 변형). 다르면 5120비트 고정 용량 큰 정수로 10진 값과 두 후보의 중간점을 정확히 비교한다(동점은 짝수, 잘린 꼬리는 큼). float도 같은 경로로 바로 반올림한다(이중 반올림 없음). 입력 길이 제한은 없다(옛 1023 byte 제한 제거).
  - 부동소수점 환경과의 무관함은 경계까지 지킨다: 해석 결과·서식 입력을 IEEE 비트로 주고받고 float ↔ double도 정수 연산으로 넓힌다. 교차검증에서 값을 변환 명령에 통과시키던 경계가 FTZ에서 float 서식 무한 루프, DAZ에서 비정규 수를 "0"으로 쓰고 `TryParseFloat`가 0을 내는 것을 재현해 고쳤다(FTZ·DAZ를 켠 회귀 시험).
  - 최단 표기: Ryu d2d를 형식 매개변수로 일반화했다. 같은 e2에서 계산이 double과 같고 가수 범위만 작으므로 double용 125비트 표(생성)를 float에도 쓴다. 끝 0 판정은 원본의 범위 자르기(q ≤ 21) 없이 정확히 계산한다.
  - 정밀도 지정 서식('f'·'e'·'g'): 정확한 10진 전개(정수부와 (m mod 2^k)·5^k의 k자리 소수부)를 가장 가까운 값(동점은 짝수)으로 자른다 — glibc·UCRT(Windows 10 2004 이후) printf와 같은 규칙. %g의 고정·과학 선택과 끝 0 제거는 C 표준 규칙이다.
- 시험(구현됨):
  - Core: 알려진 어려운 사례 표(Overflow와 문법 오류를 구별)(2^53 + 1의 동점, 최대 비정규·최소 정규 경계, 넘침 경계, `1e23`, float 경계), 2^−1075 정확 중간점(750자리 이상)과 2900자리 꼬리, ToBinary와 독립인 기준값(중간점 비교만 쓰는 이분 탐색)과 무작위 1500건·이웃 double 중간점과 그 위아래 900건 대조, 최단 표기의 최단성·최근접성을 정확한 전개로 독립 확인(double·float 무작위 8000건과 경계 — .NET이 틀리는 2^−25·2^−958 포함), 정밀도 서식의 정확한 반올림 사례, float → double 정수 확장, FTZ·DAZ 환경의 해석·서식. 교차검증은 float 최단 표기 전수(21억 개)·double 14.5억 개를 MS STL `to_chars`·Python과, 해석 237만 건을 Python 정확 오라클과 대조해 불일치 0이었다.
  - .NET 대조(`Native.NumberConversionMatchesDotNet`): 고정 시드 xorshift로 최단 표기 double 20만·float 10만, 해석 20만(무작위 1–900자리·이웃 중간점과 그 위아래, double·float)을 .NET 10의 `ToString()`·`double.Parse`·`float.Parse`와 비교해 불일치 0. .NET 10의 최단 표기는 2의 거듭제곱 중 2^−25·2^−958 두 값에서 왕복하지 않는 16자리를 낸다(예: 2^−25 → `2.980232238769531E-08`, Python repr과 Rocks는 17자리 `2.9802322387695312e-08` — 직접 확인). 이 둘은 올바른 문자열로 고정해 대조하고, 그 밖에 .NET 기준이 왕복하지 않으면 말뭉치 생성이 실패한다(조용히 빼지 않는다). 표본은 2의 거듭제곱과 그 바로 아래, 비대칭·넘침 경계 중간점, float 범위에 맞춘 지수를 포함한다.
  - 정책 `crt-number-conversion`: Core·Engine의 런타임과 개발 도구(쿠킹 결정성, 시험 제외)는 CRT 실수 해석(`strtod` 계열 POSIX·MSVC·wide 변형, `atof`)·scanf 계열·locale 함수·cvt 계열과 실수 변환(`%f`·`%e`·`%g`·`%a`)이 있는 printf 계열을 쓰지 않는다. 주석·문자열 리터럴을 지운 뒤 식별자를 비교하고 멤버·한정 이름은 제외한다(함수 포인터·여러 줄 호출도 잡는다). self 시험이 생성 표 최신성(`Policy.RepositoryNumberTables`)과 정책 양성·음성 대조를 줄 단위로 본다.

### 3.3 읽기(`Core/Text/Json.h`)

- 결과: `JsonDocument::Parse(input, options)`가 `JsonError { code, position }`를 돌려준다(성공이면 code None, 실패하면 문서가 빈다). 경고는 `Warnings()`(`JsonWarningCode::BomIgnored` 등), 설명 문자열은 `JsonErrorMessage(code)`다. 문서는 핸들(`JsonValue`)이 문서 포인터를 들고 있어 이동·복사하지 않으며, 다시 Parse·Clear하면 세대 번호로 이전 핸들이 무효가 된다.
- 입력은 UTF-8. 맨 앞 BOM 하나는 건너뛰고 `BomIgnored` 경고를 남긴다(RFC 8259 §8.1의 MAY — PowerShell 5.1·Visual Studio가 기본으로 붙인다). 주석·끝 쉼표·작은따옴표·NaN/Infinity 리터럴·잘못된 UTF-8·짝 없는 surrogate 이스케이프(`\uD800`)는 거부.
- 같은 객체 안 중복 키는 오류다(조용히 덮어쓰지 않는다). 판정은 이스케이프를 푼 뒤 byte 비교이며 Unicode 정규화는 하지 않는다(키 `a`와 이스케이프로 쓴 키 `a`(역슬래시·u·0061)은 같은 키, NFC·NFD 차이는 다른 키).
- 숫자 모델: 소수부·지수가 없는 토큰만 정수 종류(int64, 넘으면 uint64, 그보다 크면 double)이고 나머지는 double(3.2절 파서). double로도 표현할 수 없는 크기(예: `1e400`)는 `NumberOutOfRange` 오류다(±Inf로 바꾸지 않는다). `-0`은 double −0.0. 원문 토큰도 보관한다(Preserve 쓰기용). 값 비교는 수치 기준.
- 위치: 줄·열은 1부터, CRLF는 줄바꿈 1개, CR 단독도 줄바꿈, 열은 코드 포인트 단위(tab은 1칸), byte offset을 함께 보고한다(`JsonPosition { line, column, byteOffset }`, 오류 위치는 잘못된 값·문자·이스케이프의 시작, 건너뛴 BOM은 세지 않는다).
- 한도(`JsonReadOptions`로 조정, 기본): 입력 64 MiB, 깊이 128, 문자열 하나 16 MiB, 노드 수 4,194,304. 파서는 반복형(명시 스택)이다(`n_structure_100000_opening_arrays`).
- DOM: `JsonDocument` — `JsonValue`(Null·Bool·Integer·Unsigned·Double·String·Array·Object의 가벼운 핸들), 객체는 원문 순서를 보존하는 (키, 값) 배열 + 조회용 해시.
- 구현(구현됨, `Core/Text/Json.h`): ArenaAllocator는 소멸 전에 모든 할당이 반환돼야 하므로 노드·배열 원소 색인·객체 멤버·문자열 byte를 allocator의 평평한 배열 네 개에 담는다(할당 수가 적고 해제가 단순하다). 반복형 파서는 컨테이너마다 frame을 쌓고 자식을 대기 배열에 모았다가 닫을 때 최종 배열로 옮긴다. (객체 노드, 키 byte) 하나의 해시 색인이 중복 키 판정과 조회를 함께 맡는다. 줄·열은 오류·경고 때만 앞에서부터 다시 센다(정상 경로 비용 없음). 숫자 분류는 Core NumberText(C4)를 쓰고 원문 토큰을 보관한다. 입력은 1 GiB 이하(위치가 uint32).

### 3.4 쓰기

- `JsonWriter`(StringBuilder 대상)의 byte 단위 고정 규칙: 들여쓰기 2칸, 줄바꿈 LF, 끝에 줄바꿈 1개, 키 구분자 `": "`, 빈 컨테이너 `{}`·`[]`, 키 순서는 호출자 순서(정렬이 필요한 파일은 호출자가 AssetSystem 4.2절 정렬로 넣는다). 문자열 이스케이프는 `"`·`\\`·제어 문자만(U+0008·U+000C·U+000A·U+000D·U+0009는 `\b\f\n\r\t`, 그 밖은 소문자 16진 `\u00xx` — 키 안의 U+0000 포함), 그 밖의 문자는 UTF-8 그대로. 호출자가 준 문자열은 UTF-8을 검증한다. BOM은 쓰지 않는다.
- 모드: **Canonical**(기본 — 정수 종류는 10진, double은 최단 왕복 표기이며 결과에 `.`·`e`가 없으면 `.0`을 붙여 다시 읽어도 double이 된다, −0.0은 `-0.0`) — 읽기 → 쓰기 → 읽기가 같은 DOM이고 쓰기 결과는 고정점이다. **Preserve**(원문 숫자 토큰 그대로 — 사람이 쓴 표기를 유지해야 하는 편집 도구용, 고정점 보장 없음). NaN·Inf는 쓰기 오류.
- 쓰기 보강(구현됨): 같은 객체의 중복 키는 InvalidArgument(자기 reader가 거부하므로), 깊이 한도는 reader와 같은 128 기본값(생성자 인자 — 더 깊게 읽은 문서는 같은 한도의 writer로 쓴다), DOM 쓰기(`Value`)와 비교(`JsonEquals`)는 재귀하지 않는다(10만 단계 시험). `JsonEquals`는 노드가 전위 순서로 쌓여 서브트리가 연속 구간이라는 점을 써서 두 구간을 한 번 훑고, 숫자는 비트를 정수로 풀어 정확히 비교한다(부동소수점 환경과 무관 — DAZ 시험). 입력 문자열이 출력 builder의 View()여도 된다(복사 후 쓴다).
- 원자적 파일 기록은 호출자가 `FileSystem::Replace` 흐름으로 한다(AssetSystem 5.5절과 같은 임시 파일 규칙).

### 3.5 시험

- JSONTestSuite(nst/JSONTestSuite, MIT, 릴리스가 없어 commit 고정)의 y_·n_ 사례 전체. 이 문서의 선택과 다른 사례는 표로 고정한다: `y_object_duplicated_key`·`y_object_duplicated_key_and_value`(중복 키 거부 — 의도한 예외), i_ 사례 각각의 결정(BOM은 받음 등). 데이터는 `Core/Source/ThirdParty/JsonTestSuite/JSONTestSuite-<commit>/`에 데이터 전용 라이브러리(1.2절)로 둔다(`.gitattributes`의 `-text` 규칙이 `Core/Source/ThirdParty/*/*/**`에 걸려 CR·NUL·BOM·잘못된 UTF-8 파일이 보존된다).
- 왕복·고정점(Canonical), 중복 키, 한도, 오류 위치(CRLF·CR·tab), 숫자 경계.
- 구현됨(`JsonTests.cpp`): 사례 목록은 데이터 라이브러리 manifest의 files를 이 파서로 읽어 얻는다. 318개 중 99개 받음·219개 거부, i_ 35개는 결정 표와 같다. 받은 사례 모두 Canonical 왕복·고정점과 Preserve 왕복을 확인한다.
  - i_ 결정: 받음 — 0으로 underflow하는 지수, int64·uint64를 넘는 정수(double), BOM 하나(경고). 거부 — double 범위 밖 지수, 짝 없는 surrogate, UTF-8이 아닌 입력(UTF-16·Latin-1·overlong·잘림·범위 밖), 500단계 중첩(기본 깊이 한도 128 — 한도를 올리면 받는다).
  - 그 밖: 쓰기 상태 기계 오용 8종, 이스케이프 규칙(NUL·0x1F 소문자, '/'·U+007F·U+2028 그대로), 최단 표기 배치 경계(1e16·1e-4·비정규), 2^53 + 1 정수, 출력 builder 별칭, 자기 문자열 별칭 Parse, 같지 않은 쌍 15종의 JsonEquals(키 순서·정수와 정수 아닌 double·2^63·2^64 경계), 오래된 핸들, surrogate 쌍 도중 끝·두 번째 이스케이프 위치·한도 초과 값 시작 위치. 왕복은 JsonEquals와 별도로 종류·비트까지 엄격 비교한다. 교차검증: 파서 퍼징 약 40만 건(참조 파서·Python 대조)과 writer 무작위 문서 22.5만 건·double 60만 건의 고정점, JsonEquals 152만 쌍 정확 대조에서 불일치 0, 변이 시험 16종 중 12종이 살아남던 시험 공백을 위 시험으로 메웠다.
  - `n_structure_trailing_#.json`은 manifest 경로 규칙 때문에 `n_structure_trailing_hash.json`으로 이름만 바꿨다(내용은 그대로).

## 4. RecordList(Serialization)

- 형식: Record에 새 wire type `WireType::RecordList`를 추가한다(범용 도구·리플렉션이 목록을 구분한다). payload = `u32 count + count × (u64 length + 중첩 Record)`. 각 항목의 length는 중첩 Record의 전체 길이(헤더 44 byte + payload)와 같아야 한다. 빈 목록도 count 0으로 기록한다(Record에는 "없으면 기본값" 규칙이 없고 ObjectSerialization은 아는 필드가 없으면 NotFound다 — 결정적이고 단순하다).
- 한도: `RecordListLimits { maxItems, maxPayloadBytes, maxItemBytes }`를 둔다. 기본값(payload 4 MiB, 항목 4 MiB − 12)은 기본 `maxValueBytes`(4 MiB)에 맞춰 기본 한도의 목록이 기본 한도의 부모 값 하나에 들어가게 한다. builder와 reader가 같은 한도를 판정하고, 목록 archive의 크기 한도는 항목 Record 한도와 분리한다. 읽기는 할당 전에 count ≤ 남은 byte / 52(항목 최소 크기)를 확인하고 항목 파싱 전에 count만큼 work를 청구한다. 패키지처럼 큰 Record를 쓰는 곳은 상위 Record 한도를 그 용도에 맞게 올려 넘긴다(AssetSystem 5.1절 상한).
- 예산: 항목별 하위 reader는 목록 reader와 **누적 예산을 공유**한다(Serialization README의 "예산은 환급하지 않는다" 원칙 — 항목마다 새 예산을 주지 않는다).
- API: `RecordListBuilder`의 `AddItem(Span<const uint8> builtRecord)`(완성된 Record 바이트를 받는다 — RecordBuilder는 중첩 스트리밍 기록이 없다)와 `Finish(RecordBuilder& parent, Guid fieldId)`. builder는 완성된 payload를 기하 성장 buffer에 쌓고(항목마다 전체를 다시 복사하지 않는다) Finish가 부모에 한 번 복사한다. 목록 데이터의 사본은 builder buffer(성장 중 이전·새 buffer)·부모 builder·부모 Build 결과로 여러 벌이 되므로, 큰 목록은 항목을 순서대로 만들어 바로 AddItem하고 Finish 뒤 builder를 일찍 파괴한다(문서화). `RecordListReader`는 항목의 schema와 버전 범위를 인자로 받아 항목별 `RecordReader`를 길이 범위로 제한해 넘긴다.
- 오류 코드는 Archive 규약을 따른다: 잘림·길이 불일치·남은 바이트는 `InvalidArgument`, 한도 초과는 `CapacityExceeded`. 실패하면 builder는 부분 결과를 남기지 않는다(호출 전 상태로).
- 시험: 빈 목록(count 0), 중첩 목록, 잘린 길이·count 과대·남은 바이트·length 불일치, 한도, 예산 공유, 실패 원자성.
- 구현(구현됨, `Serialization/RecordList.h`, `WireType::RecordList = 11`): 예산 공유는 Archive에 공유 누적 예산 `ArchiveBudget`(한도 포함, 원자적 누적·포화)을 두어 Record 트리 단위로 했다. `RecordReader::Open`이 트리 예산을 만들고, `RecordReader::OpenValue`(값 reader)·`RecordListReader::OpenField`(중첩 목록)·`OpenItem`(항목 reader)이 모두 같은 예산을 `SharedPtr`로 공유한다 — 파괴·이동 순서 제약이 없고, 서로 다른 스레드의 공유 reader도 안전하다. 실패한 열기 호출도 operation을 소비하고, 이동된 `ArchiveReader` 원본은 빈 입력이 된다. 교차검증에서 처음 설계(목록 한 단계만 공유, 이동 가능한 raw 포인터)의 결함을 재현해 바꿨다: 목록 reader 이동·이동 대입 뒤 항목 reader의 해제 메모리 사용(ASan), 스레드 간 카운터 갱신 손실, 중첩 목록·값 reader가 새 예산을 받아 트리 순회가 maxWork의 17.6배까지 통과. 시험(`RecordListTests.cpp`): 왕복·빈 목록·중첩, 잘못된 payload 7종, builder·reader 한도, 목록 파싱·항목 열기·조회의 operation·work 공유와 각 한도, 범위 밖 OpenItem 청구, 이동 생성·이동 대입·목록 먼저 파괴 뒤 항목 reader, 이동된 ArchiveReader 원본, 부모 → 목록 → 항목 → 중첩 목록 → 값 트리의 단일 예산(트리 사용량 − 1 한도에서 CapacityExceeded), 두 스레드의 정확한 합산, 부모 AddField 실패·모든 할당 지점 실패의 원자성.

## 5. 단계와 완료 기준

| 순서 | 내용 | 완료 기준 |
|---|---|---|
| C1(구현됨) | 데이터 전용 ThirdParty 종류(`kind`·`embeddedIn`·`licenseSource`, 도구·정책·SDK 라이선스 복사·시험), UCD 18.0.0 원본 추가(JSONTestSuite는 C5에서 추가됨) | 정책 검사·self-test·SDK 통합 시험 통과 |
| C2(구현됨) | Unicode 생성기·표·API, 정책 검사 `unicode-tables` | NormalizationTest 전체(각 열의 IsNormalized 포함), caseless 키 망라 멱등성, CaseFolding·General_Category·Default_Ignorable, 생성기 결정성 |
| C3(구현됨) | `DeterministicMath` 모듈·표 생성기(sRGB 표·정확도 기준값)·소스 정책·오브젝트 검사, `LinearColor` sRGB 교체, PlatformBase `RunWithDeterministicFloatingPoint` | 기준값·정확도 표·버전 강제(digest)·환경 실행·오브젝트 검사(음성 대조 포함), macOS arm64 비트 비교(CI의 digest 시험) |
| C4(구현됨) | CRT 없는 숫자 변환(파서·최단 표기·정밀도 서식)과 NumberText·StringBuilder 교체, `number-tables` 생성기, 정책 `crt-number-conversion` | .NET 대조·어려운 사례 표·독립 기준값(이분 탐색·최단성) |
| C5(구현됨) | JSON 읽기·쓰기, JSONTestSuite 데이터 라이브러리 | JSONTestSuite·왕복·고정점·한도·위치 시험 |
| C6(구현됨) | RecordList(`WireType::RecordList`), Record 트리 공유 예산(`ArchiveBudget`, `OpenValue`·`OpenField`) | 형식·예산·실패 원자성 시험 |
