# 요청: 테스트 장면 RidgeSunset 추가를 INTERFACES 10.1에 기록 (C 트랙, 2026-09-25)

## 이유
S가 먼 거리 공기 그림자(god ray)·산 그림자를 기준 경로추적기와 비교하는 검증을 요청했다(조율 세션 경유, 사용자 지시).

## 한 일 (C 소유 폴더 안)
- `Tools/SceneGen/include/unx/scenegen/SceneGen.h`의 `SceneId`에 `RidgeSunset = 6`을 **덧붙였다**(기존 번호 불변, 헤더 주석의
  "append, never renumber" 규칙). `sceneName` = `"ridge_sunset"`, `allScenes()`에 포함.
- 내용: 20 × 20 km 지면(20 m 격자), 카메라 앞 3 km에 폭 2 km·높이 800 m 능선, 태양 고도 17°·시선 축에서 10° 옆(능선 그림자 ≈ 2.6 km가
  카메라 쪽으로), 1·2·5·8 km의 300 m 탑 4개(능선 그림자 밖), 카메라 `ridge`(지면 2 m, 태양 쪽 8° 위), `side`, 경로 `ridge_static`,
  `side_static`, `drive`. 바람 없음.

## 원하는 변경 (코어 소유: `Docs/Design/INTERFACES_KO.md` 10.1)
10.1의 장면 목록에 `RidgeSunset(능선·탑 그림자의 공기 산란, 낮은 태양, 20 km)`을 추가.

## 영향
기존 장면의 내용·해시는 바뀌지 않는다(생성 순서·RNG 스트림 독립). 새 장면만 추가된다.
