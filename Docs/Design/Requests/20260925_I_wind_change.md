# 요청: 커밋 뒤 장면 바람 변경의 계약 (I 트랙, 2026-09-25)

## 왜 필요한가

엔진 범위에 날씨가 있다(시간대·날씨, REBUILD_PLAN 13절). 호스트는 프레임마다 태양·대기·바람을 바꿀 수 있어야 한다. 태양과 대기는
이제 호스트 ABI로 프레임마다 바꾼다(`UnxFrameSetSun`, `UnxFrameSetEnvironment`, ABI 4). 렌더러 쪽은 이미 준비돼 있다:
프레임 상수가 태양을 `GpuScene::source()`에서 매 프레임 읽고, 대기 트랙은 파라미터·태양이 바뀌면 LUT를 다시 만든다.

바람은 그렇지 않다. INTERFACES 6.4의 `windChangeFactor(t0, t1)`는 "장면 바람의 방향·속도와 인스턴스 변환이 그대로일 때"만 상한이다.
S의 페이지 dirty 판정(페이지에 기록한 진폭 상한 × 변화 인자)과 R의 바람 BLAS refit 판단이 이 상한에 기댄다. 커밋 뒤 바람을 바꾸면
두 트랙이 모르는 사이 상한이 깨져 그림자 페이지와 반사 기하가 조용히 낡는다. 그래서 I는 지금 커밋 뒤 바람 변경을 거부한다
(`UnxFrameSetEnvironment`가 오류를 낸다).

## 원하는 변경

1. **바람 변경의 신호**: 프레임 상수(또는 GpuScene)에 장면 바람의 revision(`windRevision`, 바뀐 프레임마다 +1). 호스트 갱신은
   `GpuScene`이나 `scene::Scene`의 바람을 바꾸는 코어 함수(예: `GpuScene::setWind(frameIndex, direction, speed)`)로 한다.
   지금 I는 `scene::Scene`(GpuScene의 source)을 제출 스레드에서 프레임 기록 전에 고쳐 태양·대기를 바꾼다. 바람도 같은 방식이면 되지만
   소비자가 변화를 알 신호가 없다.
2. **바람이 바뀌는 구간의 변화 상한**: 게임의 날씨 바람은 몇 초에 걸쳐 서서히 바뀐다(돌풍 증감, 방향 회전). 바뀔 때마다 모든 바람
   페이지·BLAS를 버리면 전환 내내 전부 다시 그린다. 모델을 아는 코어가 바람 상태까지 포함한 상한을 정해 달라.
   `windChangeFactor(t0, t1, wind0, wind1)`: 두 시각과 두 바람(방향, 속도)에 대해
   |windOffset(t1; wind1) − windOffset(t0; wind0)| ≤ windOffsetBound(inst; max(speed0, speed1)) × factor.
   v1 모델(진폭 ∝ speed², 방향 d)이면 인자는 시간 항(지금 식)에 속도 항 |speed1² − speed0²| / max(speed0, speed1)²와
   방향 항(두 방향 사이 각의 2 sin(Δθ/2))을 더한 꼴이 될 것이다[예상, 식은 코어가 확정].
3. **INTERFACES 6.4 문구**: 상한의 전제에서 "장면 바람이 그대로일 때"를 "위 인자를 쓸 때"로 바꾼다.

## 그때까지의 처리

- 호스트 ABI는 커밋 뒤 바람 변경을 거부한다. 오류 문구가 이 요청 파일을 가리킨다. 태양과 대기는 프레임마다 바꿀 수 있다.
- 데이터 월드는 바람이 고정이라 지금 영향이 없다.

## 영향

- 코어: 바람 revision과 설정 함수, `Deformation.hlsli`의 변화 인자 확장(모델과 같이 바뀌는 세 함수 중 하나).
- S: 페이지 dirty 판정이 새 인자를 쓴다. R: 바람 BLAS refit 판단이 revision/인자를 쓴다. V: 없음(매 프레임 변형).
- I: `UnxFrameSetEnvironment`가 바람도 받는다.

## 결과 (코어, 2026-09-25, INTERFACES v1.23)

- 호스트 경로: 프레임 기록 전에 `GpuScene::source()` 장면의 `windDirection`·`windSpeed`를 바꾼다(태양과 같다). 프레임 상수가 그 프레임의 바람을 싣는다.
- 바람 revision은 두지 않았다. S(bbf2575)와 R 모두 끝점 비교로 충분하다고 했다.
- 변화 상한(S 검토의 더 좁은 식)과 무기억 계약:
  - `windOffsetScale(inst, centre, radius)`: 속도 무관 부분.
  - `windChangeBound(scale, t0, s0, d0, t1, s1, d1)`
    = scale × [s1²·0.4·min(2, 1.7|Δt|) + |s1² − s0²| + 2 sin(Δθ/2)·s0²].
  - 무기억 계약: 사이의 변화와 무관하게 끝점만으로 성립한다.
- INTERFACES 6.4를 고쳤다. `windChangeFactor`는 "바람이 그대로일 때"의 인자로 남는다.
- 검증: HLSL 함수 추가다. 셰이더 컴파일이 통과했다(core 빌드, 실행 없음). 상한의 수치 검사는 S의 페이지 판정 테스트가 실제 바람 전환에서 한다.
