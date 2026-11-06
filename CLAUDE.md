# Asphalt 4 Elite Racing (iOS Mach-O) → PS Vita

Port de Asphalt 4 Elite Racing (iOS Mach-O) a PS Vita con loader de Mach-O propio.
Leer `PORTING_PLAN.md` antes de modificar el loader.

## Estructura

- `source/macho.c`: Carga, parseo Mach-O, mapeo de segmentos, rebase y resolución de imports.
- `source/darwin.c`: Capa de compatibilidad libc/libgcc/pthread/semáforos de Darwin.
- `game_app` → symlink a la carpeta `.app` original del IPA.
- `ux0_data/asphalt4eliteracing/`: staging de `ux0:data/asphalt4eliteracing/` (`ASPHALT4.app/` -- nombre fijo en el binario --, `rebase.bin`, `saves/`, `logs/`); lo genera `tools/stage_data.sh` (convierte música ALAC->WAV y vídeo->H.264).
- `hosttest/`: el mismo loader en Linux ARM32 softfp (Docker): `sh hosttest/run.sh build && sh hosttest/run.sh run 300`.
- `docs/imports.md`: imports -> shim (regenerar con `python3 -I tools/imports_table.py`).
- `PORTING_PLAN.md`: Plan de port y estado actual de fases.
- `port_progress.md`: Registro de progreso.

## Reglas

- ABI iOS ARM: softfp en ARMv6/v7 (floats en registros enteros), r9 temporal (preservar en callbacks).
- Nada del IPA original en el repositorio git (.gitignore anti-DMCA).
- Un bug por corrida de consola física, guiado por logs.
