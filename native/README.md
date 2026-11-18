# Escena nativa de God Hand PC

`godhand-native.exe` abre el escenario `r100` y a Gene con un clip del juego. Lee mallas, esqueleto y animación de GLB, dibuja con OpenGL y reproduce una pista estéreo mediante WASAPI en Windows. `godhand_pc_media` es la biblioteca de modelos y audio que usa el ejecutable. Solo enlaza raylib y las bibliotecas de plataforma.

Desde el workspace, abre `run_native.cmd`. Usa **WASD** para mover a Gene, **← / →** para girar la cámara, **Espacio** para pausar la animación y **M** para silenciar la música.

La escena usa movimiento libre y un clip en bucle. Los volúmenes de sombra del escenario requieren un paso de composición propio. El combate, la IA, las transiciones y los menús del juego siguen pendientes de integrar con esta biblioteca.

## Compilar y preparar los recursos

Desde `godhand-recomp`, con MSVC y Ninja disponibles:

```powershell
cmake -S native -B build/native -G Ninja -DCMAKE_BUILD_TYPE=Release
cmake --build build/native --target godhand-native
python native/prepare_assets.py
```

CMake reutiliza el checkout de raylib 5.5 de `build/port/_deps/raylib-src`. Usa `-DGODHAND_RAYLIB_SOURCE_DIR=<ruta>` para indicar un checkout independiente. La compilación no necesita el ELF, las fuentes generadas ni las bibliotecas del runtime de PS2.

La preparación reutiliza el catálogo de Studio, `ghconvert.exe` y FFmpeg. Usa los archivos instalados en `build/port/bin/game_data/disc` o el directorio indicado con `--data`. Empaqueta las piezas del escenario con un solo banco de texturas y convierte una pista ADX a OGG estéreo. El ejecutable abre únicamente el paquete PC de `build/native/bin/pc_assets`. Los recursos se generan localmente desde la copia del usuario.

Para una prueba sin ventana visible:

```powershell
.\build\native\bin\godhand-native.exe --hidden --frames 240 --capture native-scene.png
```
