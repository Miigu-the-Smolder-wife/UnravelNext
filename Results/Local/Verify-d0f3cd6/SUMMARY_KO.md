# 로컬 검증 결과

- 빌드한 커밋: `?` (게이트 작업 폴더 `C:\Users\USER\UnravelNext-gate`)
- 모든 수치는 이 PC(RTX 4080)에서 잰 [실측] 값이다.

## 실행 기록 (종료 코드, D3D12 메시지 수)

| 실행 | 종료 코드 | d3d12 메시지 | 초 |
|---|---|---|---|
| ﻿caps\train_2560x1440_static_native | 0 | 0 | 21 |
| caps\train_2560x1440_static_up | 0 | 0 | 19 |
| caps\train_2560x1440_moving_native | 0 | 0 | 24 |
| caps\train_2560x1440_moving_up | 0 | 0 | 25 |
| caps\train_2560x1440_sun_native | 0 | 0 | 33 |
| caps\train_2560x1440_sun_up | 0 | 0 | 25 |
| caps\train_1920x1080_static_native | 0 | 0 | 26 |
| caps\train_1920x1080_static_up | 0 | 0 | 21 |
| caps\train_1920x1080_moving_native | 0 | 0 | 26 |
| caps\train_1920x1080_moving_up | 0 | 0 | 21 |
| caps\train_1920x1080_sun_native | 0 | 0 | 26 |
| caps\train_1920x1080_sun_up | 0 | 0 | 21 |
| caps\bath_2560x1440_static_native | 0 | 0 | 29 |
| caps\bath_2560x1440_static_up | 0 | 0 | 19 |
| caps\bath_1920x1080_static_native | 0 | 0 | 21 |
| caps\bath_1920x1080_static_up | 0 | 0 | 16 |

종료 코드가 0이 아니거나 d3d12 메시지가 있는 실행: 없음

## 업스케일 (원래 해상도 대 업스케일, 같은 출력 해상도)

비교 PNG: `caps/cmp_*.png`(전체), `caps/crop_*.png`(가운데 1:1). 왼쪽이 원래 해상도, 오른쪽이 업스케일이다. **숫자보다 눈으로 보는 판정이 먼저다.**

- bath_1920x1080_static: hf_ratio 6.351, ssim 0.998, edge_ratio 1.399, mean_diff -0.001 → 후보 기준 **불합격**
- bath_2560x1440_static: hf_ratio 3.560, ssim 0.998, edge_ratio 1.325, mean_diff 0.000 → 후보 기준 **불합격**
- train_1920x1080_moving: hf_ratio 0.692, ssim 0.982, edge_ratio 0.919, mean_diff -0.001 → 후보 기준 **불합격**
- train_1920x1080_static: hf_ratio 0.704, ssim 0.982, edge_ratio 0.915, mean_diff -0.004 → 후보 기준 **불합격**
- train_1920x1080_sun: hf_ratio 0.592, ssim 0.983, edge_ratio 0.920, mean_diff -0.002 → 후보 기준 **불합격**
- train_2560x1440_moving: hf_ratio 0.598, ssim 0.983, edge_ratio 0.916, mean_diff 0.006 → 후보 기준 **불합격**
- train_2560x1440_static: hf_ratio 0.703, ssim 0.985, edge_ratio 0.901, mean_diff 0.001 → 후보 기준 **불합격**
- train_2560x1440_sun: hf_ratio 0.943, ssim 0.979, edge_ratio 0.941, mean_diff 0.010 → 후보 기준 **불합격**

## 결정론 (같은 설정 두 번)


`det` = `debug.deterministic=true`, `def` = 기본 모드. 기본 모드의 이전 값은 평균 밝기 차 4.41 %(84e789f, 욕탕 1080p).

## 밝기 흔들림 (--luminance-log)


수백 프레임 주기로 수 % 흔들리면 클라우드의 모델(GI 캐시의 긴 Jacobi 단계)과 맞는다.

## 성능 [실측] (비동기 기본 꺼짐, 정지 카메라 600프레임, GpuLock timing)

| 장면 | 출력 | GPU 프레임 중앙값 (회차별) | p95 | 경합 초 | m.upscale |
|---|---|---|---|---|---|

이전 값(84e789f, 직렬): 기차 1440p 5.33, 기차 1080p 3.79, 욕탕 1440p 6.10, 욕탕 1080p 4.40 ms.

상위 패스 (ms, 회차 중앙값):

