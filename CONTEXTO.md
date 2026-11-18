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
