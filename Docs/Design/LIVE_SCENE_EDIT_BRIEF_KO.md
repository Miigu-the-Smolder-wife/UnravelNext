# 라이브 씬 편집 지시서: 조명 실시간 갱신 + 국소 안개 볼륨 (2026-09-30, 사용자 요청)

사용자가 Unity에서 씬을 직접 조정하고 싶어 한다. 조명을 추가하거나 옮기고, 오브젝트 위치를 바꾸고, 라이팅과 안개 설정을 바꾸는 작업이다. 오브젝트 이동과 전역 높이 안개(UnravelNextEnvironment의 Mie)는 이미 된다. 아래 두 가지는 렌더러에 기능이 없어 새로 만든다.

지금 상태(Unravel a054e6b4, 게임 프로젝트에 배포됨)
- 조명(점·스폿·사각·원판·해)은 `UnxSceneAddLight`로 커밋 때 고정된다. 브리지(`Assets/UnravelNextBridge/Runtime/UnravelNextScene.cs`)는 조명을 옮기거나 돌리거나 켜고 끄면 **레벨 전체를 다시 빌드**한다. 그래서 편집 중에 잠깐 멈추고, GI 캐시가 비워져 다시 수렴한다.
- 국소 안개는 없다. 목욕탕 김, 방 안의 연기, 기차 칸의 먼지 같은 **특정 구역의 안개**가 필요하다.

## 1. 목표

### A. 조명 실시간 갱신 (다시 빌드 없음)
- 편집기와 플레이 모드 모두에서 조명의 위치, 방향, 색, 세기, 범위, 스폿 각, 면광원 크기, 켜기와 끄기를 **다음 프레임에** 반영한다.
- 커밋 뒤에도 조명을 추가하고 제거할 수 있다. 해 방향도 매 프레임 갱신한다(시간대 무제한 속도 요구와 같은 경로).
- GI 캐시는 비우지 않는다. 조명 변화로 갱신이 필요한 칸만 기존 규칙대로 다시 수렴시킨다. 필요하면 조명 변화량으로 해당 칸의 이력을 줄이는 규칙을 설계한다.
- 국소광 VSM은 움직인 조명의 페이지만 무효화한다. **모든 조명이 매 프레임 움직이는 최악의 경우도 프레임 예산 안**이어야 한다. 해 속도 무제한 규칙과 같은 이유다.

### B. 국소 안개 볼륨
- Unity 컴포넌트 `UnravelNext/Fog Volume`(상자·타원체, 크기는 Transform)을 둔다. 속성:
  - 밀도: 가시거리 m로 입력하고, 내부적으로 소광 σt를 1/m 단위로 쓴다.
  - 산란 비율(albedo)과 색
  - 방향성 g
  - 가장자리 페이드 폭
  - 높이 감쇠
  - (선택) 잡음: 김·연기의 뭉침, 시간에 따라 흐르는 속도
- froxel 공기 적분에 매질로 합쳐져 해와 국소광, 그림자(볼륨 광선, 빛줄기)를 받는다. 조명 갱신과 같이 매 프레임 반영한다.
- 반사와 굴절(물·유리) 속에서도 보여야 한다. 적어도 설계에 비용과 방법을 적는다.

## 2. 규칙 (바꿀 수 없음)
- 설계 먼저 쓴다. 조명 수 L, 움직이는 조명 수 Lm, 볼륨 수 V, 볼륨이 덮는 froxel 비율을 변수로 한 **비용 공식**을 만들고, 게임 값을 대입한다. 게임 값은 욕탕 면광원 14개와 국소광, 기차 칸 조명이고, 볼륨은 화면에 16개까지를 예로 든다. 품질 정의와 검증 방법도 구현 전에 쓴다. 실측 하한(`Tools/Microbench/Results/FLOORS.md`)에 근거한다.
- 품질, 표본, 부하를 낮추지 않는다. 캐시와 재사용은 최악의 경우에도 예산 안이어야 한다.
- 표준 D3D12만 쓴다. DXIL은 커널당 약 200 KB, 디스패치마다 구조적 상한을 둔다. MSVC /W4 /WX로 빌드한다.
- C API는 기존 구조체처럼 `Size`, `Version` 필드로 판을 표시한다. 옛 브리지와도 안전하게 동작해야 한다(새 export가 없으면 브리지가 지금처럼 다시 빌드한다).
- Unreal 소스는 개념 참고만, 코드 복사 금지.

## 3. 만들 것
1. **설계 문서** `Docs/Design/LIVE_SCENE_EDIT_KO.md`: 자료 구조, 비용 공식, 품질 정의, 실패 모드, 구현 순서.
2. **네이티브:**
   - 조명 레코드를 프레임마다 갱신하는 경로. 조명 목록, froxel 목록, 면광원 LTC, 국소광 VSM 페이지 무효화, 반사·GI 광선의 국소광 표본이 모두 같은 레코드를 보게 한다.
   - 매질 볼륨: froxel 주입, 적분, 그림자.
3. **Host C API:**
   - 조명 id를 돌려주는 추가·제거 함수, 매 프레임 조명 갱신(예: `UnxFrameSetLights`).
   - 매 프레임 볼륨 목록(예: `UnxFrameSetMediaVolumes`).
4. **Unity 브리지 C#:** 이 저장소에는 Unity 프로젝트가 없다.
   - `Unity/BridgeStaging/` 같은 폴더에 다음 내용의 C# 파일과 적용 방법을 둔다.
     - `UnravelNextRendererNative.cs`에 새 export 선언
     - `UnravelNextScene.cs`는 조명 변화를 다시 빌드 대신 매 프레임 갱신으로 보냄
     - `UnravelNextFogVolume.cs` 컴포넌트(기즈모 포함)
     - 그래픽 설정 창에 볼륨 목록
   - 로컬 조정 세션이 Unravel 프로젝트(`C:\Users\USER\Unravel\Assets\UnravelNextBridge`)에 옮겨 넣고 확인한다.
5. **시험:**
   - 조명 이동 동치: 움직인 뒤 N프레임이 그 위치로 다시 빌드한 결과와 잡음 범위 안에서 같은지 본다.
   - 조명 추가·제거
   - 모든 조명이 매 프레임 움직일 때 비용
   - 볼륨 대 기준 경로 추적. 기준에 매질이 없으면 해석적 경우(균일 매질의 투과율, 단일 산란)로 대신한다.
   - GBV 경고 0

## 4. 브랜치와 확인
- `cloud/render-fixes`(5e31d8c)에서 `feature/live-scene-edit` 브랜치를 만들어 작업한다. 업스케일 작업과 섞지 않는다.
- 끝나면 `Docs/Status/CLOUD_BRIEF_KO.md`에 커밋별로 무엇을 했는지, 기대 비용 [예상], 로컬에서 확인할 것을 적는다.
- 로컬 확인은 조정 세션이 한다. `Tools/Verify/Verify-CloudBranch.ps1 -Ref origin/feature/live-scene-edit`, 추가된 시험, Unity 게임 프로젝트의 편집기 확인(조명을 끌어 옮기기, 안개 볼륨 배치)이다.

## 참고 위치
- 조명 수집과 다시 빌드 규칙: Unravel `Assets/UnravelNextBridge/Runtime/UnravelNextScene.cs`(Build, LightsChanged, AddLight)
- 전역 높이 안개: Unravel `Assets/UnravelNextBridge/Runtime/UnravelNextEnvironment.cs`, `EnvironmentDesc`
- 네이티브: `Native/Render/Passes/Lights`, `Native/Render/Passes/Shadow`(Froxel·VsmLocal*), `Native/Render/Passes/Volume`, `Native/Render/Passes/Atmosphere`, `Native/Host`
