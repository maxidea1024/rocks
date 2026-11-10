# Rocks 직렬화 기반층

`rocks_serialization`은 Core만 의존하는 bounded binary archive와 stable GUID TLV record를 제공한다. STL 컨테이너나 host 구조체 메모리 덤프를 사용하지 않는다. 실제 실행 결과는 통합 빌드 검증 보고서에 별도로 기록한다.

## 값과 수명

- `ArchiveWriter`는 전달 allocator로 byte buffer를 소유한다. `TakeBytes()`는 같은 allocator를 빌리는 `Array<uint8>`로 소유권을 이전한다. allocator가 결과보다 먼저 파괴되면 안 된다.
- `ArchiveReader`는 caller의 불변 byte 범위를 빌린다. `ReadStringView`/`ReadBytes`/`ReadRaw` 결과도 원본 수명에 종속된다. `ReadString(allocator)`만 문자열을 깊게 복사한다.
- 고정 폭 값은 little-endian이다. bool은 0/1, i64는 two's complement, float32/64는 IEEE bit 표현이다. Guid는 high u64 다음 low u64다. bytes/string은 u64 byte 길이와 payload이며 종단 NUL을 파일에 쓰지 않는다. UTF-8 string의 내부 U+0000은 보존한다.
- 모든 실패는 reader cursor 또는 writer의 기존 bytes/길이를 보존한다. 소비한 operation/work 예산은 환급하지 않으므로 같은 실패를 반복하여 한도를 우회할 수 없다. OOM 재시도에도 남은 예산이 필요하다.
- maxTotalBytes는 archive 전체, maxValueBytes는 가변 값/raw 범위, maxOperations/maxWork는 호출 및 처리 비용 한도다. work는 byte 검사·복사와 탐색 비교의 보수적인 비용이며 외부 allocator 실행 시간을 제한하는 타이머가 아니다.
- 동일 wrapper의 동시 변경과 allocator callback에서 해당 wrapper/input의 변경은 지원하지 않는다. writer 자기 View의 전체/부분 append는 성장해도 지원한다.

## Record version 1

헤더는 `ROCKSREC` 8 bytes, envelope version u32, schema Guid, schema version u32, field count u32, payload byte count u64의 44 bytes다. 필드마다 GUID 16 bytes, wire tag u32, payload 길이 u64, encoded payload를 쓴다.

`RecordBuilder`는 추가 순서를 보존한다. 같은 순서의 입력은 같은 bytes를 만들지만 GUID 정렬 canonicalization은 하지 않는다. `RecordReader::Open`은 전체 envelope와 모든 필드 경계·중복 ID를 검증한 뒤 index를 반환한다. reader는 index만 소유하므로 caller의 원본 buffer를 유지해야 한다.

unknown nonzero wire tag는 그대로 열거하고 skip할 수 있다. 알려진 tag도 record 계층이 payload 의미를 대신 검증하지 않는다. 소비자가 기대 tag를 확인하고 별도 ArchiveReader로 값을 읽은 다음 `AtEnd()`까지 확인한다. ID/tag 0은 InvalidArgument, 중복 ID는 AlreadyExists, envelope/schema version 미지원은 Unsupported, 기대 schema GUID 불일치는 TypeMismatch다. 누락 필드와 iteration 끝은 NotFound다.

이 계층은 checksum/인증/압축/파일 IO/객체 생성/참조 fixup을 제공하지 않는다. 범위 안의 payload bit 변경이 반드시 오류라는 뜻은 아니며, 데이터 무결성 검증이 필요한 상위 포맷은 별도 정책을 제공해야 한다.

## 검증 구성

독립 golden endian bytes, float bit 보존, UTF-8/NUL, compound cursor 불변, self-alias 성장, 크기·예산 경계, 모든 할당 지점 N-fail, 중복·truncation·version·unknown tag, 결정적 mutation fixture를 둔다. 테스트 소스 작성과 실제 Windows/macOS 실행 증거는 구분한다.
