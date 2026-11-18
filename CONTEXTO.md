# CONTEXTO — Garlic LLM en PS4 (fase 7: IA de programación con repos)

Para continuar en otro chat: pega este archivo y el zip del repo. Idioma de trabajo: español. Entregar siempre CONTEXTO.md actualizado (también suelto) y comandos git exactos; decir con honestidad qué NO se probó en la consola.

## Entorno
- PS4 Slim, fw 13.52, GoldHEN 2.4b18.12. Repo fork `Sackso_garlic-llm_Testing`. Garlic LLM corre Qwen3.5-9B en la GPU de la PS4.
- Compila GitHub Actions (Zig 0.13 + OpenOrbis); `build_chat.py` compila todo `chat/*.c`. Los .c nuevos entran solos.
- Velocidades: prefill ~60-70 tok/s (el cuello de botella: 10K tokens ≈ 2.5 min), decode ~30-37 tok/s. Contexto 16384 tokens (KV TurboQuant). "Pensar" hasta 1024 tokens.

## Qué trae la fase 7
La app hace el trabajo determinista (mapa, búsqueda, aplicar cambios); el modelo solo escribe.
- `chat/repomap.c/h`: mapa del repo (archivo, tamaño, líneas, funciones con línea; carpetas vendor plegadas), búsqueda por palabras con sinónimos ES→EN, rangos de líneas y `#función`.
- `chat/files.c/h`: `@zip` = mapa + README + 1 workflow; `@zip:ruta`, `@zip:ruta:A-B`, `@zip:ruta#func`, `@zip?pal+bras`. Hasta ~40 KB por mensaje (según contexto libre). Si corta, dice cómo pedir el resto.
- `chat/patch.c/h`: bloques estilo Aider `Archivo: ruta / <<<<<<< BUSCAR / ======= / >>>>>>> REEMPLAZAR` (acepta SEARCH/REPLACE). Coincidencia exacta, sin espacios finales o con otra sangría; pista del texto más parecido si falla.
- `chat/devcmd.c/h`: `/proyecto /mapa /grep /aplicar /empaquetar /auto /limite`; agente (`/auto on [N]`): tras cada respuesta aplica los cambios en `/data/garlic_files/out/` (nunca toca el zip; log `CAMBIOS.txt`) o hace el `/ok` solo; si un bloque falla se lo dice al modelo. Estado en `/data/garlic_dev.txt`. Prompt de programación integrado si no hay `/data/garlic_system.txt`.
- `chat/zipwrite.c/h`: `/empaquetar` crea un zip (stored) de `out/` para sacarlo por FTP.
- `chat/outfiles.c`: `/guardar` ya no guarda líneas "Archivo:" como contenido ni bloques con BUSCAR/REEMPLAZAR; exporta `outfiles_write/path_ok`.
- `chat/webcmd.c`: `/gh usuario/repo zip` baja el repo (≤8 MB, vía `api.github.com/.../zipball`) y lo deja como proyecto; `webcmd_net_on/set`; ayuda actualizada. La red sigue apagada salvo `/red on`.
- `chat/main.c`: ctx 40 KB, `ids[15000]`, presupuesto dinámico (tokens libres × 2.7 bytes), guarda contra bucle al reiniciar por contexto lleno, gancho del agente, atajos y ratón.
- `chat/ui.c/h`: chips AUTO / RED / nombre del proyecto en la cabecera.
- Teclado/ratón: tecla o clic izquierdo = escribir; clic derecho = parar (o abrir el explorador en reposo); Esc = parar; F1 ayuda, F2 agente, F3 red, F4 empaquetar, Ctrl+T pensar, Ctrl+N chat nuevo.
- `chat/filepick.c/h` + `draw_filepick` (ui.c): explorador de archivos (F5, clic derecho en reposo o `/archivos`): carpetas, dentro de zips, marcar hasta 8, devuelve `@a @out/x.c @repo.zip:chat/main.c ` al cuadro de texto. Ratón con puntero propio (el estado de `input_mouse` no está verificado en consola), teclado USB y mando.
- `tests/host_tests.sh` (+ `patch_test.c`, `dev_test.c`): pruebas en PC con ASan/UBSan; el workflow las corre sin bloquear el build.

## Probado en PC (gcc + ASan/UBSan, con el zip real del repo)
Mapa, `#función`, rangos, búsqueda, aplicar/fallar bloques, `/empaquetar` (`unzip -t` ok), `/auto`, `/limite`, outfiles. Los .c de la consola (main/ui) solo se revisaron por sintaxis con cabeceras vacías.

## NO probado en la consola real (probar en este orden)
1. Que compile en Actions (main.c/ui.c/webcmd.c no se compilaron con el toolchain).
2. `/proyecto`, `/mapa`, `@zip` y ver el tiempo de prefill con un mapa de ~3K tokens.
3. Una petición real de cambio + `/aplicar` y `/empaquetar`.
4. `/auto on`: ¿hace solo el `/ok` y aplica? Vigilar que no entre en bucle (límite de rondas).
5. Clic de ratón (izq. abre teclado, der. para) y F1–F4: el estado de botones de `input_mouse` no se verificó.
6. Explorador: F5 / clic derecho / `/archivos`; ¿se mueve el puntero?, ¿el clic marca?, ¿el texto queda en el teclado?
7. `/gh usuario/repo zip` (descarga de hasta 8 MB con `net_get`, no probada con binarios).
Límites conocidos: el prefill manda: ~10K tokens tardan ~2.5 min; tras un reinicio por contexto lleno el mapa no se reenvía solo; no hay botones clicables en pantalla (solo atajos); sigue sin resolver el error `send 0x80431073` de `/abrir` en github.com/wikipedia (no se tocó `netget_http.c`).

## Git (desde la raíz del repo, tras descomprimir encima)
```
git add -A
git commit -m "Fase 7: mapa de repo, busqueda, cambios BUSCAR/REEMPLAZAR, agente /auto, ratón y teclado"
git push
```

## Sesión 18-nov-2026: lo que mostró el log de la consola y qué se arregló
Log real (`ps4ai_log.txt`): el agente ya corre en la PS4, pero **desperdició sus rondas** por fallos de las herramientas, no del modelo:
1. `@zip:chat/engine.c#mm_tune` devolvía solo 55 bytes: `repo_symbol_range` se quedaba con la primera coincidencia, que es la declaración adelantada (`static void mm_tune(Engine* e);`, línea 52) y no la definición (línea 1233). Arreglado en `repomap.c`: si la coincidencia no tiene cuerpo, busca la siguiente que sí (hasta 12). Ahora devuelve las líneas 1233-1296.
2. `#s3_mish` / `#t3_silu`: el modelo adivinó mal el archivo (están en `shaders/s3gen.cl` y `shaders/t3.cl`) y la app solo decía "no such function", así que el modelo repetía la misma petición. Arreglado en `files.c`: si el símbolo no está en ese archivo, busca el nombre en todo el repo y manda esos trozos con una línea que lo explica.
3. La nota "cut to the first N bytes" salía también para funciones completas (`size` es el del archivo). Arreglado (`!g_att_a`).
4. `netget_http.c`: añadido `sceHttpSetResponseHeaderMaxSize(tmpl, 128 KB)` (opcional, por `dlsym`). Según el CONTEXTO del psbrowser evita `send 0x80431073` en Wikipedia; es la causa más probable del fallo de `/abrir` y `/gh` con github.com. **No probado en la consola.**
Verificación en PC: ASan/UBSan con el zip real del fork (`mm_tune` 1233-1296; `s3_mish` se resuelve a `shaders/s3gen.cl`), `tests/host_tests.sh` pasa, y **todo el fork compila, enlaza y empaqueta con el toolchain real** (Zig 0.13.0 + OpenOrbis v0.5.4, en Linux): `main.c`, `ui.c` y `webcmd.c` ya compilan.

## Contexto de 16K: números del log y plan (nada de esto está implementado)
- Memoria GPU: `used 4119 of 4512 MiB` con el LLM (pesos + KV 16K) y `4494 of 4512` con la voz (T3 ~320 MiB) y el MTP cargados. KV TurboQuant 4-bit: **66 MiB por cada 8192 posiciones** (8 capas de atención completa; el resto son capas DeltaNet de estado fijo).
- Sin voz (modo desarrollo) quedarían libres ~390 MiB: cabe el MTP (234 MiB, hoy prestado de T3) y 1-2 bloques extra de 8K, o sea **24K-32K** de contexto (estimación).
- El tope de 16K está fijado en los kernels: `TQ_AT` en `shaders/attn_tq.h` solo distingue 2 bloques (`p >= TQ_SEQ ? c + split : c`), y `ATT_MAX_BLOCKS`/`attn_part` están dimensionados para 256 bloques de 64. Pasar a N bloques toca `attn_tq.h`, `attn.h`, `prefill.h`, `spec.h` y `engine.c` (los kernels se compilan en el CI, no hay binario fijo). La numérica solo se puede comprobar en la consola.
- Decode: leer 32K de KV por token añade ~1.5 ms a ~30 ms (estimación, hay que medirlo). **El coste real es el prefill**: ~65 tok/s, así que llenar 32K tarda ~8 min. Por eso el contexto grande solo sirve si no se vuelve a calcular: (a) guardar en `/data` el estado del prefijo (KV + ~50 MiB de estados DeltaNet, ya hay copia de estado para "pensar") y restaurarlo en segundos; (b) compactar el historial con un resumen al acercarse al límite.
- Orden propuesto: modo desarrollo sin voz con interruptor en `/data` (16K o 32K) -> kernels de N bloques -> snapshot del prefijo -> compactación.

## Git
```
git add -A
git commit -m "Arreglos del agente: #funcion prefiere la definicion, busqueda en todo el repo si falla, nota de corte, cabeceras HTTP 128KB"
git push
```

