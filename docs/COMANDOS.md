# Comandos de Garlic LLM (fork)

Se escriben como un mensaje normal en el chat (teclado de la app, USB o del sistema). Ninguno se envía al modelo.

| Comando | Qué hace |
|---|---|
| `/guardar` o `/save` | Guarda en `/data/garlic_files/out/` los bloques de código con nombre de la última respuesta. |
| `/red on` · `/red off` · `/red` | Interruptor de red (apagado por defecto). Se guarda en `/data/garlic_web.txt`. |
| `/gh usuario/repo` | Lista de archivos + README del repo (también acepta un enlace de github.com). |
| `/gh usuario/repo ruta/archivo` | Un archivo del repo. Con `ruta/` lista esa carpeta. |
| `/url https://...` | Una página web como texto. |
| `/web palabras` | Búsqueda (DuckDuckGo, versión HTML): títulos, enlaces y resúmenes. |
| `/limpiar` | Descarta lo bajado de internet que aún no se usó. |
| `/ayuda` | Lista los comandos. |

Lo que bajan `/gh`, `/url` y `/web` espera y viaja al modelo **con tu siguiente mensaje**. Una copia queda en `/data/garlic_files/web/` (se relee con `@web/nombre.txt`).

## Adjuntar archivos con @
| Escribes | Resultado |
|---|---|
| `@` | Lista los archivos de `/data/garlic_files/` (y de `out/` y `web/`). |
| `@archivo.yml`, `@out/ruta/x.c` | El texto del archivo (cualquier archivo de texto). |
| `@repo.zip` | Lista de archivos del zip (sin imágenes, modelos, etc.). |
| `@repo.zip:chat/` · `@repo.zip:*.yml` | Solo una parte de la lista. `*.bin` muestra los binarios. |
| `@repo.zip:ruta/archivo` | Un archivo del zip. Sirve la ruta completa, sin la carpeta `repo-main/`, o solo el nombre si es único. |
| `@doc.docx` `.pptx` `.xlsx` `.odt` `.ods` `.odp` | El texto del documento (xlsx: celdas separadas por tabulador). |

Límites: 14 000 bytes por mensaje (unos 4-5K tokens) y 8 archivos. Si no cabe, se corta y la nota lo dice. Sin soporte: zip64, zips cifrados, PDF.

## Qué buscar en `ps4ai_log.txt`
Cada orden deja una línea: `save: ...`, `web: ... (N s)`, `files: ... (N s)` y `web text attached: N bytes`, seguidas de las líneas `prompt of N tokens in X s` de siempre.
