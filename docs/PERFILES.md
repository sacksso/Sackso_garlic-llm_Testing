# Perfiles (fase 13): programador, narrador, personaje

Un perfil es un nombre para una forma de responder: un prompt de sistema y, si quieres, la temperatura y el largo que le van bien. La misma consola sirve así para programar, para narrar una aventura de texto o para dar voz a un personaje de tu juego, sin cambiar de modelo. **Lo de programar no cambia**: sin perfil, el chat usa el prompt de programación de siempre (y tu `/data/garlic_system.txt` si existe).

## Perfiles incluidos
| Nombre | Para qué | Temp. | Largo |
|---|---|---|---|
| `programador` | respuestas exactas y breves | 0.3 | – |
| `narrador` | aventuras y rol de texto; ficción sin sermones ni avisos; no decide por el jugador | 0.9 | 260 |
| `personaje` | una línea de diálogo en la voz del personaje de la ficha del juego | 0.9 | 120 |
| `personaje-json` | igual, pero responde `{"texto":"...","emocion":"..."}` | 0.8 | 160 |

Los de ficción tratan temas oscuros (violencia, miedo, traición, lenguaje fuerte) con naturalidad y no salen del papel; el romance se sugiere, sin escenas sexuales explícitas. Si quieres otro tono, haz uno propio (abajo).

## En el chat de la PS4
- `/perfil` lista los perfiles y dice cuál está activo.
- `/perfil narrador` empieza un chat nuevo con ese perfil (el prompt cambia, así que no se mezcla con lo anterior). L2/R2 siguen cambiando la temperatura.
- `/perfil off` (o `/perfil programador`) vuelve al prompt de programación.
- Con un perfil de ficción activo el agente `/auto` no actúa; al volver con `/perfil off` sigue como antes.

## En la API (juegos y apps)
El perfil se elige de dos formas:
1. **Por el nombre del modelo** que manda el juego: `"model": "garlic-9b:narrador"` o solo `"narrador"` (sirve cualquier cosa antes de `:` o `/`). Un nombre que no es perfil se ignora y todo funciona como antes.
2. **Por defecto**: escribe el nombre en `/data/garlic_api_profile.txt` (una línea, p. ej. `personaje`). Se usa cuando la petición no nombra ninguno. Útil si el juego no deja cambiar el nombre del modelo.

Cómo se combina con lo que manda el juego: el texto del perfil va **primero** en el mensaje de sistema y el mensaje de sistema del juego (la ficha del personaje, las reglas del mundo) va justo después, en el mismo mensaje. El juego decide los detalles y el perfil la manera. La temperatura y el largo del perfil se usan solo si la petición no trae `temperature` / `max_tokens` (`max_length` en Kobold, `num_predict` en Ollama). Con prompts en texto crudo (OpenAI `/v1/completions`, KoboldCpp, Ollama con `raw`) no hay mensaje de sistema donde ponerlo: solo se aplica el muestreo.

Ejemplo (OpenAI chat; sirve igual en Ollama `/api/chat`):
```json
{
  "model": "garlic-9b:personaje",
  "messages": [
    {"role": "system", "content": "Eres Marta, 24 años, panadera. Seca, irónica, leal. Está harta de que el jugador llegue tarde. Lleva tres días sin dormir."},
    {"role": "user", "content": "Perdona el retraso, Marta..."}
  ]
}
```
Consejos para diálogos de juegos:
- Manda siempre la **misma ficha** al principio y detrás las últimas 6-10 líneas: con `/cache` activado la consola reutiliza el principio ya leído y responde mucho antes.
- Pide poco (`max_tokens` 80-120) y, si tu juego lo necesita, usa `personaje-json` y lee `texto` y `emocion`.
- Una ficha de 5-8 líneas basta: personalidad, cómo habla, qué quiere, qué sabe, qué siente ahora.

## Perfiles propios
Un archivo `/data/garlic_profiles/NOMBRE.txt` (nombre en minúsculas: letras, cifras, `-` y `_`). Si se llama como uno incluido, lo sustituye.
```
# comentarios
desc=Marta, la panadera (se ve en /perfil)
temperature=1.0
max_tokens=100
stop=\nJugador:
---
Eres Marta, ... (todo el prompt; unos 3,5 KB como máximo)
```
`stop` se puede repetir dos veces (`\n` es un salto de línea). Sin línea `---`, todo el archivo es el prompt. El perfil se lee en cada petición/cada `/perfil`: se edita por FTP sin reiniciar.

## Límites
- Es un modelo de 9B: el prompt quita los avisos y sermones y le da carácter, pero no puede cambiar lo que el modelo sabe hacer; algunas peticiones las seguirá evitando.
- El muestreo de la consola es temperatura + min-p 0,05 fijos: no hay `top_p`, `top_k` ni penalización de repetición (cambiarlo exige tocar el kernel de la GPU).
- Probado: compila con el toolchain real y pasa las pruebas de PC (incluida la API con perfiles). Aún no probado en la consola.
