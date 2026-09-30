# 로컬 검증 안내서 (클라우드 브랜치 → 이 PC)

클라우드 세션에는 GPU가 없다. 클라우드가 `cloud/render-fixes`에 올린 코드는 이 PC(RTX 4080)에서 빌드하고, 시험·캡처·측정으로 확인해야 쓸 수 있다. 이 안내서는 그 확인을 누가 하든 같은 방법으로 하도록 정리한 것이다. 확인 결과는 클라우드 세션과 조정 세션에 돌려준다. **main 적용은 사용자의 허락이 있을 때 조정 세션이 한다.** 이 안내서를 따르는 쪽은 main을 건드리지 않는다.

## 0. 한 줄 요약

```powershell
cd C:\Users\USER\UnravelNext
git pull -q origin main
powershell -NoProfile -ExecutionPolicy Bypass -File Tools\Verify\Verify-CloudBranch.ps1
powershell -NoProfile -ExecutionPolicy Bypass -File Tools\Verify\Verify-CloudBranch.ps1 -Phases publish
```

- 첫 명령 두 줄은 이 스크립트가 들어 있는 main을 받는다.
- 셋째 줄이 빌드부터 보고서까지 모두 돌린다(40~50분 [예상]).
- 넷째 줄은 PFM을 뺀 결과를 `origin/local/verify-<커밋>` 브랜치에 올린다.
- 결과는 `Results\Local\Verify-<커밋>\SUMMARY_KO.md`에 있다. 5절대로 이미지를 눈으로 보고, 6절 양식으로 회신한다.

## 1. 시작 전에 반드시 확인

1. **사용자가 게임 중이 아닌지.** 스크립트가 알려진 게임(League of Legends, Remnant 2, Overwatch 등)을 보면 스스로 멈춘다. 목록에 없는 게임도 있을 수 있으니, 사용자가 게임한다고 했으면 돌리지 않는다. GPU 작업은 게임에 영향을 준다.
2. **다른 GPU 측정이 돌고 있지 않은지.** `C:\Users\USER\UnravelNext\.gpulock\current.json`이 있으면 누가 잠금을 쥐고 있는 것이다. 스크립트는 잠금이 풀릴 때까지 기다린다.
3. **장면 파일이 있는지.** `C:\Users\USER\UnravelNext\Results\R\GiInterior\bath_bt0_ev6.unxscene`, `train_v0_ev6.unxscene`(git에 없는 큰 파일). 없으면 멈추고 조정 세션에 알린다.
4. **Python이 되는지.** `python -c "import numpy, PIL"`가 오류 없이 끝나야 한다.

### 하지 말 것
- 사용자나 다른 세션의 프로세스를 끄지 않는다(게임, 클라이언트, Unity, 다른 빌드).
- 엔진 코드를 고치지 않는다. 이상을 발견하면 고치지 말고 회신한다.
- `main`에 병합하거나 푸시하지 않는다. `cloud/render-fixes`에도 푸시하지 않는다(클라우드가 작업 중이다).
- PFM 파일(장당 25~100 MB)을 커밋하지 않는다. `publish` 단계가 알아서 뺀다.

## 2. 스크립트가 하는 일

`Tools\Verify\Verify-CloudBranch.ps1` (인수 없이 실행하면 아래 순서대로 모두 한다)

| 단계 (`-Phases`) | 하는 일 | 시간 [예상] |
|---|---|---|
| `build` | `Tools\CI\Build.ps1 -Track all -Committed -Ref origin/cloud/render-fixes` → `C:\Users\USER\UnravelNext-gate`에 빌드한다. 빌드가 끝나면 장면 파일을 거기에 하드링크한다(빌드가 작업 폴더를 정리하면서 지우므로 매번 다시). | 1~3분 |
| `tests` | reflectionanalytic, gianalytic, hostmotion, LocalShadowTests, FroxelTests | 약 2분 |
| `caps` | 기차·욕탕 × 1440p·1080p를 원래 해상도(`--capture`)와 업스케일(`--capture-output`)로 캡처한다. 정지 장면은 둘 다, 움직임(`--moving`)과 해 이동(`--sun-deg-per-s 20`, 조명 변화 잔상 확인용)은 기차만. | 약 8분 |
| `convergence` | 장면을 연 직후(가장 심한 가림 해제)부터 1·4·16·64·256프레임째와 600프레임 수렴 화면을 업스케일 출력으로 캡처해, 오차와 "타일 P95 ≤ 3 %에 닿는 프레임"을 잰다(사용자 요구 2026-09-30: 몇 프레임 안에 깨끗해야 한다). `conv/strip_*.png`로 눈으로도 본다. | 약 6분 |
| `unity` (따로 실행, 배포 뒤) | 게임 프로젝트(`-GameProject`, `-GameScene`, 기본은 목욕탕)를 Unity batchmode로 열어 플레이 모드 게임 뷰를 캡처한다. 레벨 빌드 뒤 1·4·16·64프레임, 가만히 선 8프레임의 깜빡임 지도, 걷는 중. 그 프로젝트에 **이미 배포된** 렌더러를 보고, 프로젝트는 닫혀 있어야 한다. | 약 5분 |
| `determinism` | 1080p에서 `debug.deterministic=true`로 두 번, 기본 모드로 두 번 캡처한다(`--warmup-frames 300`). | 약 3분 |
| `luminance` | 욕탕 1080p 3000프레임의 프레임별 평균 밝기(`--luminance-log`) | 약 1분 |
| `timing` | 기차·욕탕 × 1080p·1440p·4K를 두 번 잰다(GpuLock timing, 비동기는 기본값대로 꺼짐). | 약 6분 |
| `report` | `Tools\Verify\verify_analyze.py`가 `SUMMARY_KO.md`, 비교 PNG, 밝기 그래프, 패스 표를 만든다. 업스케일 수치는 클라우드의 `Tools\ImageQuality\UpscaleCompare.py`로 잰다. | 1분 |
| `publish` (따로 실행) | PFM을 뺀 결과를 `origin/local/verify-<커밋>`에 올린다. | 1분 |

- 모든 GPU 실행은 `Tools\CI\GpuLock.ps1` 안에서 한다. 정확성은 correctness, 측정은 timing 잠금이다.
- 일부만 다시 돌리려면 `-Phases caps,report`처럼 쉼표로 적는다. 빌드는 한 번 했으면 다시 하지 않아도 된다(같은 커밋일 때).
- `-Quick`은 스크립트 자체를 점검하는 짧은 실행이다. 결과를 판정하는 데 쓰지 않는다.
- `-DryRun`은 GPU를 전혀 쓰지 않고 실행 묶음만 만들어 문법을 검사한다. 게임 중에도 된다.
- 결과 폴더를 바꾸려면 `-Out <폴더>`를 쓴다.

## 3. 멈췄을 때

| 메시지 | 뜻 | 할 일 |
|---|---|---|
| `게임 실행 중(...)` / `게임이 시작되어 멈췄다` | 게임 감지 | 게임이 끝난 뒤 남은 단계만 `-Phases`로 다시 |
| `빌드 실패` | 클라우드 코드가 이 PC 컴파일러(MSVC /W4 /WX)에서 안 됨 | `build.log`에서 `FAILED:`와 `error` 줄을 찾아 클라우드에 그대로 전달 |
| `장치 제거(TDR)` | GPU가 멈췄다가 리셋됨 | **모든 GPU 실행을 멈춘다.** 어느 실행이었는지(`steps.csv` 마지막 줄)와 그 로그 끝 30줄을 조정 세션과 클라우드에 즉시 알린다. 다시 돌리지 않는다. |
| 시험 종료 코드가 0이 아님 | 시험 실패 | 그 시험 로그(`tests\*.log`)에서 `FAIL` 줄을 찾아 회신에 넣는다. 원인 추적이나 재실행은 하지 않는다(실패는 실패다). |
| `장면 파일이 없다` | 1절 3 | 조정 세션에 알린다 |

스크립트가 멈춰도 GPU 잠금은 GpuLock이 풀어 준다. 강제 종료가 필요해 보여도 다른 프로세스는 건드리지 않는다.

## 4. 결과 폴더

`Results\Local\Verify-<커밋>\`
- `SUMMARY_KO.md`: 모든 결과를 한 문서로 정리한 것
- `steps.csv`: 실행마다 이름, 종료 코드, D3D12 메시지 수, 초
- `caps\cmp_*.png`(전체), `caps\crop_*.png`(가운데 1:1): 왼쪽 원래 해상도, 오른쪽 업스케일
- `caps\*_up_cmp.json`: 업스케일 수치
- `det\*.pfm`: 결정론 캡처
- `lum\*.csv`, `lum\*.png`: 밝기 곡선
- `timing\...\*.json`: 패스별 시간 원자료
- 실행 로그(`*.log`)

## 5. 눈으로 볼 것 (숫자보다 먼저)

사용자의 기준은 **"게임플레이 중 다양한 행동에서도 보이는 것이 같다"** 이다. 숫자가 통과해도 눈에 보이는 차이가 있으면 불합격이다.

1. **업스케일 정지** (`crop_train_2560x1440_static.png`, `crop_train_1920x1080_static.png`, 욕탕도 같이)
   - 오른쪽(업스케일)에 왼쪽(원래)에 없는 **격자·빗살 무늬**(나뭇결 면 전체에 가는 선이 규칙적으로 깔림)가 있는가
   - **가로 몰딩 선의 계단·울림**이 있는가
   - 조각 무늬 윤곽과 나뭇결이 **흐리거나 뭉개지는가**
   - 밝기와 색이 다른가
2. **업스케일 움직임** (`crop_train_*_moving.png`): 조각이나 가장자리가 **번지거나 잔상**이 남는가, 밝은 점이 생기는가.
3. **조명 변화** (`crop_train_*_sun.png`): 해가 움직이는 중에 창으로 드는 빛 무늬 가장자리에 **꼬리(잔상)** 가 왼쪽보다 긴가.
4. **욕탕 바닥·벽**(`cmp_bath_*_static.png`): 원래 해상도에 없는 **얼룩, 밝은 점, 네모 블록**이 있는가.
5. **업스케일 수치**(`SUMMARY_KO.md`): 후보 기준은 `0.95 ≤ hf_ratio ≤ 1.05`이고 `ssim ≥ 0.99`다.
   - hf_ratio가 1.05를 넘으면 무늬나 계단이 더해진 것이다. 84e789f는 1.64였다.
   - hf_ratio가 0.95 미만이면 흐린 것이다.
6. **결정론**: `det` 쌍은 "비트 동일"이어야 한다. `def` 쌍의 평균 밝기 차는 이전 4.41%에서 얼마나 줄었는가.
7. **밝기 곡선** (`lum\bath_1080_default.png`): 워밍업(빨간 선) 뒤에 수백 프레임 주기로 수 % 흔들리는가. 요약의 "최대−최소 %"와 "주된 주기"도 적는다.

## 6. 회신 양식 (클라우드 세션에 붙여 넣기)

```
로컬 검증 <커밋> (RTX 4080, Tools/Verify/Verify-CloudBranch.ps1): origin/local/verify-<커밋> 의 Results/Local/Verify-<커밋>/SUMMARY_KO.md 와 PNG를 먼저 봐라.
1) 시험: <통과/실패 목록, 실패면 FAIL 줄>
2) 업스케일 정지: <격자·계단·흐림이 보이는지, 어디서> / 수치 <hf_ratio, ssim (1440p·1080p, 기차·욕탕)>
3) 업스케일 움직임·조명 변화: <번짐·잔상·밝은 점>
4) 결정론: det <비트 동일 여부>, def <평균 밝기 차 %> (이전 4.41 %)
5) 밝기 곡선: <최대−최소 %, 주된 주기 프레임>
6) 성능 [실측]: 기차 1080p/1440p/4K <ms>, 욕탕 <ms>, m.upscale <ms> (이전 직렬: 기차 1440p 5.33·1080p 3.79, 욕탕 1440p 6.10·1080p 4.40)
7) 그 밖에 이상한 점: <있으면>
```

조정 세션에는 같은 내용과 함께 **main에 적용해도 되는지에 대한 판단 근거**를 전달한다. 판단 근거는 시험 전부 통과, 업스케일이 눈으로 같음, 성능이 이전보다 나빠지지 않음이다. 적용 여부는 사용자가 정한다.
