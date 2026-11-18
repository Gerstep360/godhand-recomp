"""Extract the complete God Hand _DATA.AFS table directly from a local ISO."""

from __future__ import annotations

import argparse
import hashlib
import json
from pathlib import Path
import re
import struct
from tempfile import TemporaryDirectory

from snr2disc import Disc


ROOT = Path(__file__).resolve().parents[2]
DEFAULT_ISO = ROOT / 'game' / 'God Hand (USA).iso'
DEFAULT_OUT = ROOT / 'build' / 'assets' / 'data-afs'


def extract(iso: Path, output: Path) -> None:
    if output.exists():
        raise FileExistsError(f'La carpeta de destino ya existe: {output}')
    output.parent.mkdir(parents=True, exist_ok=True)

    disc = Disc(str(iso))
    try:
        names = {}
        destinations = set()
        for name, index in disc.fst_files():
            if index >= len(disc.afs_entries):
                raise ValueError(f'Índice FST fuera de _DATA.AFS: {index}')
            if any(not re.fullmatch(r'[a-zA-Z0-9_-][a-zA-Z0-9_.-]*', part) or part.endswith('.')
                   for part in name.split('/')):
                raise ValueError(f'Ruta FST inválida: {name!r}')
            key = name.casefold()
            if key in destinations:
                raise ValueError(f'Ruta FST repetida: {name}')
            destinations.add(key)
            names.setdefault(index, []).append(name)

        table_end = 8 + 8 * len(disc.afs_entries)
        for index, (offset, size) in enumerate(disc.afs_entries):
            if size and (offset < table_end or offset + size > disc.afs_size):
                raise ValueError(f'Entrada {index} fuera de los límites de _DATA.AFS.')

        manifest = []
        with TemporaryDirectory(prefix='extract-afs-', dir=output.parent) as temp:
            staging = Path(temp) / 'data'
            staging.mkdir()
            for index, (offset, size) in enumerate(disc.afs_entries):
                paths = names.get(index, [])
                relative = paths[0] if paths else f'_unnamed/{index:04}.bin'
                entry = {'index': index, 'offset': offset, 'size': size, 'fst_paths': paths,
                         'file': None, 'sha256': None}
                if size:
                    destination = staging / relative
                    destination.parent.mkdir(parents=True, exist_ok=True)
                    digest = hashlib.sha256()
                    disc.f.seek(disc.afs_base + offset)
                    left = size
                    with destination.open('xb') as file:
                        while left:
                            block = disc.f.read(min(left, 4 * 1024 * 1024))
                            if not block:
                                raise EOFError(f'ISO truncada en la entrada {index}.')
                            digest.update(block)
                            file.write(block)
                            left -= len(block)
                    entry.update(file=relative, sha256=digest.hexdigest())
                manifest.append(entry)
                if (index + 1) % 250 == 0:
                    print(f'{index + 1} de {len(disc.afs_entries)} entradas', flush=True)

            (staging / 'GODHAND.FST').write_bytes(disc.read_root_file('GODHAND.FST'))
            (staging / 'manifest.json').write_text(
                json.dumps({'archive': '_DATA.AFS', 'entries': manifest}, indent=2) + '\n', encoding='utf-8')
            staging.rename(output)
    finally:
        disc.close()

    populated = sum(entry['file'] is not None for entry in manifest)
    total = sum(entry['size'] for entry in manifest)
    print(f'{populated} archivos, {len(manifest) - populated} entradas vacías y {total:,} bytes en {output}')


def main() -> None:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--iso', type=Path, default=DEFAULT_ISO)
    parser.add_argument('--out', type=Path, default=DEFAULT_OUT)
    args = parser.parse_args()
    try:
        extract(args.iso.resolve(strict=True), args.out.resolve())
    except (OSError, ValueError, EOFError, KeyError, struct.error) as error:
        parser.exit(1, f'{error}\n')


if __name__ == '__main__':
    main()
