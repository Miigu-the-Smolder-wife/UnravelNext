# 로컬 검증 20379fb (RTX 4080, 2026-09-28)

renderergate 정지 600프레임(움직임 캡처는 --moving), GpuLock, 경합 0. 이미지: 선형 캡처 × main 기준 한 노출, ACES 근사. 모든 비교 이미지는 왼쪽(또는 위) main(35c7d96 빌드), 오른쪽(또는 아래) cloud.

## 시험
- vsmtests PASS.
- reflectionanalytic furnace FAIL: cloud G worst 12.99 %, `--set gi.anchor_visibility=false`에서도 12.90 %. **main도 FAIL(12.59 %)** — 브랜치 이전부터 있던 실패.
- hostmotion "static: 180° shutter" FAIL: 기본, UNX_GRAPH_PLAN_CACHE=0, shadow cache=false, anchor_visibility=false 모두 FAIL. **main도 FAIL** — 브랜치 이전부터.
- particletests --ticks 160: UNX_FX_FRAME_FENCE=1, UNX_GRAPH_PLAN_CACHE=0 둘 다 tick 137 event 499 같은 FAIL(1478 s, 1156 s). main은 돌리지 않음.

## 성능 [실측] (A/B/B/A 중앙값, ms)
| 장면 | main | cloud |
|---|---:|---:|
| 기차 1440p | 10.50 | 10.50 |
| 기차 4K 출력(내부 1440p) | 19.25 | 10.93 |
| 욕탕 1440p | 16.83 | 16.17 |
| 욕탕 4K 출력 | 31.06 | 16.62 |

기차 1440p 패스 main → cloud: r.refl.shade 1.76 → 2.06, r.gi.screen 0.77 → 1.44, r.gi.screen.filter 0 → 0.39, r.gi.denoise.it 0.71 → 0, r.gi.trace 0.47 → 0.66, s.vsm.raster 0.42 → 0.
욕탕 4K 출력 cloud 상위: m.lit.shade.b0 1.31, s.vsm.localraster0/1 1.28/1.17, s.vsm.localmark 1.18, s.froxel.integrate 1.10, s.shadow.overflow 1.05, s.shadow.visibility 0.88.

## 화면
- **욕탕 1080p 바닥에 main에 없던 밝은 푸른 얼룩(`floor_bath_1080.png` 아래쪽: 욕조 왼쪽 바닥, 욕조 오른쪽 뒤, 오른쪽 벽 밑).** 벽 모서리 쪽도 main보다 어둡다. 시각 회귀라 main 적용 보류.
- 기차: 흰 네모 사라짐. 4K 출력 움직임(업스케일) 크롭에서 잔상은 보이지 않음(`crop_train_4k_moving.png`).
