# 로컬 검증 d0f3cd6 (세션 11) — 정확성·화면만, 성능은 재지 않음 (RTX 4080, 2026-09-29)

## 시험 [실측]
| 시험 | 결과 |
|---|---|
| reflectionanalytic | **FAIL**: furnace G worst 16.02 % (84e789f에서는 통과) |
| visibilitytests | **FAIL**: `vis_id_decodes_to_the_covering_triangle` — 화소 (1659, 198)의 vis id가 가리키는 삼각형이 화소에서 478 px 떨어져 있다(틀린 삼각형 = 틀린 셰이딩). `depth_ties_choose_stable_primitive`는 PASS. d3d12 WARNING 926(WriteBufferImmediate: 5 resources contain the GPU VA range) 14건 |
| gianalytic, hostmotion, localshadowtests, froxeltests, vsmtests, froxeltests `--queue-ab --set debug.deterministic=true` | PASS, d3d12 메시지 0 |

## 결정론 (`debug.deterministic=true`, 1080p, `--capture-output`) — 아직 비트 동일이 아니다
| 비교 | 욕탕 | 기차 |
|---|---|---|
| 같은 커밋 두 번, 워밍업 300 | 다름: 화소 309,161개, 평균 |차| 0.033 | 다름: 평균 밝기 1.06 %, 거의 모든 화소 |
| 같은 커밋 두 번, 첫 프레임(워밍업 0, 1프레임) | 다름: 화소 89개 | 다름: 화소 5개 |
| a30bc03(최적화 직전) 대 d0f3cd6 | 다름: 평균 0.30 % | 다름: 평균 0.064 % |
| batch_gi_corners=true / overflow_filter_queue=false / integration_queue=false 대 기본 | 모두 다름(같은 커밋 반복과 같은 크기) | 두 결과(해시 1daf68dc…, a1170ffd…) 중 하나가 나온다: gicorners는 기본 1회차와 비트 동일, 두 큐 끔은 기본 2회차와 비트 동일 |

- 기차는 실행마다 **두 가지 결과 중 하나**가 나온다(경합으로 갈리는 이분 상태). 첫 프레임부터 5~89 화소가 다르다.
- 같은 커밋 반복이 같지 않으므로 3a~4d의 "결과가 같다"는 비트로 확인할 수 없다. 반복 차이와 같은 크기라 반증도 아니다.

## GI 이력 분리 (`gi.split_bounce_history`) — 밝기 곡선
- 욕탕 1080p: 로그 값이 0.99987로 포화(L/(1+L), 이 장면 노출에서 밝기 ≈ 80)되어 흔들림을 볼 수 없다. **`--luminance-log`는 선형 평균(또는 노출 보정)을 기록해야 한다.**
- 기차 1080p 3000프레임(워밍업 300 이후, L/(1+L) 공간): 끔 — 최대−최소 2.36 %, 표준편차 0.29 %, 주된 주기 약 500프레임, 끝−처음 −0.25 %. 켬 — 최대−최소 1.11 %, 표준편차 0.29 %, 주된 주기 약 3000프레임(느린 표류), 끝−처음 −0.81 %, 평균 −0.15 %. 개선이 뚜렷하지 않다.

## 업스케일 (`caps/crop_*.png`, `caps/cmp_*.png`: 왼쪽 원래 해상도, 오른쪽 업스케일)
- 기차: 나뭇결 면의 가는 격자 무늬가 여전하고 조각·몰딩이 원래보다 흐리다. hf_ratio 0.59~0.70(디테일 손실), ssim 0.98.
- **욕탕: 새 결함** — 타일 줄눈 끊김·들쭉날쭉, 벽 타일 가장자리의 검은 선 조각, 욕조 아래 파란 띠의 검은 줄무늬, 욕조 판 선이 거칠다. hf_ratio 3.6~6.4(고주파 추가), ssim 0.998.
- 8개 경우 모두 후보 기준(0.95 ≤ hf_ratio ≤ 1.05, ssim ≥ 0.99) 밖이고 눈으로도 원래 해상도와 다르다 → 불합격.
