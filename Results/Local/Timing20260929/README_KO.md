# 렌더러 패스별 시간 (2026-09-29, RTX 4080)

`unx_gate_shadow_renderergate --scene Results/R/GiInterior/<scene>.unxscene --resolution <WxH> --out <dir>`, 정지 카메라 600프레임, GpuLock timing, 경합 0. 파일 이름은 `<빌드>_<장면>_<출력 해상도>.json`.
- 887b5d8 = main (업스케일은 4K 출력에서만, 내부 1440p).
- 974c6bb = cloud/render-fixes (output.render_scale 0.6667: 1080p → 내부 720p, 1440p → 960p, 4K → 1440p). 업스케일 화면은 원래 해상도보다 확연히 흐려 불합격이다.
장면 파일(`*_ev6.unxscene`, 55·195 MB)은 저장소에 없다. 로컬 `Results/R/GiInterior/`에 있다.
