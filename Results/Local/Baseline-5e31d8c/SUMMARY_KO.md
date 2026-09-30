# 로컬 검증 결과

- 빌드한 커밋: `5e31d8c` (게이트 작업 폴더 `C:\Users\USER\UnravelNext-gate`)
- 모든 수치는 이 PC(RTX 4080)에서 잰 [실측] 값이다.

## 실행 기록 (종료 코드, D3D12 메시지 수)

| 실행 | 종료 코드 | d3d12 메시지 | 초 |
|---|---|---|---|
| ﻿caps\train_2560x1440_static_native | 0 | 0 | 19 |
| caps\train_2560x1440_static_up | 0 | 0 | 15 |
| caps\train_2560x1440_moving_native | 0 | 0 | 19 |
| caps\train_2560x1440_moving_up | 0 | 0 | 16 |
| caps\train_2560x1440_sun_native | 0 | 0 | 19 |
| caps\train_2560x1440_sun_up | 0 | 0 | 16 |
| caps\train_1920x1080_static_native | 0 | 0 | 15 |
| caps\train_1920x1080_static_up | 0 | 0 | 13 |
| caps\train_1920x1080_moving_native | 0 | 0 | 15 |
| caps\train_1920x1080_moving_up | 0 | 0 | 13 |
| caps\train_1920x1080_sun_native | 0 | 0 | 15 |
| caps\train_1920x1080_sun_up | 0 | 0 | 14 |
| caps\bath_2560x1440_static_native | 0 | 0 | 16 |
| caps\bath_2560x1440_static_up | 0 | 0 | 12 |
| caps\bath_1920x1080_static_native | 0 | 0 | 12 |
| caps\bath_1920x1080_static_up | 0 | 0 | 10 |
| conv\train_2560x1440_1 | 0 | 0 | 9 |
| conv\train_2560x1440_4 | 0 | 0 | 9 |
| conv\train_2560x1440_16 | 0 | 0 | 9 |
| conv\train_2560x1440_64 | 0 | 0 | 9 |
| conv\train_2560x1440_256 | 0 | 0 | 11 |
| conv\train_2560x1440_ref | 0 | 0 | 13 |
| conv\train_1920x1080_1 | 0 | 0 | 9 |
| conv\train_1920x1080_4 | 0 | 0 | 9 |
| conv\train_1920x1080_16 | 0 | 0 | 9 |
| conv\train_1920x1080_64 | 0 | 0 | 9 |
| conv\train_1920x1080_256 | 0 | 0 | 10 |
| conv\train_1920x1080_ref | 0 | 0 | 12 |
| conv\bath_2560x1440_1 | 0 | 0 | 4 |
| conv\bath_2560x1440_4 | 0 | 0 | 4 |
| conv\bath_2560x1440_16 | 0 | 0 | 5 |
| conv\bath_2560x1440_64 | 0 | 0 | 5 |
| conv\bath_2560x1440_256 | 0 | 0 | 7 |
| conv\bath_2560x1440_ref | 0 | 0 | 9 |
| conv\bath_1920x1080_1 | 0 | 0 | 5 |
| conv\bath_1920x1080_4 | 0 | 0 | 4 |
| conv\bath_1920x1080_16 | 0 | 0 | 4 |
| conv\bath_1920x1080_64 | 0 | 0 | 5 |
| conv\bath_1920x1080_256 | 0 | 0 | 6 |
| conv\bath_1920x1080_ref | 0 | 0 | 8 |

종료 코드가 0이 아니거나 d3d12 메시지가 있는 실행: 없음

## 업스케일 (원래 해상도 대 업스케일, 같은 출력 해상도)

비교 PNG: `caps/cmp_*.png`(전체), `caps/crop_*.png`(가운데 1:1). 왼쪽이 원래 해상도, 오른쪽이 업스케일이다. **숫자보다 눈으로 보는 판정이 먼저다.**

- bath_1920x1080_static: hf_ratio 0.645, ssim 1.000, edge_ratio 0.959, mean_diff 0.000 → 후보 기준 **불합격**
- bath_2560x1440_static: hf_ratio 0.787, ssim 1.000, edge_ratio 0.962, mean_diff 0.000 → 후보 기준 **불합격**
- train_1920x1080_moving: hf_ratio 1.212, ssim 0.983, edge_ratio 1.017, mean_diff 0.003 → 후보 기준 **불합격**
- train_1920x1080_static: hf_ratio 1.484, ssim 0.979, edge_ratio 1.033, mean_diff 0.008 → 후보 기준 **불합격**
- train_1920x1080_sun: hf_ratio 1.263, ssim 0.985, edge_ratio 1.009, mean_diff 0.005 → 후보 기준 **불합격**
- train_2560x1440_moving: hf_ratio 1.075, ssim 0.985, edge_ratio 0.996, mean_diff 0.004 → 후보 기준 **불합격**
- train_2560x1440_static: hf_ratio 0.945, ssim 0.985, edge_ratio 0.986, mean_diff -0.005 → 후보 기준 **불합격**
- train_2560x1440_sun: hf_ratio 1.038, ssim 0.988, edge_ratio 0.982, mean_diff 0.002 → 후보 기준 **불합격**

## 수렴 곡선 (장면 첫 프레임부터, 업스케일 출력) — 움직임 화질의 대리 지표

장면을 연 직후는 가장 심한 가림 해제다(조명 편집 뒤 재빌드도 지금은 같다). 사용자 요구는 몇 프레임 안에 깨끗해지는 것이다.

- bath_1920x1080: 1프레임: 평균 58.8 %, 타일 P95 76.2 %; 4프레임: 평균 40.3 %, 타일 P95 54.5 %; 16프레임: 평균 41.5 %, 타일 P95 54.6 %; 64프레임: 평균 23.8 %, 타일 P95 33.3 %; 256프레임: 평균 13.1 %, 타일 P95 16.9 % → 타일 P95 ≤ 3 %에 닿는 프레임: 측정 범위 밖(더 늦음) (`conv/strip_bath_1920x1080.png`: 왼쪽부터 1, 4, 16, 64, 256프레임, 마지막이 수렴)
- bath_2560x1440: 1프레임: 평균 60.2 %, 타일 P95 79.7 %; 4프레임: 평균 44.0 %, 타일 P95 58.9 %; 16프레임: 평균 35.8 %, 타일 P95 52.1 %; 64프레임: 평균 22.1 %, 타일 P95 30.9 %; 256프레임: 평균 10.0 %, 타일 P95 14.1 % → 타일 P95 ≤ 3 %에 닿는 프레임: 측정 범위 밖(더 늦음) (`conv/strip_bath_2560x1440.png`: 왼쪽부터 1, 4, 16, 64, 256프레임, 마지막이 수렴)
- train_1920x1080: 1프레임: 평균 20.5 %, 타일 P95 65.7 %; 4프레임: 평균 15.1 %, 타일 P95 51.1 %; 16프레임: 평균 9.5 %, 타일 P95 52.7 %; 64프레임: 평균 4.1 %, 타일 P95 20.8 %; 256프레임: 평균 2.6 %, 타일 P95 17.2 % → 타일 P95 ≤ 3 %에 닿는 프레임: 측정 범위 밖(더 늦음) (`conv/strip_train_1920x1080.png`: 왼쪽부터 1, 4, 16, 64, 256프레임, 마지막이 수렴)
- train_2560x1440: 1프레임: 평균 23.2 %, 타일 P95 69.5 %; 4프레임: 평균 15.5 %, 타일 P95 54.6 %; 16프레임: 평균 11.5 %, 타일 P95 44.3 %; 64프레임: 평균 5.4 %, 타일 P95 29.8 %; 256프레임: 평균 3.8 %, 타일 P95 18.2 % → 타일 P95 ≤ 3 %에 닿는 프레임: 측정 범위 밖(더 늦음) (`conv/strip_train_2560x1440.png`: 왼쪽부터 1, 4, 16, 64, 256프레임, 마지막이 수렴)

## 결정론 (같은 설정 두 번)


`det` = `debug.deterministic=true`, `def` = 기본 모드. 기본 모드의 이전 값은 평균 밝기 차 4.41 %(84e789f, 욕탕 1080p).

## 밝기 흔들림 (--luminance-log)


수백 프레임 주기로 수 % 흔들리면 클라우드의 모델(GI 캐시의 긴 Jacobi 단계)과 맞는다.

## 성능 [실측] (비동기 기본 꺼짐, 정지 카메라 600프레임, GpuLock timing)

| 장면 | 출력 | GPU 프레임 중앙값 (회차별) | p95 | 경합 초 | m.upscale |
|---|---|---|---|---|---|

이전 값(84e789f, 직렬): 기차 1440p 5.33, 기차 1080p 3.79, 욕탕 1440p 6.10, 욕탕 1080p 4.40 ms.

상위 패스 (ms, 회차 중앙값):

