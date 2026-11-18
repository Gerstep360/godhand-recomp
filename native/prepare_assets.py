"""Build the PC scene assets from a local God Hand USA installation."""

import argparse
from pathlib import Path
import shutil
import subprocess
import sys
from tempfile import TemporaryDirectory

from gene_rig import build_player
from pack_room import pack_room


PORT = Path(__file__).resolve().parents[1]
STUDIO = PORT.parent / 'godhand-extras' / 'studio'
DEFAULT_DATA = PORT / 'build' / 'port' / 'bin' / 'game_data' / 'disc'
DEFAULT_ASSETS = PORT / 'build' / 'native' / 'bin' / 'pc_assets'
DEFAULT_GENE = PORT.parent / 'UNPACK_pl00.dat' / 'FILES'


def prepare(data: Path, output: Path, gene: Path, player_only: bool):
    sys.path.insert(0, str(STUDIO))
    from backend.assets import Catalog
    from backend.conversion import converter_path, preview

    catalog = Catalog(data) if not player_only or not gene.is_dir() else None
    output.parent.mkdir(parents=True, exist_ok=True)
    with TemporaryDirectory(prefix='prepare-pc-', dir=output.parent) as work:
        staging = Path(work)
        aliases = {'MOT_009': 'idle', 'MOT_021': 'walk', 'MOT_025': 'run', 'MOT_032': 'strike'}
        if gene.is_dir():
            model = staging / 'gene.glb'
            subprocess.run([str(converter_path()), '-model', str(gene / 'MD' / 'MD_001.md'),
                            '-textures', str(gene / 'TM3' / 'TM3_000.tm3'), '-out', str(model)], check=True)
            sources = [(path.stem, path.read_bytes()) for path in sorted((gene / 'MOT').glob('MOT_*.mot'))]
        else:
            model = preview(catalog, catalog.index['afs-1053.1'])
            sources = [(f"MOT_{int(item.id.rsplit('.', 1)[1]):03}",
                        catalog.extract(item, staging / 'source.mot').read_bytes())
                       for item in catalog.children['afs-1053'] if item.tag == 'MOT']
        if not set(aliases).issubset(name for name, _ in sources):
            raise ValueError('Faltan los MOT básicos de Gene.')
        build_player(model, [(aliases.get(name, name), raw) for name, raw in sources], staging / 'player.glb')
        output.mkdir(exist_ok=True)
        if player_only:
            (staging / 'player.glb').replace(output / 'player.glb')
            print(f'Preparados Gene y {len(sources)} MOT en {output}')
            return

        ffmpeg = shutil.which('ffmpeg')
        if not ffmpeg:
            raise RuntimeError('Se requiere FFmpeg para convertir la pista ADX de la instalacion local.')
        room = [item for item in catalog.children['afs-1156.3'] if item.tag == 'SCR']
        if not room:
            raise ValueError('El paquete r100 no contiene geometria SCR.')
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
    print(f'Preparados Gene, {len(sources)} MOT, {len(room)} piezas del escenario y una pista estereo en {output}')


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--data', type=Path, default=DEFAULT_DATA)
    parser.add_argument('--output', type=Path, default=DEFAULT_ASSETS)
    parser.add_argument('--gene-files', type=Path, default=DEFAULT_GENE)
    parser.add_argument('--player-only', action='store_true')
    args = parser.parse_args()
    try:
        if args.gene_files != DEFAULT_GENE and not args.gene_files.is_dir():
            raise ValueError('No se encuentra la carpeta de archivos extraídos de Gene.')
        prepare(args.data.resolve(), args.output.resolve(), args.gene_files.resolve(), args.player_only)
    except (ImportError, KeyError, OSError, RuntimeError, ValueError, subprocess.CalledProcessError) as error:
        parser.exit(1, f'{error}\n')


if __name__ == '__main__':
    main()
