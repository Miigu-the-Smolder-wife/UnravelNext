# 로컬 수정 작업 상태 — 2026-09-30

사용자의 작업 마무리 요청 시점에 보존한 상태다. **전체 수정 완료 또는 main 적용 승인 상태가 아니다.**

## 위치와 공개 상태

- 작업 폴더: `C:\Users\USER\UnravelNext-render-followup`
- 브랜치: `cloud/render-fixes`
- 코드 커밋 기준: `81f8288` (아래 네 커밋). 이 작업의 커밋은 아직 GitHub에 push하지 않았다.
- 업스케일·화소 footprint 수정/실험은 작업 폴더의 **미커밋 변경**으로 보존했다. 현재 실행 파일에는 마지막 미승인 실험도 들어 있다. 출시용 또는 검증 합격 바이너리로 사용하지 않는다.
- `C:\Users\USER\Unravel`에서 작업한 것이 아니며, main에는 적용하지 않았다. 빌드·검증 프로세스는 종료된 상태로 확인했다.
- 실행 파일: `build/rendercheck/bin/unx_gate_shadow_renderergate.exe`; 큰 PFM 원본과 실험 백업은 같은 작업 폴더의 `build/`에 있다.

## 커밋한 수정과 확인 범위

| 커밋 | 변경 | 실제 확인 |
|---|---|---|
| `da5b61a` | visibility 시험의 투영·판독을 원래 raster 크기로 일치. alias 자원에 대한 WriteBufferImmediate를 명시적 CopyBufferRegion으로 교체 | 원래 실패 화소를 포함한 covering-triangle 시험, 전체 visibility 9/9 PASS. 오류 0, WARNING 926 소멸. 별도 기존 WARNING 820은 남음 |
| `eae9c17` | luminance CSV를 exposed-linear 평균과 invalid 화소 수로 변경 | 욕탕 평균 36~52로 밝은 장면 포화 제거. GPU 평균과 캡처 CPU 평균 차이 약 4.92e-8, invalid 0. 3000프레임 이력 분리 재평가는 미실행 |
| `a139daf` | 반사 split/overflow inline 경로 및 방향 재생의 strict FP, gate 캡처 프레임 정렬 | 당시 소스에서 기차·욕탕 frame 0/300 반복 네 쌍 차이 0화소. 초기 프레임 split/강제 inline 비트 일치. 이후 미커밋 업스케일 전체에는 이 결론을 적용하지 않음 |
| `81f8288` | GI hit anchor 후퇴량이 실제 광선 구간을 벗어나지 않도록 제한 | 벽 밖 anchor 원인 수정. GI analytic PASS 및 524416 probe 값 반복 차이 0. Reflection analytic 전체 PASS, GPU validation 오류 0. **아래 128프레임 제한을 반드시 함께 읽을 것** |

[예상] 위 수정은 광선·표본 수를 줄이지 않는다. strict FP는 비용 증가 가능성이 있고, anchor는 min/거리 인수가 추가된다. 새 성능 수치는 측정·판정하지 않았다.

## furnace 제한 — 원래 조건까지 완료된 것은 아님

`81f8288`은 equilibrium 시험의 기본 cold 프레임을 128에서 256으로 늘렸다. 256에서는 deterministic furnace M worst 2.70%, outlier 0, PASS다. 그러나 **128에서는 M worst 4.89%, 11 outliers로 여전히 FAIL**이다. 이를 원래 128프레임 회귀의 완전 해결이라고 주장할 수 없다. 허용 오차는 바꾸지 않았지만 준비 프레임 조건이 달라졌으므로 별도 수용 판단/추가 수정이 필요하다.

## 미커밋 업스케일 상태 — 불합격

- 정지 표면 motion의 두 큰 투영값 뺄셈 오차를 CPU의 행렬 차이로 제거했다. 욕탕 정지 motion 판독은 전체 화소 0으로 확인했다.
- 출력 화소 크기를 normal variance, coverage 면적·mask·centroid, 그림자/ray cone, geometry LOD에 전달했다. 평면 반사 crop에도 주 뷰의 비율을 전달했다.
- scaled coverage LUT 시험: native/0.5/2/3/0.75, 각 786432 삼각형의 비모호 표본에서 mismatch 0. 시험 로그 표시 정리 이후 C++ 재빌드는 아직 하지 않았다.
- 원래 해상도의 기차 캡처 비교에서는 9028화소의 부동소수점 차이가 관찰됐다(상대 1% 초과는 0). 따라서 이 묶음을 비트 동일 변경이라고 주장하지 않는다.
- 선형 FP32 이력, 공간 평균을 이용한 밝기 보정, 출력 좌표계 반사 이력, 반사 control residual 분리 등을 실험했다. **채택 완료된 알고리즘이 아니다.** 특히 마지막 control 실험은 합격으로 개선되지 않았다.
- 마지막 gate 빌드는 MSVC /W4 /WX 및 기존 DXIL 크기 gate를 통과했다. 그 실행 파일로 기차 frame 300 캡처를 얻었다. 마지막 소스 전체에 대한 visibility/GI/reflection suite, 반복 결정론, 이동/가림 해제/빛 변화/1440p 검사는 완료하지 않았다.

마지막 기차 1080p 비교 [실측, 화질]: HF ratio **0.9902**, SSIM **0.98595**. 기준 HF 0.95~1.05, SSIM >=0.99 중 SSIM 미달이며, 장식 주변 격자가 눈으로도 남는다. 원래 해상도끼리 frame 300/364 비교는 HF 1.0063, SSIM 0.99271이므로 원래 영상의 변동만으로 남은 오차를 설명할 수 없다.

- [전체 비교: 왼쪽 원래 해상도 / 오른쪽 마지막 실험](train-reflection-control-cmp.png)
- [확대 비교: 격자 결함이 남는 부분](train-reflection-control-crop.png)
- [마지막 화질 수치](train-reflection-control.json)

반사를 분리한 진단에서 화면 프로브 반사만 남긴 비교는 SSIM 0.9958, 광선/명시적 반사를 남긴 비교는 0.9856이었다. GI 프로브 위치의 지터 의존성도 조사하기 시작했으나 **확정 원인이나 구현된 수정이 아니다**.

## 적용 보류와 필요한 후속 검증

업스케일 불합격 실험을 정리하고 실제 알고리즘을 수정해야 한다. 원래 해상도 강제 복귀, 표본/품질 감소, 합격 기준 완화로 대체하지 않는다. 원래 furnace 128 조건, 최종 동일 커밋 반복 비트 비교, 3a~4d 스위치별 A/B, 움직임·빛 변화 화질 검증이 남는다. `gi.split_bounce_history=false` 유지.

게임 실행 중이라는 요청에 따라 성능을 평가하지 않았다. 로그에 도구가 자동 출력한 시간은 유효한 성능 측정으로 사용하지 않는다. 84e789f 사용자 실측 1440p 기차 5.33 / 욕탕 6.10 ms에서 3 ms까지 남은 2.33 / 3.10 ms와 세션 11.4의 과거 패스 배분은 갱신할 새 근거가 없다.
