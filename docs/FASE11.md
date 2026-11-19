# Fase 11: deshacer, errores de compilación y trucos de GoldHEN

Ideas tomadas de [claude-for-ps4](https://github.com/blue0x1/claude-for-ps4) (MIT, Chokri Hammedi / blue0x1): su papelera con restauración, su lectura de errores del sistema y su gestor de GoldHEN. El código es propio (Garlic es GPLv3); el crédito va aquí y en CREDITS.md.

| Comando | Qué hace |
|---|---|
| `/deshacer` | Revierte el último cambio de la IA en `out/`: `/aplicar`, `/guardar` o una ronda de `/auto`. Los archivos que existían vuelven como estaban; los nuevos se borran. Repite para ir más atrás (se guardan 20 cambios en `/data/garlic_undo/`). |
| `/errores [log] [repo.zip]` | Lee el log de compilación (el zip `build-log-N` o un `.txt`/`.log` de `/data/garlic_files`; sin nombre usa el de número más alto), encuentra los errores del compilador, del enlazador y de Python y los prepara para tu siguiente mensaje, cada uno con las 11 líneas de código de alrededor sacadas del zip del repo (por defecto el último zip usado). Con el log y el repo ya no hay que pegar nada a mano. |
| `/trucos CUSA06548` | Lista los trucos (`json`, `shn`, `mc4`) y parches de GoldHEN que hay para ese juego en `/data/GoldHEN`, de solo lectura. Si ya hay uno de dinero o vida, no hace falta editar la partida. |

Flujo típico de programación: la IA cambia código → `/aplicar` → `/empaquetar` → subes al repo → Actions compila → bajas `build-log-N.zip` a `/data/garlic_files/` → `/errores` → "arréglalo" → `/aplicar` (y `/deshacer` si empeora).

## Partidas guardadas y archivos binarios (fase 10)
Ver `docs/GUARDADOS.md`. Resumen: `@zip` (lista todo), `@zip:archivo` (informe), `@zip:archivo:0x100-0x1ff` (hex), `@zip?=12500` (buscar un número), `@antes.zip~despues.zip` (qué cambió) y `/poke OFFSET TIPO VALOR ARCHIVO [zip]` (crea `nombre-mod.zip`; `/aplicar` ejecuta la línea `/poke` de la última respuesta).
