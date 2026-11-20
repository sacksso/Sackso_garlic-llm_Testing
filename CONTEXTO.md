# CONTEXTO — Garlic LLM en PS4 (hasta fase 12)

## Entorno
- PS4 Slim fw 13.52, GoldHEN 2.4b18.12. Fork `Sackso_garlic-llm_Testing`. Qwen3.5-9B en la GPU.
- Build en GitHub Actions: Zig 0.13 + OpenOrbis; `build_chat.py` compila `chat/*.c` por glob.
- Modelo local: ctx 16384 tokens, prefill ~60-70 tok/s (cuello de botella), decode ~30-40 tok/s.
- Principio: la app hace el trabajo determinista; el modelo solo redacta.
- Idioma de trabajo: español. Cada entrega lleva CONTEXTO.md actualizado y comandos git exactos.

## Fases 7-11 (resumen; detalle en docs/COMANDOS.md y docs/MEJORAS.md)
- 7: mapa de repo, `@zip`, `@zip:ruta[:A-B]`, `@zip:ruta#func`, `@zip?pal+pal`, bloques BUSCAR/REEMPLAZAR (aider), agente `/auto`, explorador de archivos, F-keys, ratón.
- 8-11: workspace/proyectos, libro de progreso, bintools, `/errores` (buildlog), `/deshacer` (undo.c), `/trucos`, caché de API, partidas guardadas (`?=num`, `~`, `pc/savetool.py`, docs/PARTIDAS.md).

## Fase 12 (esta entrega)
- `chat/verify.c/.h` NUEVO: comprobador estructural conservador (JSON, YAML/workflows de GitHub, Python, shell, familia C, Markdown). Validado contra corpus reales para evitar falsos positivos. No cubre Zig/Rust/Go.
- Agente: tras aplicar/guardar, verifica lo escrito (antes vs después) y avisa al modelo de los PROBLEMS para que corrija.
- `/verificar [archivo]`: comprueba archivos de la carpeta de salida o del proyecto.
- `@zip!nombre`: todos los usos de un identificador (`repo_refs`). Útil para "quién llama a esto".
- Mapa del repo ahora empieza con líneas `Project:` / `CI:` (lenguajes %, ficheros de build, nº tests, hasta 3 workflows).
- `/errores` reconoce errores de workflows (`.github/workflows/x.yml#L12`, `Line: N`, `on line N`).
- Prompt nuevo `docs/garlic_system.txt` (3858 bytes, límite <4095): 6 pasos de trabajo, sección workflows, reglas, partidas/binarios.
  **IMPORTANTE:** cópialo por FTP (2121) a `/data/garlic_system.txt`; ese archivo pisa al prompt incorporado.
- main.c: `sys_ids[2304]`, margen `tok_free` ajustado. `undo.h`: `UNDO_DIR` sobrescribible.
- Integrados tus archivos de partidas (fase 11) sin conflictos: api_run.inc, api_run_test.c, patch_test.c, PARTIDAS.md, savetool.py, savetool_test.py.

## Probado en PC (gcc + ASan/UBSan)
`bash tests/host_tests.sh` pasa completo: patch, progress, workspace, dev, filepick, dev9 (agente/verificar/deshacer), 57/64/199 checks con 0 fallos, savetool, verify_test.

## NO probado en consola ni en CI (probar en este orden)
1. Que el CI compile (main.c, ui.c, webcmd.c solo se revisaron con cabeceras stub).
2. Copiar el prompt a /data y comprobar que arranca y responde.
3. `@zip` + `/verificar` + `@zip!nombre` con un repo pequeño.
4. `/auto` con un cambio de 1 archivo y mirar si avisa de PROBLEMS.
5. Ratón, F-keys, `/gh zip`, selector de archivos (nunca verificados en consola).
- Límites: el 9B sigue fallando en cambios multi-archivo y en copiar SEARCH exacto; el prefill limita el contexto útil.
- Abierto: error `0x80431073` en `/abrir` (netget_http.c no se tocó).

## Git (desde la raíz del repo, tras descomprimir encima)
    git add -A
    git commit -m "Fase 12: verificador, @zip!nombre, mapa con Project/CI, errores de workflows"
    git push

## Fase 13: perfiles (programador / narrador / personaje) para el chat y la API
Pedido: que la IA sea mucho más libre para rolear y generar diálogos de juegos de texto por la API, con perfiles de personalidad, sin perder la programación.
Hallazgo: la API no inyecta ningún prompt propio (usa lo que manda el juego); el prompt de programación solo se usa en el chat. Así que la "permisividad" se resuelve con perfiles de prompt + muestreo, sin tocar el prompt de programación.
- `chat/profiles.c/h` (nuevo, C puro): perfiles incluidos (`programador`, `narrador`, `personaje`, `personaje-json`) y propios en `/data/garlic_profiles/NOMBRE.txt` (cabecera `desc=`, `temperature=`, `max_tokens=`, `stop=`, línea `---`, prompt). `profile_find/list/from_model/api_default`, `profile_is_cmd`.
- `main.c`: `/perfil` (lista), `/perfil NOMBRE` (chat nuevo con ese prompt; fija la temperatura), `/perfil off`. Con perfil activo no se añade el bloque del proyecto y el agente `/auto` no actúa.
- `api_run.inc`: el perfil lo nombra el campo `model` (`garlic-9b:narrador` o `narrador`) o `/data/garlic_api_profile.txt`; su texto va primero y se fusiona con el mensaje de sistema del juego en UN solo mensaje; temperatura y largo del perfil solo si la petición no los trae (`api_gen_given` en `api_compat.c`); prompts crudos: solo muestreo.
- Pruebas: `tests/profiles_test.c` y `test_profile` en `api_run_test.c` (219 comprobaciones). Compila todo el fork con el toolchain real (36 fuentes). NO probado en consola.
- Docs: `docs/PERFILES.md`. El muestreo del motor sigue fijo (temperatura + min-p 0.05).
- Pendiente de otras fases: análisis de binarios grandes (la partida `CUSA57220`, EA FC 27 carrera, `DATA` de 15,7 MB > `BIN_MAX` 6 MB), `/calc`, búsqueda de patrones de bytes, aviso si el agente repite el mismo bloque fallido.

## Fase 13 (sesión 20-nov-2026): guardado de contexto en disco y partidas por bits
**Todo compila, enlaza y empaqueta con el toolchain real en Linux (Zig 0.13.0 + OpenOrbis v0.5.4) y `tests/host_tests.sh` pasa. NADA de esto se ha probado en la consola.**

### Guardado del contexto en disco (`chat/ctxcache.c/.h`)
- Una conversación nueva que empieza con archivos (`@zip`, mapa...) tarda minutos en leerlos (prefill ~65 tok/s). `start_turn` (main.c) toma los tokens anteriores a la pregunta (`pre_n`); si hay un snapshot con el mismo hash de ids lo restaura (KV TurboQuant + estados DeltaNet, ~116 MiB) y solo lee la pregunta; si no, lee el prefijo solo y lo guarda en `/data/garlic_cache/<clave>.kv` (máx. 3, el menos usado se borra). Mínimo 1200 tokens. `/data/garlic_cache.txt` con `0` lo apaga.
- La cabeza MTP no se guarda: `engine_spec_follow(pos)` la hace seguir desde ahí (como tras la voz).
- Riesgo por verificar en la consola: la CPU lee memoria Garlic; `ctxcache_save` mide 256 KiB antes y no guarda si va a < 15 MiB/s (queda en el log: "ctx cache: not saved..."). Las líneas `ctx cache: saved/restored ... in X s` dan los tiempos reales.

### Archivos grandes y partidas (`chat/bitscan.c/.h`, `zipread.c` streaming, `bintools.c`)
- Causa de que la IA no encontrara nada en la partida de EA FC: `zip_read` solo descomprimía los primeros 128 KB comprimidos (~0,5 MB de 15,7) y la búsqueda era por bytes. Ahora `zip_stream`/`zip_extract` descomprimen cualquier tamaño en flujo (ventana de 32 KB) a un archivo en `/data/garlic_files/raw/` (caché, máx. 6) y todo se hace con pread. Probado contra zlib (casos sintéticos, bloques stored, parada anticipada) y con la partida real byte a byte.
- `@zip:archivo` (>192 KB): informe de TODO el archivo (zonas por entropía, textos, firmas, veredicto). La partida real: no está cifrada (4 de 97 bloques parecen aleatorios; el inicio aleatorio no significa nada).
- `@zip:archivo?bits=NUM[+0xOFFSET]` busca el número en cada bit, en los dos órdenes (LSB-first/MSB-first) y dice cuántos ceros pueden ser del mismo campo. Un NUM de 17 bits tiene cientos de falsos positivos: solo sirve solo para números grandes o con `+OFFSET`.
- `@antes.zip~despues.zip[?VIEJO/NUEVO]`: cada zona cambiada leída como campo de bits; con los valores que mostró el juego da `FIELD FOUND ... byte 0x2000 bit 3, 17 bits: 100822 -> 77777`. (En un @token no caben `@`, `>` ni `,`: por eso `+` y `/`.)
- `/pokebits OFFSET:BIT ANCHO VALOR lsb|msb ARCHIVO [zip]` (y desde la respuesta con `/aplicar`) crea `nombre-mod.zip` en flujo, lo relee y compara; nunca toca el original. `/poke` ya funciona en entradas > 6 MB.
- `/checksum OFFSET ARCHIVO [zip]`: prueba CRC32, Adler32 y sumas sobre 5 rangos y dice si coincide con los 4 bytes de ese offset. Aún no recalcula el valor al editar.
- Pruebas: `tests/bits_test.py` (partida sintética de 9 MB con campo de 17 y 25 bits, CRC32, 2 órdenes, parches, original intacto, cruce de bloque de 1 MB) y `tests/ctxcache_test.c`.
- Pendiente / no verificado: que el juego acepte una partida editada (puede haber checksum o firma); el tiempo de descompresión en la PS4 (el decodificador lee bit a bit; la primera vez de un archivo de 15 MB puede tardar varios segundos, luego queda en caché).

### Git
```
git add -A
git commit -m "Fase 13: guardado de contexto en disco y partidas por bits (zip en flujo, ?bits=, diff por bits, /pokebits, /checksum)"
git push
```

