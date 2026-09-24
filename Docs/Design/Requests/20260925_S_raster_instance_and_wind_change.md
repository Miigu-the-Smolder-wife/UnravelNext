# 요청: 깊이 래스터 픽셀 입력에 인스턴스 번호, 바람 변위 변화 상한 함수 (S 트랙, 2026-09-25)

## 왜 필요한가 (실측)

설계서 2.3 dirty 규칙 (b) "바람 변형이 그 페이지 텍셀보다 크게 움직인 것"을 인스턴스 쪽에서 판정하면(인스턴스마다 12단의 경계
사각형 안 페이지를 훑음) 스레드 하나가 근거리 수목 하나에 수천 페이지를 직렬로 본다. city_block(인스턴스 353개) 정지 카메라에서
`s.vsm.invalidate` = **4K 0.43 ms, P95 0.75 ms / 1440p 0.38 ms** [실측, `Results/S/ShadowGate/`]. 설계 비용식의 페이지 관리 항
(0.04 ms)의 10배이고, forest_thin(수목 100k + 풀 1M, 전부 바람)에서는 더 커진다. **구현 구조 문제**다: 판정의 정보량은 "상주 페이지
수(≤ 3072)"이지 "인스턴스 × 페이지"가 아니다.

페이지 쪽 판정: 페이지를 래스터할 때 그 페이지에 그려진 캐스터들의 바람 진폭 상한 A_max를 페이지 메타데이터에 기록하고, 매 프레임
상주 페이지마다 `A_max × (렌더 시각 이후 변위 변화 인자) > windTexels × τ_k`면 stale로 표시한다. 페이지당 O(1)이다. 이를 위해 두 가지가
필요하다.

## 원하는 변경

1. **`DepthRasterPixel`에 인스턴스 번호** (`Passes/Visibility/DepthRaster.hlsli`, INTERFACES 5.3):
   ```hlsl
   nointerpolation uint instance : INSTANCE;   // 이 삼각형의 장면 인스턴스 번호 (loadInstance로 읽음)
   ```
   S의 픽셀 커널(`Passes/Shadow/VsmPagePixel`)이 `loadInstance(p.instance)`로 바람 진폭 상한(`windOffsetBound`)을 구해 페이지
   메타데이터에 `InterlockedMax`한다. V의 메시 셰이더가 이미 인스턴스를 알고 있으므로 출력 하나 추가다.

2. **바람 변위 변화 인자** (`Passes/Common/Deformation.hlsli`, 6.4):
   ```hlsl
   // 모든 인스턴스·정점에 대해 |windOffset(t1) - windOffset(t0)| <= windOffsetBound(...) * windChangeFactor(t0, t1).
   float windChangeFactor(float t0, float t1);
   ```
   바람 모델(P3에서 교체)을 아는 곳은 코어다. 모델이 바뀌어도 S의 규칙은 이 두 함수만 부르면 맞다. v1 모델
   (진폭 × (0.6 + 0.4 sin(1.7 t + φ)))이면 `0.4 * min(2, 1.7 * abs(t1 - t0))`이다(위상과 무관한 상한).

## 그때까지의 처리

- S는 위 이름 그대로 구현한다. `VsmPagePixel`은 자기 입력 구조체에 `instance : INSTANCE`를 더해 선언하고, S의 테스트용 래스터가 그
  출력을 낸다. V의 서비스가 이 출력을 내기 전까지 V 경로에서는 PSO가 만들어지지 않는다(연결 시점에 필요).
- `windChangeFactor`는 S 폴더에 v1 식 사본(`VsmCommon.hlsli`의 `vsmWindChangeFactor`)을 두고, 코어 함수가 생기면 그것으로 바꾼다.

## 영향

- 코어/V: 픽셀 입력 필드 1개, Deformation.hlsli 함수 1개. 다른 트랙에는 영향이 없다(필드 추가).
- 이동·스킨 캐스터(규칙 a)는 인스턴스 쪽 판정을 유지하되, 그 프레임에 바뀐 인스턴스만 모아(압축 목록) 인스턴스 × 단마다 스레드
  그룹 하나로 경계 사각형을 병렬로 훑는다(S 내부 변경).

## 처리 결과 (코어/V, 2026-09-25, INTERFACES v1.4)

- 반영: `DepthRasterPixel`에 `nointerpolation uint instance : INSTANCE`를 추가했고(`DepthRaster.hlsli`), V 서비스의 메시 셰이더
  (`Passes/Visibility/DepthRaster.ms`)가 삼각형의 장면 인스턴스 번호를 이 이름으로 낸다.
- 반영: `float windChangeFactor(float t0, float t1)`를 `Passes/Common/Deformation.hlsli`에 두었다. v1 식은 요청대로 `0.4 * min(2, 1.7 |t1 − t0|)`이고,
  조건(장면 바람의 방향·속도와 인스턴스 변환이 그대로일 때)을 주석과 INTERFACES 6.4에 적었다. 바람 모델이 바뀌면 `windOffset`,
  `windOffsetBound`, `windChangeFactor`를 코어가 같이 바꾼다. S의 사본(`vsmWindChangeFactor`)은 이 함수로 바꾸면 된다.
