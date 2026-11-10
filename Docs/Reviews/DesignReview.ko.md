# Rocks 설계 교차 검증 결과

**검토일 2026.11.10 · 전체안 1.3 / Core·Object·Input 상세안 1.1**

서브에이전트 3개가 통합 설계와 작업 데이터를 **4차례 검증**하였다. 첫 통합 검토의 16개 사항, 담당 영역을 교환한 2차 검토의 6개 사항, 역방향 반례를 대입한 3차 검토의 2개 사항을 합쳐 **24개 계약·추적 보완 사항**을 반영하였다. 4차에서 세 검토자가 해당 범위의 수정 완료를 확인하였다. 이는 설계 문서의 반례 검토이며 엔진 구현·실행·성능 시험 결과가 아니다.

## 1. 검토 범위

| 담당 | 주요 대조 범위 |
|---|---|
| Core·Object | 조회 context·생성/공개·GC/수명·종료·Foundation 순서·상세안 정합성 |
| 렌더·콘텐츠·수학 | 5개 API·두 렌더 경로·재질/베이크·streaming·device 수명·PIE 격리·SIMD |
| 빌드·헤드리스·플랫폼 | Win/Mac·Linux 단계·local/cross toolchain·템플릿 ID·시험 단계·입력 외부 조건 |

검토자는 원문 위치·구체 반례·최소 수정·구현 시 수용 기준을 기록하였다. 종합 담당자가 수정한 뒤 해당 검토자가 지적 사항의 해결 여부를 다시 확인하였다. 이전 개정 1.2의 검토 이력은 전체안 33.4절에 보존하고 이번 보완과 구분한다.

## 2. 반영한 보완 사항

| ID·검토 근거 | 발견한 경계 | 반영한 계약·검증 위치 |
|---|---|---|
| RV01 · FC01 | master Resolve 예제의 context·오류 반환 누락 | 명시 ObjectAccessContext와 Result&lt;Borrow&lt;T&gt;&gt;; 전체 5.1·상세 10.2 |
| RV02 · FC02 | 공개 후 Register 실패를 미공개 rollback으로 처리할 가능성 | pre-commit Initialize/Commit을 분리하고 이후 실패는 RequestDestroy·정상 retire; 전체 5.3 |
| RV03 · FC03 | scheduler 종료 후 최종 GC cleanup을 실행할 수 없음 | root 반환→최종 GC→추가 cleanup/backend drain→closure→scheduler/type/writer 종료; 전체 3.2 |
| RV04 · FC04/FB01 | 동일 CF01–CF12가 서로 다른 검증 의미 | 전체 1.2를 정본으로 상세 16.1의 CT 매핑을 통일 |
| RV05 · FC05 | 전체 allocator/IO를 객체 최소 구현의 선행 조건으로 해석 | F 표를 책임 묶음으로 정의, 실제 R0→R1→R2; slab/thread cache는 C28 선택 |
| RV06 · FR01 | CPU baker가 GPU 재질을 평가하는 경로 미정의 | BakeMaterialProfile·CPU 부분집합·texture/time 고정·proxy/미지원 오류; 14.10·LT01/LT06 |
| RV07 · FR02 | reduced-mip 교체가 논리 크기·LOD 의미를 변경 | logical extent/mip·resident base와 descriptor의 동일 generation 게시, explicit/implicit/Load 규약; 9.5·TX/API |
| RV08 · FR03 | 새 GPU device가 옛 handle/fence를 재사용할 위험 | DeviceId/DeviceEpoch domain과 구 장치 recovery 격리; 11.2·API04/BR04 |
| RV09 · FR04 | EditorWorld와 PIE의 가변 RenderScene 혼용 | session/World별 논리 scene·cache/history 격리, 동일 session view만 공유; 11.1/11.4·PI03/PI05 |
| RV10 · FR05 | 예산 포화 시 축소 allocation도 실패하여 진행 불가 | 전환 reserve 또는 typed builtin 전환→old fence drain→해제; 9.5·TX03 |
| RV11 · FR06 | 5개 backend와 오래된 디렉터리·DXR 표현 불일치 | D3D11/D3D12/Vulkan/Metal/OpenGL 트리·hardware RT caps·기본 vertex/pixel fallback 명료화 |
| RV12 · FB02 | Linux가 문서별로 Foundation 필수/선택으로 분리 | 첫 Foundation Win/Mac; Linux CPU M3·서버 M5 필수 후속 |
| RV13 · FB03 | cross-target도 외부 worker 없이 빌드 가능하다는 해석 | native host 지원 target만 local 보장, compiler host 부재는 capability 실패; 27절 |
| RV14 · FB04 | M4에 M5/M6 scenario·Linked template 시험을 요구 | LT/TT case별 required/deferred와 owner gate; 전체 suite의 허위 PASS 방지 |
| RV15 · FB05 | 코드 템플릿 복제가 TypeId까지 복사 | 새 선언 TypeId 최초 plan에서 1회 발급, 재시도/rename 유지·기존 타입 참조 보존; 22.4 |
| RV16 · FB06 | I13의 실제 Studio/PIE 시험 외부 조건 누락 | standalone/실제 IT19 분리, 양OS Studio/PIE 조건 누적·I15/I16 상속; 상세 24절·CSV |

상태는 **문서 계약 반영·구현 검증 대기**다. 각 항목의 실제 회귀는 연결된 CF/CT/IT/API/TX/BR/LT/PI/TT 사례를 구현하는 단계에서 수행한다. 문구 수정만으로 메모리 안전·영상 정합·플랫폼 실행을 승인하지 않는다.

## 3. 확정한 코딩 원칙

**사람이 알아보기 좋은 코드 스타일 유지.** 탭 문자 없이 스페이스 4칸을 사용하고, 함수·타입 정의와 의미가 다른 처리 단계 사이에 적절한 빈 줄을 둔다. 읽기 쉬운 이름·짧고 명확한 함수·불필요한 한 줄 압축 금지를 리뷰 기준으로 적용한다.

함께 제공하는 `.editorconfig`와 `.clang-format`은 문서 C++ 예제에 실제 적용하였다. 사용 도구는 clang-format 21.1.6이며 37개 예제의 재포맷 동일성·탭 부재와 잘못된 서식 fixture의 실패를 확인하였다. 이는 서식 검증이며 API의 컴파일 시험은 아니다. 실제 저장소에서는 formatter 버전·설정을 고정하고 C00/CT01에서 로컬·CI 검사를 연결한다.

DirectXMath의 품질·성능을 낮게 평가하여 배제한 것이 아니다. Rocks의 공개 API를 `rocks::Vec3`, `Quat`, `Mat4` 등 자체 스타일로 유지하고 MS식 vendor 표기의 노출을 피한다. 수학 backend는 아직 미선정이며 검증된 구현을 adapter로 사용한다. RTM·DirectXMath 등의 후보는 정확성·SIMD·f64·플랫폼·유지보수·컴파일 비용으로 판단한다. 해당 후보의 Rocks 성능 벤치마크는 실행하지 않았다.

## 4. 구현 착수와 남은 검증

첫 구현은 Core 기반 라이브러리와 Object 시스템이다. 작업 목록은 **52개 작업과 297개 예정 파일**이며 실제 엔진 소스 생성 목록이 아니다. 각 작업에 선행 관계·수용 시험·장애 주입·동시성 검사·완료 증거·플랫폼 조건을 연결하였다. 입력은 Windows/macOS 키보드·마우스·패드·IME·focus·PIE·replay를 구체화하고 콘솔 provider는 SDK/장비 확보 후 실제 시험을 요구한다.

최종 배포 검사는 장/표·목차·PDF 텍스트·HTML 배치·작업 DAG·중복 경로·JSON/CSV 일치와 파일 해시를 확인한다. 검사 결과는 배포 JSON에 기록한다. 엔진의 unit/stress/sanitizer·실제 GPU/음향·네트워크·메모리 누수·성능 시험은 구현 후 수행할 항목으로 남아 있다. 기반의 정확성·안정성, 확장성, 성능 순서와 Blueprint 제외·커스텀 언어 정책을 유지한다.

## 5. 담당 교환 후 2차 검증

Core 담당은 렌더·에셋·PIE의 수명을, 렌더 담당은 Core·Object·Input의 소비 경계를, 빌드 담당은 첫 구현 순서·파일 목록·캐시 정확성을 검토하였다. 기존 지적의 해결 여부만 확인하는 절차와 구분하여 새 실행 순서 반례를 구성하였다.

| ID·근거 | 확인한 문제 | 반영한 계약·검증 |
|---|---|---|
| RV17 · R2C01 | 요청 ticket 반환만으로 유효 AssetLease의 unload 시작 가능 | 전체 9.2: request/residency 계수·원자 admission·scope revoke와 tombstone; NL03/NL04·PI05 |
| RV18 · R2-RC01 | 완료 TaskHandle 보관이 capture root를 계속 유지 | 전체 8.1·상세 8.1: 실행 payload cleanup 뒤 terminal 게시, 결과 record와 분리; CT08·C17/C24 |
| RV19 · R2-RC02 | ModuleLease만으로 scratch metadata 수명 보장 불가 | 전체 7.2·상세 9.1: 중첩 schema 소유 복사·원자 등록·revision lease; CT10·C07/C24 |
| RV20 · R2-RC03 | 동일 user/tick의 UI·PIE A/B 조회 혼합 | 전체 22.6·상세 17–18: InputReadKey의 scope/domain/clock epoch/tick; IT03/IT19·I04/I15 |
| RV21 · R2B01 | R0 mutex 구현보다 public Sync.h 도입이 늦음 | 파일 목록의 최초 소유를 C02로 이동하고 C16을 확장으로 정의; CT02/CT07 |
| RV22 · R2B02 | generated code 서식 변경이 캐시 입력에서 누락 가능 | 전체 27.2·상세 9.3: formatter binary/config digest·단일 producer·동일 출력 보존; CT12·C20 |

수정으로 작업 수를 증가시키지 않았다. 기존 52개 작업·297개 예정 파일에 계약과 시험 조건을 연결하였다. 정상 기능을 추가하는 것보다 잘못된 수명·증분 결과·세션 혼합을 구현 전에 차단하는 데 우선순위를 두었다.

## 6. 3·4차 검증과 착수 판정

3차 검증에서는 수정된 계약에 실행 중 취소와 cleanup 종료, 등록 복사 실패와 old reader, 같은 tick의 여러 PIE와 UI, 장기 보관된 revoked token과 공유 revision 사용, 최초 Core build 및 formatter 변경의 반례를 대입하였다. 이 과정에서 종료 진척과 callback 재진입에 관한 2개 추가 경계를 확인하였다.

| ID·근거 | 추가 반례 | 반영한 계약·검증 |
|---|---|---|
| RV23 · R3-RC01 | queue/node 포화로 cleanup 제출 실패·terminal 정지 | 전체 8.1·상세 8.1: 승인 전 slot/link 확보, 기존 node의 추가 할당 없는 cleanup, gate 밖 destructor; CT08·C17 |
| RV24 · R3C01 | callback의 같은 package 재요청·자기 Stop 중 gate 재진입 | 전체 9.2: explicit acceptance, 전달 slot 예약, gate 밖 callback/destructor, 자기 Stop 비동기, revoke와 delivery admission 분리; NL03/NL04·PI05·C24 fake |

4차는 두 추가 수정과 기존 지적의 직접 영향 범위를 다시 검증하였다. 세 검토자의 문서 판정은 모두 PASS이며, **이번 검토 범위에 미해결로 남은 지적은 없다.** Task pool 포화·실행 중 취소·완료 observer 보관·RootRef/ModuleLease 정리, callback 재진입·등록 실패·scope revoke, R1 선행 관계와 JSON/CSV 일치를 확인하였다.

| 회차 | 방법 | 결과 |
|---|---|---|
| 1차 | 세 전문 영역의 전체안·상세안·작업 추적 대조와 수정 확인 | 16개 보완 반영 |
| 2차 | 담당 영역 교환, 실제 실행 순서의 복합 반례 구성 | 6개 추가 보완 반영 |
| 3차 | 수정 경로의 역방향 반례와 기존 경로 회귀 | 2개 추가 보완 반영 |
| 4차 | 추가 수정의 종료·재진입·의존성 회귀 확인 | 세 담당 문서 PASS, 검토 범위 미해결 0건 |

설계 검토 종료 기준은 발견한 P0/P1의 미해결 항목이 없고 각 수정에 구현 시 실패를 검출할 구체적인 oracle이 연결되는 것이다. 구현 착수 이후에는 같은 계약을 유닛·장애 주입·동시성·실제 장치 하네스로 재검증한다. 검토 횟수 자체를 안전성 증명으로 사용하지 않으며, SDK·backend·수치 오차·성능 예산의 미확정 사항은 해당 단계의 검증 항목으로 유지한다.
