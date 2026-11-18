# Escena nativa de God Hand PC

`godhand-native.exe` abre el escenario `r100` y a Gene con guardia, caminata, carrera y golpe. Lee mallas, esqueleto y animaciones de GLB, dibuja con OpenGL y reproduce una pista estéreo mediante WASAPI en Windows. `godhand_pc_media` es la biblioteca de modelos, colisiones y audio que usa el ejecutable. Solo enlaza raylib y las bibliotecas de plataforma.

Desde el workspace, abre `run_native.cmd`. Usa **WASD** para mover a Gene, **Shift + W** para correr, **ratón** o **flechas** para orientar la cámara, **Espacio** para saltar, **clic izquierdo** para golpear, **Tab** para liberar el ratón y **M** para silenciar la música. El blanco frente al punto de aparición se ilumina cuando detecta contacto con el puño.

El controlador usa gravedad, contacto con el suelo y deslizamiento contra paredes. La cámara al hombro se acerca si hay geometría detrás de Gene. La IA, la campaña y los menús originales siguen pendientes de integrar en esta escena.

Abre el inspector con **F7**. En **Animaciones** puedes buscar los 206 MOT por número y elegir uno con la barra de la lista, **↑ / ↓**, **Re Pág / Av Pág** o **Inicio / Fin**. Usa **← / →** o la barra inferior para recorrer fotogramas y **Espacio** para reproducir o pausar. **Pose base** muestra la postura de la malla antes de animar. Activa **Esqueleto** y desactiva **Malla** para ver los huesos por separado. Usa **A / D** o el botón derecho para girar, y **− / +** para cambiar el zoom.

En **Pesos**, elige un hueso y haz clic en la malla para ver los pesos del vértice. Los botones de ajuste redistribuyen el peso entre sus influencias. **Restaurar** recupera los pesos del vértice seleccionado y **Guardar** escribe los cambios en `pc_assets/gene-weights.txt`.

## Compilar y preparar los recursos

Desde `godhand-recomp`, con MSVC y Ninja disponibles:

```powershell
cmake -S native -B build/native -G Ninja -DCMAKE_BUILD_TYPE=Release
cmake --build build/native --target godhand-native
python native/prepare_assets.py
```

CMake reutiliza el checkout de raylib 5.5 de `build/port/_deps/raylib-src`. Usa `-DGODHAND_RAYLIB_SOURCE_DIR=<ruta>` para indicar un checkout independiente. La compilación no necesita el ELF, las fuentes generadas ni las bibliotecas del runtime de PS2.

La preparación reutiliza el catálogo de Studio, `ghconvert.exe`, NumPy y FFmpeg. Usa los archivos instalados en `build/port/bin/game_data/disc` o el directorio indicado con `--data`. Empaqueta las piezas del escenario y convierte la música a OGG estéreo. Para Gene busca los subdirectorios `MD`, `TM3` y `MOT` en `../UNPACK_pl00.dat/FILES`. Puedes indicar otro directorio con `--gene-files` o regenerar únicamente `player.glb` con `python native/prepare_assets.py --player-only`.

Las piernas conservan las rotaciones originales de los muslos. El conversor usa los objetivos MOT en el pivote de la punta para orientar la tibia y el pie, manteniendo las longitudes del esqueleto. La previsualización reproduce las poses en el sitio. Los eventos originales de `SEQ` requieren integración con el motor de juego. El ejecutable abre únicamente el paquete PC de `build/native/bin/pc_assets`.

## Extraer el archivo de recursos

Desde `godhand-recomp`, ejecuta `python tools/snr2/extract_data_afs.py`. El extractor lee `game/God Hand (USA).iso` y guarda los archivos en `build/assets/data-afs`. Puedes indicar otra ISO con `--iso` y elegir una carpeta nueva con `--out`. Conserva las rutas de `GODHAND.FST`, registra los índices vacíos e incluye el tamaño y SHA-256 de cada archivo en `manifest.json`. Los paquetes DAT y demás contenedores internos se conservan en su formato original.

Como referencia adicional para el disco y la lógica original, el proyecto god-hand-decomp documenta la extracción de módulos y contiene código decompilado de la versión NTSC-U. [god-hand-decomp](https://github.com/LucasPicoli/god-hand-decomp)

Para una prueba sin ventana visible:

```powershell
.\build\native\bin\godhand-native.exe --hidden --frames 240 --capture native-scene.png
```
