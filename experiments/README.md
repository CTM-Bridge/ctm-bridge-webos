# experiments/ — parked, not built

Reference only. **Not** in the webOS IPK and **not** built by
`scripts/build_ipk_macos.sh`. The TV is ARM-only; nothing here runs on it.

## main.c — `ctm_bridge_test` (standalone POC)
The original single-file CTMB bridge (its own SDL status UI + hidraw/BT +
evdev grab + stopSniff). Superseded on the TV by `ctm_bridge_lvgl_ui`
(`src/lvgl_ui.c` + `src/tv_bridge_worker.c`). Only ever built for x86_64
desktop/WSL testing.

## build_linux_x86_64.sh
Built `ctm_bridge_test` natively for x86_64 Linux and wrapped it in a
`run.sh`. The sole consumer of `main.c`.

To resurrect: restore a `ctm_bridge_test` target in `CMakeLists.txt`
pointing at `experiments/main.c` (needs SDL2 + SDL2_ttf), then run this
script.
