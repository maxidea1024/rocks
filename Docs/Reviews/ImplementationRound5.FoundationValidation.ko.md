# GC·Reflection·Serialization 확장 검증

기준일: 2026-11-10. 이번 변경은 worker 실행, 비이동 점진·병렬 GC, Clang 생성 메타데이터, typed 필드 접근과 객체 그래프 저장·복원을 연결한다. 전체 Core/Foundation Gate 또는 제품용 엔진 완료를 뜻하지 않는다. 수학·기본 충돌 및 상위 엔진 기능은 이번 범위에 포함하지 않는다.

## 구현과 독립 검토

| 영역 | 구현 범위 | 검토와 추가 검증 |
|---|---|---|
| Task | 실제 worker, 선행 handle 의존성, 협력적 취소, 종료 drain, 결과 handle 수명 분리 | root의 코드 대조와 다른 담당자의 중첩 재진입 반례 구성 |
| GC | 비이동 chunk, slice 사이 mutator 재개, 삽입 장벽, 도달성 cutoff, 병렬 marker drain, 작업량·soft 시간 예산 | 2명의 독립 검토와 별도 인접 그래프 oracle, 부분 제출 OOM·worker 재진입 회귀 |
| Reflection | 실제 Clang JSON AST, stable ID, 상속 metadata·필드 flatten, typed getter/setter, 자동 Trace | root의 생성·필드 접근·등록 수명 대조, 실제 Clang 음성 하네스 |
| Serialization | little-endian Archive, GUID/TLV Record, 한도·실패 원자성, 소유·비소유 입력 구분 | 다른 담당자의 전체 byte/bit 변형과 비영점 cursor·자기 alias 시험 |
| ObjectGraph | cycle/shared identity, 버전 migration 경계, 사전 검증, 비공개 생성·fixup·Commit | root와 독립 담당자의 수명·원자성 대조, 생성 코드에서 저장·복원·수명 종료까지 연결 |

작성자와 검토자를 분리하여 1차 API 검토, 실제 구현·반례 검토, 수정 후 코드 및 실행 증거 대조를 수행한다. 검토자가 작성한 독립 시험도 일반 Foundation 실행 대상이다. 상세 반례는 [Worker·GC 교차 검토](ImplementationRound5.WorkerGcCrossReview.ko.md)와 [ObjectGraph 교차 검토](ImplementationRound5.ObjectGraphReview.ko.md)에 보존한다.

## 검출 사항과 수정

- **WG01 / P1:** A.Submit의 allocator가 B.Submit을 호출하고 B의 allocator가 A.Shutdown을 호출하면 단일 TLS 포인터로 바깥 Submit을 발견하지 못하여 교착할 수 있었다. 활성 제출 프레임 전체를 검색하는 할당 없는 연결 스택으로 수정하였다. 정상·중첩 OOM 회귀 2개가 Function 보존, reservation 반환, 바깥 종료 거부와 최종 종료 성공을 검사한다.
- **OG01 / P2:** metadata IsA와 native setter의 정확한 타입 조건이 달라 사전 검증을 통과한 뒤 생성자 실행 후에야 복원을 거부하였다. Save와 Load가 동일한 등록 native type을 비교하도록 변경하였다. 생성자 호출 0으로 거부하는 사례와 같은 native type의 별명 TypeId 저장·재복원 사례를 독립 시험한다.
- **MC01:** annotation 버전의 하위 integer literal을 읽으면 `-1`, `1 + 2`의 의미를 오해할 수 있었다. Clang의 ConstantExpr 평가값으로 판정하고 음수·범위 밖 값·계산식과 문자열 pointer expression 회귀를 추가하였다.
- **AR01:** Record 변형 시험의 LCG 하위 비트 선택은 76바이트 중 38개 위치에만 도달하였다. 상위 비트를 사용하여 모든 위치에 도달하도록 변경하고, 별도의 608개 byte×bit 전수 변형 시험을 추가하였다. 인증 없는 포맷이므로 모든 변형의 거부를 요구하지 않고, 성공한 해석의 범위·반복·한도를 검사한다.
- **GC 예산 계약:** Begin의 루트 스캔, 잠금 대기, 회수 후보 탐색과 사용자 callback을 hard deadline으로 제한하지 않음을 명시하였다. 작업량 제한과 프레임 시간 보장을 혼동하지 않는다.

## 테스트 구성

기존 110개 C++ fixture에 69개를 추가하였다. 새 범위는 Worker 12개, Archive/Record 23개, 점진 GC 11개, Reflection 10개, ObjectGraph 12개, metadata 접근 수명 1개다. Core-only 구성은 118개, 객체 시스템 포함 구성은 179개다. 숫자는 사례 수이며 입력 변형·N-fail 반복을 각기 별도 fixture로 부풀리지 않는다.

CTest는 일반 구성 10개, Core-only 8개, ASan 11개 묶음으로 구성한다. 생성 메타데이터 freshness와 실제 Clang 음성 검사 15개 그룹을 포함한다. 기존 경고 오류 정책, 공개 헤더 단독 컴파일, STL·플랫폼 경계 감사, 하네스 음성 대조, 빌드 증거 자체 검사, 템플릿 트랜잭션 검사를 계속 실행한다. ASan은 의도적인 use-after-free가 실제 sanitizer 오류로 거부되는 대조군을 추가한다.

## 최종 Windows 로컬 실행

Clang 21.1.6, seed 7351로 다음 구성을 실제 실행하였다. 기존 110개에 worker·GC·Reflection·직렬화·metadata 접근 69개를 추가한 전체 179개다.

| 구성 | C++ fixture | CTest | 결과 |
|---|---:|---:|---|
| Windows Core-only Debug | 118개 | 8개 | PASS |
| Windows 전체 Debug | 179개 | 10개 | PASS |
| Windows 전체 Release | 179개 | 10개 | PASS |
| Windows 전체 ASan | 179개 | 11개 | PASS |

네 구성 모두 sourceStableDuringRun=true, runnerReportCurrent=true, testsStatus=PASS이며 실제 report SHA-256과 evidence가 일치한다. 소스 입력 147개의 digest는 `a321125b22a4ca628f5bbc15b3bb0f74b9ab1bfda003e46bb33ffa015e75b27a`다. HEAD `b38c9437b32dcd41837b5df7b8ab2a1dbc622272` 위 dirty 작업 트리 시험이며, clean commit 실행과 구분한다.

같은 소스로 Release 전체를 100회 반복하여 **179 × 100 = 17,900회** 실패 0을 확인하였다. repeat-report와 repeat-evidence를 기본 빌드 보고서와 분리하였다. 변형·N-fail 내부 반복을 이 숫자에 추가하지 않는다. 실제 Clang 메타 생성과 15개 음성 검사 그룹, 생성 헤더 단독 컴파일, 경고 오류·STL·플랫폼 경계 검사, ASan UAF 음성 대조가 포함된다.

독립 회귀에는 중첩 scheduler Submit의 바깥 Shutdown 교착 방지, 부분 marker 제출 OOM 후 drain, generated metadata 기반 cycle의 저장·복원 및 scratch 파괴 후 접근, native 참조 불일치의 생성 전 거부, 같은 native 타입의 별명 TypeId 보존이 포함된다. 이 검증 기록에 수정 근거와 범위 제한을 기록하였다.

## 최종 Windows/macOS CI

구현 커밋 `a310ba5dcd5f19eff4e8bf43a3d35d468a067e20`의 [CI 실행 42775786556](https://github.com/maxidea1024/rocks/actions/runs/42775786556)에서 Windows 및 macOS job이 모두 성공하였다. 원본 artifact의 report, build-evidence, CTest 로그를 내려받아 실행 목록·결과·해시를 직접 대조하였다.

| CI 구성 | 실제 compiler·architecture | Foundation | CTest | 결과 |
|---|---|---:|---:|---|
| Windows Debug | clang version 20.1.8 · x64 | 179개 | 10개 | PASS |
| macOS 15 Debug | Apple clang version 17.0.0 · arm64 | 179개 | 10개 | PASS |
| macOS 15 ASan | Apple clang version 17.0.0 · arm64 | 179개 | 11개 | PASS |


세 구성 모두 위 HEAD, dirty=false, sourceStableDuringRun=true, runnerReportCurrent=true다. source digest `a321125b22a4ca628f5bbc15b3bb0f74b9ab1bfda003e46bb33ffa015e75b27a`와 147개 입력 파일은 로컬 네 구성 및 17,900회 반복과 동일하다. 실제 report SHA-256도 각각 evidence와 일치한다. 새 독립 회귀를 포함한 179개 이름의 집합, seed 7351, 메타 컴파일러 15개 그룹, 경고 오류·플랫폼/STL 감사의 실행 성공을 확인하였다. macOS ASan은 실제 UAF 음성 대조를 포함한다.

artifact는 `out/ci-42775786556`에 보관한다. 별도의 검토자가 로컬 결과와 CI 원본 산출물을 독립 대조하여 같은 커밋·소스·실행 목록·보고서 해시 및 실제 음성 대조군 성공을 확인하였다. 불일치는 없었다. 문서만 갱신한 후속 커밋은 이 C++/빌드 입력 digest를 변경하지 않는다. 이는 CPU Foundation subset의 native 실행 결과이며 전체 엔진·Intel Mac·콘솔·TSan 검증으로 확대하지 않는다.

## 검증 범위의 제한

allocator ledger, 객체·lease 계수, ASan과 실패 주입은 이번 fixture의 오류 검출 수단이다. 모든 실행에서 누수가 절대 발생하지 않는다는 증명이나 Windows LeakSanitizer 결과로 해석하지 않는다. TSan, 임의 스케줄 전체 탐색, 장시간 제품 workload, native OS API의 모든 실패 지점, Intel Mac·Linux·콘솔 실행은 별도 과제다.

worker에는 우선순위·work stealing·fiber가 없고, GC는 단일 mutator safepoint 기반이다. Reflection은 지원하는 공개 필드·단일 비가상 상속 subset이며, 일반 native 상속 cast나 method/enum/container reflection을 제공하지 않는다. ObjectGraph는 단일 root의 strong managed graph와 명시 migration callback까지이며 외부 에셋·weak 참조·IO·압축·무결성 인증은 후속이다. 자세한 수명 계약은 각 구현 문서를 따른다.
