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
