# Camino de mejoras para Garlic LLM (fork)

Estado y orden de trabajo. Cada fase se compila con el workflow `build-pkg.yml` y se prueba en la PS4.

## Cómo usar el workflow

1. Sube estos archivos a tu fork (conservando las carpetas) y haz commit en `main`.
2. Pestaña **Actions → Build Garlic LLM pkg → Run workflow** (o se lanza solo al hacer push a `chat/`, `shaders/` o los `build*.py`).
3. Al terminar, descarga el artefacto `garlic-llm-pkg-N` (un zip con el `.pkg`) y, si falla, `build-log-N`.
4. El pkg sale **sin modelos** (~40 MB). La app los busca en `/app0/models` y luego en `/data`. Copia una vez a `/data` por FTP de GoldHEN (puerto 2121): `qwen3.5-9b-q4s.bin`, `qwen3.5-9b-mtp.bin`, `parakeet-v3-q4i.bin`, `chatterbox-t3.bin`, `chatterbox-s3gen-mf.bin`.

Aviso: este pkg tiene el mismo TITLE_ID (`PSAI00001`) que el pkg completo de Hugging Face, así que al instalarlo **reemplaza** esa app y los modelos que traía dentro dejan de estar. Copia los modelos a `/data` antes de abrirlo.

Se probó el build de punta a punta en Linux (Zig 0.13.0 + OpenOrbis v0.5.4 + libssl 1.1). No se probó en la consola.

## Antes de compilar (revisar a mano)
1. `build_chat.py` debe compilar `chat/input.c` y `chat/files.c` (si toma `chat/*.c` solo, no hay nada que hacer; si tiene una lista, añádelos) y llevar `-lSceKeyboard` en `LIBS`. Eso no lo pude ver: el fork no venía en el zip.
2. Los módulos nuevos (`input.c`, `files.c`) se compilaron contra los headers reales de OpenOrbis v0.5.4 y `files.c` se probó en un PC. `main.c` modificado NO se pudo compilar aquí (faltan `engine.h`, `ui.h`...): si el CI da error, el log dirá la línea.
3. Aviso del TITLE_ID ya descrito arriba: este pkg reemplaza al completo de Hugging Face.

## Fase 0: base reproducible (hecha)
`.github/workflows/build-pkg.yml`. Para cambiar versiones: `ZIG_VERSION`, `OPENORBIS_TAG` y `OPENORBIS_ASSET` arriba del archivo.

## Fase 1: log de conversaciones (hecha, sin probar en consola)
`chat/chatlog.c/.h` + 3 líneas en `chat/main.c`. Cada turno se guarda en `/data/chats/chat_NNNN.jsonl` (una línea JSON por mensaje: `n`, `role`, `cut`, `text`). Un chat nuevo (△ o contexto lleno) abre otro archivo; el contador está en `/data/chats/counter.txt`. Para apagarlo: `/data/garlic_chatlog.txt` con `0`. No registra las peticiones de la API. Se lee por FTP.

## Fase 2: retomar un chat con su contexto
- En `start_turn` (main.c), la rama `pos == 0` arma el prompt con `system_ids()`. Ahí se añadiría el chat guardado, codificado como turnos `<|im_start|>user ... <|im_end|>` y `assistant`, antes del mensaje nuevo.
- Hay que recrear los mensajes en pantalla (`msg_new` / `msg_append`) y una pantalla para elegir chat.
- El buffer `ids[8192]` limita lo que se puede releer de una vez, y releer cuesta tiempo de prefill (se ve en el log: "prompt of N tokens in X s").
- "Sin contexto previo" ya es el comportamiento actual de △ (chat nuevo).

## Fase 3: prompt de sistema editable (hecha, sin probar en consola)
Si existe `/data/garlic_system.txt` (UTF-8, hasta ~4 KB), se usa en lugar del prompt de fábrica; se lee en cada chat nuevo, así que se edita por FTP sin reiniciar. Ejemplo para usarla como ayudante de desarrollo: `docs/garlic_system.txt`.

## Fase 4: archivos (leer, zips y documentos, escribir: añadidas, sin probar en consola)
Los comandos están resumidos en `docs/COMANDOS.md`.

**Leer.** Sube archivos por FTP (GoldHEN, puerto 2121) a `/data/garlic_files/` y menciónalos como `@build-pkg.yml`: su texto viaja al modelo antes de tu pregunta (el chat muestra solo lo que escribiste y una nota "adjuntado / cortado / no encontrado"). Un `@` solo lista los archivos (también `out/` y `web/`). Subcarpetas: `@out/chat/foo.c`. Cualquier archivo de texto sirve; un binario se rechaza. Límites: 14 000 bytes por mensaje y hasta 8 archivos.

**Zips y documentos** (`chat/zipread.c`, un inflate propio sin librerías). `@repo.zip` lista el árbol (oculta imágenes, modelos y archivos binarios, y quita la carpeta común `repo-main/`); `@repo.zip:chat/` o `@repo.zip:*.yml` filtra; `@repo.zip:ruta/archivo` trae un archivo (ruta completa, sin la carpeta raíz, o solo el nombre si es único). `@doc.docx`, `.pptx`, `.xlsx`, `.odt/.ods/.odp` traen el texto. No hay zip64, cifrado ni PDF. Solo se descomprime el principio de cada archivo.

**Escribir.** `chat/outfiles.c`. `/guardar` guarda los bloques ``` con nombre de la última respuesta en `/data/garlic_files/out/` (conserva carpetas). Nombre: línea del bloque (```yaml ruta/x.yml), primera línea `# file: ruta`, o línea anterior con la ruta entre acentos graves. Solo escribe dentro de `out/`; máximo 12 archivos y 60 KB por orden.

Prompt de sistema: `docs/garlic_system.txt` (copiar a `/data/garlic_system.txt`) enseña al modelo a etiquetar sus bloques y a no decir que tiene acceso a nada.

Pendiente: pantalla para elegir archivo con el mando, PDF, resumir archivos que no caben en el contexto.

## Fase 5: teclado y mouse USB (hecha, sin probar en consola)
`chat/input.c/.h`, portado de tu psbrowser (mismo `mouse.cpp` por `sceKernelDlsym`; teclado con `sceKeyboard*`). En el chat: escribir cualquier tecla (o Enter) abre el teclado de la app con esa letra puesta; dentro, Enter envía y Esc cierra; Re Pág / Av Pág / flechas y la rueda hacen scroll; Esc detiene una respuesta; Ctrl+N = chat nuevo. Solo ASCII (como en el psbrowser: ñ y acentos no). El mouse solo usa la rueda por ahora. OPTIONS sigue abriendo el teclado del sistema.

## Fase 6: otras IAs
- Los pesos están convertidos a formatos propios y los kernels (`shaders/`) son para Qwen, así que otra arquitectura exige kernels nuevos.
- Opción realista: un modo "IA remota" con un cliente HTTP a un servidor OpenAI-compatible de tu red (Ollama, llama.cpp). HTTPS sí es posible: `libSceHttp` + `libSceSsl` del sistema, como hace `app/netclient.cpp` de tu psbrowser (probado en PS4 en su fase 0). Hay que añadir `-lSceSsl -lSceHttp` a `LIBS` en `build_chat.py`. La API que ya trae la app es el servidor; falta el cliente.
- Ya existe un build 4B: `python build_chat.py --4b` (app aparte, `PSAI00002`).

## Fase 7: red para la IA (comandos hechos; falta el backend HTTPS)
`chat/webcmd.c`, `chat/htmltext.c`, `chat/netget.h`. Interruptor `/red on|off` (apagado por defecto), `/gh usuario/repo [ruta]` (árbol y README, o un archivo, desde GitHub), `/url https://...` y `/web palabras` (DuckDuckGo HTML). El resultado se guarda en `/data/garlic_files/web/` y viaja al modelo con tu siguiente mensaje. Solo sale a la red lo que tú escribes.

La capa está probada en PC con un servidor falso. Falta el backend real, `net_get()` (ver `chat/netget.h`): hasta entonces `chat/netget_stub.c` responde "no network backend". Se hace portando a C el `netclient.cpp` del psbrowser (`libSceHttp` + `libSceSsl`, con los casts que piden las cabeceras de OpenOrbis) y añadiendo `-lSceSsl -lSceHttp` a `LIBS` en `build_chat.py`; la versión fuerte reemplaza a la débil sin tocar nada más. Ojo: la pantalla del chat espera mientras se descarga, así que el backend debe tener su propio timeout (unos 15 s). Wikipedia: las páginas `/wiki/` fallaban con 0x80431073 y `action=raw` funcionaba.

## Fase 8: ver fotos
Es la más difícil: hace falta un codificador de visión (kernels y pesos nuevos) y memoria de GPU que el 9B casi no deja libre. Lo probable es un modo aparte o el modelo de 4B. Se evalúa al final.

## Licencia
El proyecto es GPLv3: tu fork debe seguir siendo GPLv3 y mantener los créditos (`CREDITS.md`).
