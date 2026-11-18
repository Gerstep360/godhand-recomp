"""Pack static scene fragments from the game's shared room texture bank."""

import json
from pathlib import Path
import re
import struct


def read_glb(path: Path):
    data = path.read_bytes()
    if len(data) < 28:
        raise ValueError(f'GLB truncado: {path}')
    magic, version, total, json_size, json_type = struct.unpack_from('<4sIIII', data)
    if (magic, version, total, json_type) != (b'glTF', 2, len(data), 0x4E4F534A):
        raise ValueError(f'GLB invalido: {path}')
    end_json = 20 + json_size
    if end_json + 8 > len(data):
        raise ValueError(f'GLB sin buffer de geometria: {path}')
    length, chunk = struct.unpack_from('<II', data, end_json)
    if chunk != 0x004E4942 or end_json + 8 + length != len(data):
        raise ValueError(f'GLB sin buffer de geometria: {path}')
    return json.loads(data[20:end_json]), memoryview(data)[end_json + 8:]


def pack_room(parts: list[Path], destination: Path):
    room = {'asset': {'version': '2.0'}, 'scene': 0, 'scenes': [{'nodes': []}],
            'nodes': [], 'meshes': [], 'accessors': [], 'bufferViews': []}
    storage = bytearray()
    image_bank = []

    for index, part in enumerate(parts):
        model, binary = read_glb(part)
        if model.get('animations'):
            raise ValueError('La geometria del escenario debe estar en reposo.')
        views = {}
        accessors = {}

        def view_data(source):
            offset = source.get('byteOffset', 0)
            size = source['byteLength']
            if source['buffer'] != 0 or offset < 0 or size < 0 or offset + size > len(binary):
                raise ValueError(f'Buffer del escenario invalido: {part}')
            return binary[offset:offset + size]

        def view(number):
            if number not in views:
                source = model['bufferViews'][number]
                storage.extend(b'\0' * (-len(storage) % 4))
                views[number] = len(room['bufferViews'])
                room['bufferViews'].append(source | {'byteOffset': len(storage)})
                storage.extend(view_data(source))
            return views[number]

        def accessor(number):
            if number not in accessors:
                source = model['accessors'][number]
                if source.get('sparse'):
                    raise ValueError('El escenario usa un accessor disperso.')
                accessors[number] = len(room['accessors'])
                room['accessors'].append(source | {'bufferView': view(source['bufferView'])})
            return accessors[number]

        images = model.get('images', [])
        material_keys = ('textures', 'materials', 'samplers')
        if index == 0:
            for key in material_keys:
                if key in model:
                    room[key] = model[key]
            room['images'] = [image | {'bufferView': view(image['bufferView'])} for image in images]
            image_bank = [bytes(view_data(model['bufferViews'][image['bufferView']])) for image in images]
        elif (any(room.get(key) != model.get(key) for key in material_keys) or len(images) != len(image_bank) or
              any(view_data(model['bufferViews'][im['bufferView']]) != original for im, original in zip(images, image_bank))):
            raise ValueError(f'La pieza {part.name} usa otro banco de materiales.')

        # Rest-pose inverse bind transforms cancel their joints. Retain the
        # model placement and static geometry and omit the room's unused rigs.
        def node(number):
            src = model['nodes'][number]
            children = [child for i in src.get('children', ()) if (child := node(i)) is not None]
            result = {key: src[key] for key in ('name', 'translation', 'rotation', 'scale', 'matrix') if key in src}
            if children:
                result['children'] = children
            if 'mesh' in src:
                mesh = model['meshes'][src['mesh']]
                # These volumes are shadow overlays. Drawing them as opaque
                # surfaces covers the ground under storefronts and actors.
                if not re.fullmatch(r'_\d{2}_shad', mesh.get('name', '')):
                    primitives = []
                    for primitive in mesh['primitives']:
                        attributes = {key: accessor(value) for key, value in primitive['attributes'].items()
                                      if not key.startswith(('JOINTS_', 'WEIGHTS_'))}
                        primitives.append(primitive | {'attributes': attributes, 'indices': accessor(primitive['indices'])})
                    result['mesh'] = len(room['meshes'])
                    room['meshes'].append(mesh | {'primitives': primitives})
            if not children and 'mesh' not in result:
                return None
            room['nodes'].append(result)
            return len(room['nodes']) - 1

        root_nodes = model['scenes'][model.get('scene', 0)]['nodes']
        room['scenes'][0]['nodes'].extend(child for root in root_nodes if (child := node(root)) is not None)

    if not room['meshes']:
        raise ValueError('El escenario empaquetado no contiene mallas.')
    room['buffers'] = [{'byteLength': len(storage)}]
    encoded = json.dumps(room, separators=(',', ':')).encode()
    encoded += b' ' * (-len(encoded) % 4)
    storage.extend(b'\0' * (-len(storage) % 4))
    with destination.open('wb') as out:
        out.write(struct.pack('<4sIIII', b'glTF', 2, 28 + len(encoded) + len(storage), len(encoded), 0x4E4F534A))
        out.write(encoded)
        out.write(struct.pack('<II', len(storage), 0x004E4942))
        out.write(storage)
