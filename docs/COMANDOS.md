# Comandos de Garlic LLM (fork)

Se escriben como un mensaje normal en el chat (teclado de la app, USB o del sistema). Ninguno se envía al modelo.

## Archivos
| Comando | Qué hace |
|---|---|
| `/guardar` o `/save` | Guarda en `/data/garlic_files/out/` los bloques de código con nombre de la última respuesta. |

## Internet (primero `/red on`; apagado por defecto)
| Comando | Qué hace |
|---|---|
| `/red on` · `/red off` · `/red` | Interruptor de red. Se guarda en `/data/garlic_web.txt`. |
| `/web palabras` (`/b`) | Busca en DuckDuckGo y muestra la lista numerada de resultados. |
| `/abrir N` (`/o`) | Lee el resultado N de la última búsqueda. |
| `/wiki palabras` (`/w`) | Introducción del artículo de Wikipedia en español. `/wiki en:palabras` para inglés. |
| `/url https://...` (`/u`) | Una página web como texto. |
| `/gh usuario/repo [ruta]` (`/g`) | Lista de archivos + README del repo; con una ruta, ese archivo; con `ruta/`, esa carpeta. También acepta enlaces de github.com. |

## Que la IA pida archivos (`/ok`)
| Escribes | Resultado |
|---|---|
| `/ok` (o `ok`, `si`, `dale` justo después de una respuesta que pide cosas) | Si la última respuesta de la IA nombra archivos (`@repo.zip:ruta`) o comandos de red (`/gh`, `/url`, `/web`), la app los trae y se los manda a la IA en un solo mensaje. Sin `/ok` no se lee nada. |

## Otros
| Comando | Qué hace |
|---|---|
| `/estado` | Red ON/OFF, backend, texto web pendiente y última búsqueda. |
| `/limpiar` | Descarta lo bajado de internet que aún no se usó. |
| `/ayuda` (`/?`) | Lista los comandos por secciones. |

Una palabra con `/` que no sea ninguno de estos (por ejemplo `/guardr`) responde "Unknown command" en vez de ir al modelo.

Flujo típico de búsqueda: `/red on` → `/web error zig linker` → `/abrir 2` → tu pregunta (el texto bajado viaja con ella).

Mientras baja algo, el chat muestra "Working on it..." y la pantalla espera (límite de unos 30 s).

Certificados HTTPS: el backend intenta primero verificar el servidor; si el handshake falla enseguida, reintenta sin verificar y `/estado` lo indica (`TLS not verified`). Si prefieres que nunca lo haga, crea un archivo vacío `/data/garlic_web_strict.txt`.

Lo que bajan `/gh`, `/url` y `/web` espera y viaja al modelo **con tu siguiente mensaje**. Una copia queda en `/data/garlic_files/web/` (se relee con `@web/nombre.txt`).

## Adjuntar archivos con @
| Escribes | Resultado |
|---|---|
| `@` | Lista los archivos de `/data/garlic_files/` (y de `out/` y `web/`). |
| `@archivo.yml`, `@out/ruta/x.c` | El texto del archivo (cualquier archivo de texto). |
| `@repo.zip` | Lista de archivos del zip (sin imágenes, modelos, etc.) y, debajo, el comienzo del README y de hasta 2 workflows (`.github/workflows`). |
| `@zip`, `@repo`, `@:ruta` | El último zip usado (o el único zip que haya). Ej.: `@:chat/main.c`, `@:*.yml`. |
| `@Sackso` | Basta el comienzo (o un trozo) del nombre si es único: sirve para nombres con espacios o `(1)`. Un error de una letra (`@build-pkh.yml`) se corrige solo y la nota lo dice; si no hay una opción clara, la nota sugiere nombres parecidos. |
| `@repo.zip:chat/` · `@repo.zip:*.yml` | Solo una parte de la lista. `*.bin` muestra los binarios. |
| `@repo.zip:ruta/archivo` | Un archivo del zip. Sirve la ruta completa, sin la carpeta `repo-main/`, o solo el nombre si es único. |
| `@doc.docx` `.pptx` `.xlsx` `.odt` `.ods` `.odp` | El texto del documento (xlsx: celdas separadas por tabulador). |

Límites: 14 000 bytes por mensaje (unos 4-5K tokens) y 8 archivos. Si no cabe, se corta y la nota lo dice. Sin soporte: zip64, zips cifrados, PDF.

## Qué buscar en `ps4ai_log.txt`
Cada orden deja una línea: `save: ...`, `web: ... (N s)`, `files: ... (N s)` y `web text attached: N bytes`, seguidas de las líneas `prompt of N tokens in X s` de siempre.
