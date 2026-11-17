# Camino de mejoras para Garlic LLM (fork)

Estado y orden de trabajo. Cada fase se compila con el workflow `build-pkg.yml` y se prueba en la PS4.

## Cómo usar el workflow

1. Sube estos archivos a tu fork (conservando las carpetas) y haz commit en `main`.
2. Pestaña **Actions → Build Garlic LLM pkg → Run workflow** (o se lanza solo al hacer push a `chat/`, `shaders/` o los `build*.py`).
3. Al terminar, descarga el artefacto `garlic-llm-pkg-N` (un zip con el `.pkg`) y, si falla, `build-log-N`.
4. El pkg sale **sin modelos** (~40 MB). La app los busca en `/app0/models` y luego en `/data`. Copia una vez a `/data` por FTP de GoldHEN (puerto 2121): `qwen3.5-9b-q4s.bin`, `qwen3.5-9b-mtp.bin`, `parakeet-v3-q4i.bin`, `chatterbox-t3.bin`, `chatterbox-s3gen-mf.bin`.

Aviso: este pkg tiene el mismo TITLE_ID (`PSAI00001`) que el pkg completo de Hugging Face, así que al instalarlo **reemplaza** esa app y los modelos que traía dentro dejan de estar. Copia los modelos a `/data` antes de abrirlo.

Se probó el build de punta a punta en Linux (Zig 0.13.0 + OpenOrbis v0.5.4 + libssl 1.1). No se probó en la consola.

## Fase 0: base reproducible (hecha)
`.github/workflows/build-pkg.yml`. Para cambiar versiones: `ZIG_VERSION`, `OPENORBIS_TAG` y `OPENORBIS_ASSET` arriba del archivo.

## Fase 1: log de conversaciones (hecha, sin probar en consola)
`chat/chatlog.c/.h` + 3 líneas en `chat/main.c`. Cada turno se guarda en `/data/chats/chat_NNNN.jsonl` (una línea JSON por mensaje: `n`, `role`, `cut`, `text`). Un chat nuevo (△ o contexto lleno) abre otro archivo; el contador está en `/data/chats/counter.txt`. Para apagarlo: `/data/garlic_chatlog.txt` con `0`. No registra las peticiones de la API. Se lee por FTP.

## Fase 2: retomar un chat con su contexto
- En `start_turn` (main.c), la rama `pos == 0` arma el prompt con `system_ids()`. Ahí se añadiría el chat guardado, codificado como turnos `<|im_start|>user ... <|im_end|>` y `assistant`, antes del mensaje nuevo.
- Hay que recrear los mensajes en pantalla (`msg_new` / `msg_append`) y una pantalla para elegir chat.
- El buffer `ids[8192]` limita lo que se puede releer de una vez, y releer cuesta tiempo de prefill (se ve en el log: "prompt of N tokens in X s").
- "Sin contexto previo" ya es el comportamiento actual de △ (chat nuevo).

## Fase 3: prompt de sistema editable
`system_ids()` usa `tr(S_SYSTEM_PROMPT)`, y `sys_ids[]` es de 1024 tokens. Plan: si existe `/data/garlic_system.txt`, usarlo en su lugar (con ese tope).

## Fase 4: archivos
- El Python de la app está aislado a propósito: `open()` falla al leer y las escrituras no van a ningún lado (`chat/pyrun.c`, `garlic_open` y el BOOT).
- Plan: un módulo propio `garlic_files` limitado a `/data/garlic_files/` (listar, leer, escribir), y una forma de pegar un archivo de texto en el chat (se cuenta contra los 16K tokens de contexto).
- Subir archivos a la consola es por FTP; la app solo los lee de esa carpeta.

## Fase 5: teclado y mouse
Hoy OPTIONS abre el teclado del sistema (`sceImeDialog`), que sirve para escribir con un teclado USB. Para teclado/mouse directo se puede reutilizar lo de tu psbrowser: `app/mouse.cpp` (carga `libSceMouse` en runtime con `sceKernelDlsym`, porque `orbis/Mouse.h` del toolchain son stubs vacíos) y el teclado con el sample `keyboard` de OpenOrbis (`-lSceKeyboard`). Ojo: el propio `mouse.cpp` se declara versión de diagnóstico (el layout de `SceMouseData` salió de memoria, no de logs); no encontré confirmación de que mouse y teclado USB funcionen en la consola, así que hay que probarlos antes de depender de ellos.

## Fase 6: otras IAs
- Los pesos están convertidos a formatos propios y los kernels (`shaders/`) son para Qwen, así que otra arquitectura exige kernels nuevos.
- Opción realista: un modo "IA remota" con un cliente HTTP a un servidor OpenAI-compatible de tu red (Ollama, llama.cpp). HTTPS sí es posible: `libSceHttp` + `libSceSsl` del sistema, como hace `app/netclient.cpp` de tu psbrowser (probado en PS4 en su fase 0). Hay que añadir `-lSceSsl -lSceHttp` a `LIBS` en `build_chat.py`. La API que ya trae la app es el servidor; falta el cliente.
- Ya existe un build 4B: `python build_chat.py --4b` (app aparte, `PSAI00002`).

## Fase 7: integración con el navegador
La PS4 no corre dos apps a la vez. Camino: una herramienta "buscar en la web" dentro de Garlic (descarga HTTP, extrae texto y se lo pasa al modelo), reutilizando código del navegador. Con `sceHttp` (como tu psbrowser) también sirve HTTPS. Para el modelo conviene texto limpio: `htmlToLines` de tu repo (104 líneas, hay que pasarlo a C) y buscadores en HTML (DuckDuckGo). Ojo: las páginas `/wiki/` de Wikipedia fallaban con 0x80431073, mientras `action=raw` funcionaba. Las cabeceras de OpenOrbis declaran `sceHttpSetRecvTimeOut`, `sceHttpSetResponseHeaderMaxSize` y `sceSslTerm` sin parámetros: hay que llamarlas con un cast, como hace `netclient.cpp`.

## Fase 8: ver fotos
Es la más difícil: hace falta un codificador de visión (kernels y pesos nuevos) y memoria de GPU que el 9B casi no deja libre. Lo probable es un modo aparte o el modelo de 4B. Se evalúa al final.

## Licencia
El proyecto es GPLv3: tu fork debe seguir siendo GPLv3 y mantener los créditos (`CREDITS.md`).
