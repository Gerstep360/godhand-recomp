"""Build the PC scene assets from a local God Hand USA installation."""

import argparse
import json
from pathlib import Path
import shutil
import struct
import subprocess
import sys
from tempfile import TemporaryDirectory

from pack_room import pack_room


PORT = Path(__file__).resolve().parents[1]
STUDIO = PORT.parent / 'godhand-extras' / 'studio'
DEFAULT_DATA = PORT / 'build' / 'port' / 'bin' / 'game_data' / 'disc'
DEFAULT_ASSETS = PORT / 'build' / 'native' / 'bin' / 'pc_assets'


def sanitize_motion_glb(glb_path: Path):
    """Filter out PS2 IK effector translations on child limb bones (e.g. ankles).
    God Hand MOT files encode foot IK goals that dislocate knees/legs when played
    back as raw forward-kinematics translations."""
    data = glb_path.read_bytes()
    if len(data) < 20:
        return
    magic, version, length = struct.unpack('<III', data[:12])
    chunk_len, chunk_type = struct.unpack('<II', data[12:20])
    gltf = json.loads(data[20:20 + chunk_len].decode('utf-8'))
    bin_data = data[20 + chunk_len + 8:]

    modified = False
    for anim in gltf.get('animations', []):
        new_channels = []
        for ch in anim.get('channels', []):
            node = ch['target'].get('node')
            path = ch['target'].get('path')
            # Only root (1) and pelvis (2) should have translation tracks.
            if path == 'translation' and node not in (1, 2):
                modified = True
                continue
            new_channels.append(ch)
        anim['channels'] = new_channels

    if modified:
        new_json_bytes = json.dumps(gltf).encode('utf-8')
        while len(new_json_bytes) % 4 != 0:
            new_json_bytes += b' '
        new_total_len = 12 + 8 + len(new_json_bytes) + 8 + len(bin_data)
        header = struct.pack('<III', magic, version, new_total_len)
        json_chunk = struct.pack('<II', len(new_json_bytes), 0x4E4F534A) + new_json_bytes
        bin_chunk = struct.pack('<II', len(bin_data), 0x004E4942) + bin_data
        glb_path.write_bytes(header + json_chunk + bin_chunk)


def prepare(data: Path, output: Path):
    sys.path.insert(0, str(STUDIO))
    from backend.assets import Catalog
    from backend.conversion import preview

    ffmpeg = shutil.which('ffmpeg')
    if not ffmpeg:
        raise RuntimeError('Se requiere FFmpeg para convertir la pista ADX de la instalacion local.')

    catalog = Catalog(data)
    player = catalog.index['afs-1053.1']
    motion = catalog.index['afs-1053.21']  # Gene's canonical walk animation (57 frames)
    room = [item for item in catalog.children['afs-1156.3'] if item.tag == 'SCR']
    if not room:
        raise ValueError('El paquete r100 no contiene geometria SCR.')

    output.parent.mkdir(parents=True, exist_ok=True)
    with TemporaryDirectory(prefix='prepare-pc-', dir=output.parent) as work:
        staging = Path(work)
        shutil.copyfile(preview(catalog, player, motion), staging / 'player.glb')
        sanitize_motion_glb(staging / 'player.glb')
        pack_room([preview(catalog, item) for item in room], staging / 'room.glb')

        # Use the clean Stage 1-1 music (Smoking Roll) from user's audio pack or afs-0066.2
        wav_003 = output / 'PS2 Audio' / 'GODHAND (SLUS-21503)' / '_DATA.AFS' / '003.wav'
        if wav_003.is_file():
            music_source = wav_003
        elif 'afs-0066.2' in catalog.index:
            music_source = catalog.extract(catalog.index['afs-0066.2'], staging / 'music.adx')
        else:
            music_source = catalog.extract(catalog.index['afs-0066.0'], staging / 'music.adx')

        subprocess.run([ffmpeg, '-hide_banner', '-nostdin', '-loglevel', 'error', '-y', '-i', str(music_source),
                        '-map', '0:a:0', '-c:a', 'libvorbis', '-q:a', '6', str(staging / 'music.ogg')],
                       check=True)

        output.mkdir(exist_ok=True)
        for name in ('player.glb', 'music.ogg', 'room.glb'):
            (staging / name).replace(output / name)
        manifest = staging / 'scene.txt'
        manifest.write_text('GH-PC-SCENE-1\nroom.glb\n', encoding='ascii')
        manifest.replace(output / 'scene.txt')
    print(f'Preparados Gene, una animacion, {len(room)} piezas del escenario y una pista estereo en {output}')


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--data', type=Path, default=DEFAULT_DATA)
    parser.add_argument('--output', type=Path, default=DEFAULT_ASSETS)
    args = parser.parse_args()
    try:
        prepare(args.data.resolve(strict=True), args.output.resolve())
    except (ImportError, KeyError, OSError, RuntimeError, ValueError, subprocess.CalledProcessError) as error:
        parser.exit(1, f'{error}\n')


if __name__ == '__main__':
    main()
