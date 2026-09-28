# 로컬 검증 1db06e4 (RTX 4080, 2026-09-28)

renderergate 정지 600프레임, GpuLock, 경합 0. 비교 이미지는 위(왼쪽) main(35c7d96 빌드), 아래(오른쪽) cloud. 노출은 main 기준 하나.

## 시험
- hostmotion PASS (main에서도 실패하던 것이 고쳐짐), vsmtests PASS.
- reflectionanalytic furnace FAIL: G worst 13.18 % (main도 12.59 %로 FAIL).
- particletests는 이번에 돌리지 않음(FX 커널이 main과 같다고 함).

## 성능 [실측] (A/B/B/A 중앙값, ms)
| 장면 | main | cloud |
|---|---:|---:|
| 기차 1440p | 10.44 | 9.93 |
| 기차 4K 출력(내부 1440p) | 19.21 | 10.42 |
| 욕탕 1440p | 16.71 | 15.53 |
| 욕탕 4K 출력 | 30.91 | 16.06 |

기차 1440p 패스 main → cloud: r.refl.shade 1.76 → 2.05 (여전히 느림), r.gi.screen 0.77 → 1.09 (+ filter 0.39; main은 denoise.it 0.71, 합은 같음), r.gi.trace 0.46 → 0.50.

## 화면
- 욕탕 바닥 얼룩은 줄었지만 **아직 보인다** (`floor_bath_1080.png` 아래: 욕조 왼쪽 바닥, 욕조 앞 두 곳, 오른쪽 뒤). 벽에도 밝은 점 두 개(`cmp_bath_1080.png` 오른쪽 이미지: 왼쪽 벽 가운데, 오른쪽 벽 위쪽). main에는 없다.
- 기차 4K 출력 움직임 크롭: 잔상 없음.
