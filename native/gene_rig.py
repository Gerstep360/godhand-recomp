"""Bake Gene's original model and MOT clips into a PC animation library."""

import struct

import numpy as np

from pack_room import read_glb, write_glb


BASE_MESHES = {'z_00', 'y_cloth_', 'hat', 'cort', 'RH_00', 'LH_00', 'HD_00', 'GH_00'}


def decode_mot(data):
    if len(data) < 8:
        raise ValueError('MOT de Gene truncado.')
    signature, frames, count, _ = struct.unpack_from('<IHBB', data)
    table_end = 8 + count * 12
    if signature != int.from_bytes(b'mtb3', 'little') or frames < 2 or table_end > len(data):
        raise ValueError('MOT de Gene invalido.')

    def half(value):
        if value & 0x7fff == 0:
            return 0.0
        exponent = (value >> 9) & 63
        return (-1 if value & 0x8000 else 1) * (1 + (value & 511) / 512) * 2.0 ** (exponent - 47)

    t = np.arange(frames)

    def hermite(times, positions, incoming, outgoing):
        values = np.full(frames, positions[0])
        for i in range(len(times) - 1):
            start, end = times[i:i + 2]
            if end <= start:
                continue
            covered = (t >= start) & (t < end)
            u = (t[covered] - start) / (end - start)
            u2, u3 = u * u, u * u * u
            # The stored tangents are expressed per segment.
            values[covered] = ((2*u3 - 3*u2 + 1)*positions[i] + (u3 - 2*u2 + u)*outgoing[i]
                               + (-2*u3 + 3*u2)*positions[i + 1] + (u3 - u2)*incoming[i + 1])
        values[t >= times[-1]] = positions[-1]
        return values

    tracks = {}
    for target, channel, keys, _, offset in struct.iter_unpack('<BBHII', data[8:table_end]):
        component = channel & 15
        if component > 5:
            continue
        encoding = channel & 0xf0
        if encoding == 0:
            values = np.full(frames, struct.unpack('<f', struct.pack('<I', offset))[0])
        elif encoding == 16 and keys and table_end <= offset and offset + 12 + keys * 4 <= len(data):
            p, dp, a, da, b, db = map(half, struct.unpack_from('<6H', data, offset))
            delta, points, incoming, outgoing = np.frombuffer(data, np.uint8, keys * 4, offset + 12).reshape(-1, 4).T
            values = hermite(np.cumsum(delta), p + dp * points, a + da * incoming, b + db * outgoing)
        elif encoding == 0x30 and keys and table_end <= offset and offset + 24 + keys * 8 <= len(data):
            # Semi-precision MOT uses a float header and absolute 16-bit keys.
            # https://github.com/akitotheanimator/God-Hand-Tools/wiki/God-Hand-MOT-Files
            p, dp, a, da, b, db = struct.unpack_from('<6f', data, offset)
            times, points, incoming, outgoing = np.frombuffer(data, '<u2', keys * 4, offset + 24).reshape(-1, 4).T
            values = hermite(times, p + dp * points, a + da * incoming, b + db * outgoing)
        else:
            raise ValueError(f'MOT usa una curva no admitida en el canal {channel:#x}.')
        if not np.isfinite(values).all():
            raise ValueError('MOT contiene valores no finitos.')
        tracks[target, component] = values
    return frames, tracks


def euler_matrix(angles):
    (sx, sy, sz), (cx, cy, cz) = np.sin(angles), np.cos(angles)
    return np.array([[cy*cz, sx*sy*cz - cx*sz, cx*sy*cz + sx*sz],
                     [cy*sz, sx*sy*sz + cx*cz, cx*sy*sz - sx*cz],
                     [-sy, sx*cy, cx*cy]])


def euler_quaternions(angles):
    sx, sy, sz = np.sin(angles / 2).T
    cx, cy, cz = np.cos(angles / 2).T
    return np.column_stack((sx*cy*cz - cx*sy*sz, cx*sy*cz + sx*cy*sz,
                            cx*cy*sz - sx*sy*cz, cx*cy*cz + sx*sy*sz))


def quaternion(rotation):
    # Choose the largest component to keep the divisor away from zero.
    x, y, z = rotation.diagonal()
    diagonal = np.array([1+x-y-z, 1-x+y-z, 1-x-y+z, 1+x+y+z])
    component = int(np.argmax(diagonal))
    r = rotation
    variants = (
        [diagonal[0], r[0, 1]+r[1, 0], r[0, 2]+r[2, 0], r[2, 1]-r[1, 2]],
        [r[0, 1]+r[1, 0], diagonal[1], r[1, 2]+r[2, 1], r[0, 2]-r[2, 0]],
        [r[0, 2]+r[2, 0], r[1, 2]+r[2, 1], diagonal[2], r[1, 0]-r[0, 1]],
        [r[2, 1]-r[1, 2], r[0, 2]-r[2, 0], r[1, 0]-r[0, 1], diagonal[3]],
    )
    return np.array(variants[component]) / (2 * np.sqrt(diagonal[component]))


def shin_basis(direction, foot, thigh):
    length = np.linalg.norm(direction, axis=1, keepdims=True)
    up = np.where(length > 1e-6, -direction / np.maximum(length, 1e-6), thigh[:, :, 1])
    right = np.cross(up, foot[:, :, 2])
    length = np.linalg.norm(right, axis=1, keepdims=True)
    right = np.where(length > 1e-6, right, np.cross(up, thigh[:, :, 2]))
    length = np.linalg.norm(right, axis=1, keepdims=True)
    right = np.where(length > 1e-6, right, np.cross(up, np.eye(3)[np.argmin(np.abs(up), axis=1)]))
    right /= np.linalg.norm(right, axis=1, keepdims=True)
    return np.stack((right, up, np.cross(right, up)), axis=2)


def rotation_track(matrices):
    result = [quaternion(matrices[0])]
    for matrix in matrices[1:]:
        value = quaternion(matrix)
        result.append(-value if np.dot(result[-1], value) < 0 else value)
    return result


def build_player(model, clips, destination):
    doc, mesh_data = read_glb(model)
    binary = bytearray(mesh_data)
    nodes = doc['nodes']
    bones = {int(n['extras']['godhandName']): i for i, n in enumerate(nodes)
             if 'godhandName' in n.get('extras', {})}
    parents = {c: i for i, node in enumerate(nodes) for c in node.get('children', [])}
    waist = bones[0]
    rest = {j: np.asarray(nodes[j].get('translation', [0, 0, 0])) for j in bones.values()}
    legs = ((18, 19, 20, 21), (22, 23, 24, 25))
    pelvis = bones[17]
    for node in nodes:
        if 'mesh' in node and doc['meshes'][node['mesh']].get('name', '').rstrip('\0 ') not in BASE_MESHES:
            node.pop('mesh')
            node.pop('skin', None)

    for leg in legs:
        hip, knee, ankle, toe = [bones[number] for number in leg]
        if (parents[hip], parents[knee], parents[ankle], parents[toe]) != (pelvis, hip, knee, ankle):
            raise ValueError('Los huesos de Gene tienen otra jerarquia.')

    animations = []
    for name, raw in clips:
        frames, tracks = decode_mot(raw)
        output = {'name': name, 'channels': [], 'samplers': []}

        def accessor(values):
            data = np.asarray(values, dtype='<f4')
            width = data.shape[-1] if data.ndim == 2 else 1
            if data.shape[0] != frames or not np.isfinite(data).all():
                raise ValueError(f'Muestras invalidas en {name}.')
            binary.extend(b'\0' * (-len(binary) % 4))
            view = len(doc['bufferViews'])
            doc['bufferViews'].append({'buffer': 0, 'byteOffset': len(binary), 'byteLength': data.nbytes})
            binary.extend(data.tobytes())
            index = len(doc['accessors'])
            info = {'bufferView': view, 'componentType': 5126, 'count': frames, 'type': 'SCALAR' if width == 1 else f'VEC{width}'}
            if width == 1:
                info.update(min=[float(data[0])], max=[float(data[-1])])
            doc['accessors'].append(info)
            return index

        # Raylib's glTF loader samples in 17 ms steps. Match that grid so the
        # inspector addresses source frames exactly, including the last one.
        sample_times = np.arange(frames) * .017
        sample_times[-1] += .000001
        time = accessor(sample_times)

        def channel(joint, path, values):
            output['channels'].append({'sampler': len(output['samplers']), 'target': {'node': joint, 'path': path}})
            output['samplers'].append({'input': time, 'output': accessor(values), 'interpolation': 'LINEAR'})

        def motion(target, first, default=(0, 0, 0)):
            return np.column_stack([tracks.get((target, first + i), np.full(frames, default[i])) for i in range(3)])

        _, _, records, _ = struct.unpack_from('<IHBB', raw)
        global_joints = {target for target, component, _, flags, _ in
                         struct.iter_unpack('<BBHII', raw[8:8 + records * 12])
                         if 3 <= (component & 15) <= 5 and flags & 1}
        ik_legs = [leg for leg in legs if leg[0] in global_joints
                   and all((leg[2], axis) in tracks for axis in range(3))]
        derived_joints = {joint for leg in ik_legs for joint in leg[1:3]}
        waist_track = motion(0, 0, rest[waist])
        if ik_legs:
            def orientation(number):
                return np.array([euler_matrix(sample) for sample in motion(number, 3)])

            waist_rotations = orientation(0)
            pelvis_rotations = waist_rotations @ orientation(17)
            pelvis_origin = waist_track + np.einsum('fij,fj->fi', waist_rotations, rest[pelvis] + motion(17, 0))
            for hip, knee, ankle, toe in ik_legs:
                # MOT keys the thigh locally and stores a model-space foot
                # target at the toe pivot. Recover the ankle in all three axes.
                thigh = pelvis_rotations @ orientation(hip)
                hip_origin = pelvis_origin + np.einsum('fij,fj->fi', pelvis_rotations, rest[bones[hip]] + motion(hip, 0))
                knee_origin = hip_origin + thigh @ rest[bones[knee]]
                foot = orientation(ankle)
                ankle_goal = motion(ankle, 0) - foot @ rest[bones[toe]]
                shin = shin_basis(ankle_goal - knee_origin, foot, thigh)
                channel(bones[knee], 'rotation', rotation_track(thigh.transpose(0, 2, 1) @ shin))
                channel(bones[ankle], 'rotation', rotation_track(shin.transpose(0, 2, 1) @ foot))
        for number, joint in bones.items():
            if number == 0:
                channel(joint, 'translation', waist_track)
            elif number not in derived_joints and any((number, i) in tracks for i in range(3)):
                channel(joint, 'translation', rest[joint] + motion(number, 0))
            if number not in derived_joints and any((number, i) in tracks for i in range(3, 6)):
                channel(joint, 'rotation', euler_quaternions(motion(number, 3)))
        animations.append(output)
    doc['animations'] = animations
    write_glb(doc, binary, destination)
