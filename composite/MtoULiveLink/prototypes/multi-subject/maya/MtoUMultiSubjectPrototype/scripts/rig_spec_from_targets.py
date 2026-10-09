"""Build a fixture rig spec from an Unreal target-skeleton dump.

The Unreal real-asset check writes ``mtou-multi-subject-real-targets-unreal.json``
(one entry per registered target: bone list, reference pose, Morph names). A
Maya subject can only be paired with such a target when the Maya rig has the
target's own skeleton, so for a target the supplied scene does not already
contain this tool emits the recipe the Maya peer builds with ``--rig-spec``.

The recipe carries each bone's local translation and rotation in Maya space.
Both are inverted from the dump's UE-space values with the product's own
``convert_transform`` mapping (translation ``(x, z, y)``, quaternion
``(-x, -z, -y, w)``), and the quaternion is verified against its Euler form
before it is written, so a spec this tool emits binds within the prototype's
tolerance or the tool refuses.

    python rig_spec_from_targets.py --targets targets.json --target-id prop \
        --namespace prop --output prop-spec.json [--fps 30] [--range 1,6]

Standard library only.
"""

import argparse
import json
import math
import sys
from pathlib import Path

#: The generated spec must bind within the prototype's rotation tolerance; the
#: verification below is stricter than the receiver's 0.5 degrees.
ROTATION_VERIFY_TOLERANCE_DEGREES = 0.05
#: Scale is not part of the recipe; anything beyond this is refused.
SCALE_TOLERANCE = 0.005


class SpecRefused(RuntimeError):
    pass


def read_numbers(value, count, what):
    """A JSON array of ``count`` finite numbers."""
    if not isinstance(value, (list, tuple)) or len(value) != count:
        raise SpecRefused("{0} must be {1} numbers, got {2!r}".format(what, count, value))
    numbers = []
    for item in value:
        if isinstance(item, bool) or not isinstance(item, (int, float)) \
                or not math.isfinite(float(item)):
            raise SpecRefused("{0} must be finite numbers, got {1!r}".format(what, value))
        numbers.append(float(item))
    return numbers


def ue_quaternion(value):
    """UE-space quaternion from the dump's ``rotation`` array."""
    quaternion = read_numbers(value, 4, "rotation")
    norm = math.sqrt(sum(component * component for component in quaternion))
    if norm <= 0.0:
        raise SpecRefused("rotation {0!r} has no length".format(value))
    return [component / norm for component in quaternion]


def maya_quaternion(ue):
    """Inverse of the product's ``convert_transform`` quaternion mapping."""
    x, y, z, w = ue
    return [-x, -z, -y, w]


def quaternion_multiply(a, b):
    ax, ay, az, aw = a
    bx, by, bz, bw = b
    return [
        aw * bx + ax * bw + ay * bz - az * by,
        aw * by - ax * bz + ay * bw + az * bx,
        aw * bz + ax * by - ay * bx + az * bw,
        aw * bw - ax * bx - ay * by - az * bz,
    ]


def quaternion_axis(axis, degrees):
    half = math.radians(degrees) * 0.5
    sine, cosine = math.sin(half), math.cos(half)
    return [
        sine if axis == 0 else 0.0,
        sine if axis == 1 else 0.0,
        sine if axis == 2 else 0.0,
        cosine,
    ]


def quaternion_angle_degrees(a, b):
    dot = abs(sum(x * y for x, y in zip(a, b)))
    return math.degrees(2.0 * math.acos(min(1.0, max(-1.0, dot))))


def euler_xyz_from_quaternion_degrees(quaternion):
    """Maya ``rotate`` (degrees, rotateOrder xyz) for a quaternion.

    Maya's rotation order ``xyz`` composes the quaternion as
    ``qz * qy * qx``; that convention was checked against the running Maya
    build, and the result is verified again below before it is written.
    """
    x, y, z, w = quaternion
    # Rotation matrix of the quaternion.
    matrix = [
        [1 - 2 * (y * y + z * z), 2 * (x * y - z * w), 2 * (x * z + y * w)],
        [2 * (x * y + z * w), 1 - 2 * (x * x + z * z), 2 * (y * z - x * w)],
        [2 * (x * z - y * w), 2 * (y * z + x * w), 1 - 2 * (x * x + y * y)],
    ]
    sin_y = max(-1.0, min(1.0, -matrix[2][0]))
    ry = math.asin(sin_y)
    if abs(sin_y) > 1.0 - 1.0e-9:  # gimbal lock: x and z share one axis
        return None
    rx = math.atan2(matrix[2][1], matrix[2][2])
    rz = math.atan2(matrix[1][0], matrix[0][0])
    return [math.degrees(rx), math.degrees(ry), math.degrees(rz)]


def maya_rest_rotation(value, bone_name):
    """Maya Euler degrees whose local rotation is the dump's UE rotation."""
    quaternion = maya_quaternion(ue_quaternion(value))
    euler = euler_xyz_from_quaternion_degrees(quaternion)
    if euler is None:
        raise SpecRefused(
            "bone {0!r} rests in gimbal lock; the recipe cannot express it "
            "unambiguously".format(bone_name))
    recomposed = quaternion_multiply(
        quaternion_multiply(quaternion_axis(2, euler[2]), quaternion_axis(1, euler[1])),
        quaternion_axis(0, euler[0]))
    if quaternion_angle_degrees(recomposed, quaternion) \
            > ROTATION_VERIFY_TOLERANCE_DEGREES:
        raise SpecRefused(
            "bone {0!r} rests rotated beyond what the recipe can express"
            .format(bone_name))
    if max(abs(value) for value in euler) < 1.0e-9:
        return None
    return euler


def scale_error(value):
    return max(abs(component - 1.0)
               for component in read_numbers(value, 3, "scale"))


def translation(value):
    """Maya-space translation from the dump's UE-space translation."""
    x, y, z = read_numbers(value, 3, "translation")
    return [x, z, y]


def parse_skeleton_entry(entry):
    """One ``skeleton[]`` row of the dump: index, name, parent, rest transform."""
    if not isinstance(entry, dict):
        raise SpecRefused("skeleton entry is not an object: {0!r}".format(entry))
    for key in ("index", "name", "parent", "translation", "rotation", "scale"):
        if key not in entry:
            raise SpecRefused("skeleton entry has no {0}: {1!r}".format(key, entry))
    try:
        index = int(entry["index"])
    except (TypeError, ValueError):
        raise SpecRefused("skeleton entry has no index: {0!r}".format(entry))
    return {
        "index": index,
        "name": str(entry["name"]),
        "parent_name": str(entry["parent"]),
        "translate": entry["translation"],
        "rotation": entry["rotation"],
        "scale": entry["scale"],
    }


def bone_specs(target, extra_bone_suffix=None):
    rows = [parse_skeleton_entry(entry) for entry in target["skeleton"]]
    rows.sort(key=lambda row: row["index"])
    if [row["index"] for row in rows] != list(range(len(rows))):
        raise SpecRefused("the skeleton dump is not a complete parent-first list")
    by_name = {}
    bones = []
    for row in rows:
        if row["parent_name"] == "none":
            parent = -1
        else:
            if row["parent_name"] not in by_name:
                raise SpecRefused(
                    "bone {0!r} names parent {1!r} before it appears".format(
                        row["name"], row["parent_name"]))
            parent = by_name[row["parent_name"]]
        if row["scale"] and scale_error(row["scale"]) > SCALE_TOLERANCE:
            raise SpecRefused(
                "bone {0!r} rests scaled; the recipe only carries a local "
                "translation and rotation".format(row["name"]))
        bone = {
            "name": row["name"],
            "parent": parent,
            "translate": translation(row["translate"]),
        }
        rotate = maya_rest_rotation(row["rotation"], row["name"])
        if rotate is not None:
            bone["rotate"] = rotate
        by_name[row["name"]] = len(bones)
        bones.append(bone)
    if extra_bone_suffix:
        for row in rows:
            if row["parent_name"] == "none":
                continue
            bones.append({
                "name": row["name"] + extra_bone_suffix,
                "parent": by_name[row["name"]],
                "translate": [0.0, 0.0, 0.0],
            })
    if not bones:
        raise SpecRefused("the target skeleton is empty")
    return bones


def make_spec(targets, target_id, namespace, fps, time_range, mesh_size,
              extra_bone_suffix, animate):
    if target_id not in targets:
        raise SpecRefused("no target {0!r} in the dump".format(target_id))
    bones = bone_specs(targets[target_id], extra_bone_suffix)
    if bones[0]["parent"] != -1:
        raise SpecRefused("the skeleton dump's first bone is not the root")
    animation = {}
    if animate:
        if len(bones) < 2:
            raise SpecRefused("a single-bone target has no second bone to animate")
        # Parent motion above the root and a rotating second bone: the pair
        # exercises "the root world pose carries its parent" and "a non-root
        # bone moves", without touching the bind pose.
        animation[bones[1]["name"]] = {
            "rotateX": [[time_range[0], 0.0], [time_range[1], 35.0]]}
    curves = {"Shared": [[time_range[0], 0.5], [time_range[1], 0.5]]}
    rig = {
        "namespace": namespace,
        "ancestors": [{
            "name": "Socket",
            "translate": [0.0, 0.0, 0.0],
            "animation": {"translateY": [[time_range[0], 0.0], [time_range[1], 4.0]]},
        }] if animate else [],
        "bones": bones,
        "mesh": {"name": namespace + "Mesh", "size": list(mesh_size)},
        "curves": curves,
        "animation": animation,
    }
    placeholder = {
        "namespace": "character",
        "ancestors": [],
        "bones": [{"name": "Root", "parent": -1, "translate": [0.0, 0.0, 0.0]}],
        "mesh": {"name": "characterMesh", "size": [10.0, 10.0, 10.0]},
        "curves": {"Shared": [[time_range[0], 0.0], [time_range[1], 0.0]]},
        "animation": {},
    }
    return {
        "name": "mtou-real-{0}".format(namespace),
        "revision": 1,
        "fps": float(fps),
        "range": [float(time_range[0]), float(time_range[1])],
        "rigs": {"character": placeholder, namespace: rig},
    }


def main(argv=None):
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    parser.add_argument("--targets", required=True,
                        help="the Unreal target dump written by the real-asset check")
    parser.add_argument("--target-id", required=True,
                        help="target id inside the dump (for example 'prop')")
    parser.add_argument("--namespace", default=None,
                        help="Maya rig namespace; defaults to the target id")
    parser.add_argument("--output", required=True)
    parser.add_argument("--fps", type=float, default=30.0)
    parser.add_argument("--range", default="1,6",
                        help="time range the generated rig animates over")
    parser.add_argument("--mesh-size", default="20,40,20",
                        help="generated mesh size in centimetres")
    parser.add_argument("--extra-bone-suffix", default=None,
                        help="append a renamed copy of every non-root bone, to "
                             "stand in for an export branch the scene carries")
    parser.add_argument("--no-animation", action="store_true",
                        help="do not animate the generated rig")
    args = parser.parse_args(argv)

    data = json.loads(Path(args.targets).read_text(encoding="utf-8"))
    targets = data.get("targets", data)
    time_range = [float(part) for part in str(args.range).split(",")]
    mesh_size = [float(part) for part in str(args.mesh_size).split(",")]
    spec = make_spec(
        targets, args.target_id, args.namespace or args.target_id,
        args.fps, time_range, mesh_size, args.extra_bone_suffix,
        not args.no_animation)
    Path(args.output).write_text(
        json.dumps(spec, indent=2, ensure_ascii=True) + "\n", encoding="utf-8")
    print(json.dumps({
        "output": args.output,
        "namespace": args.namespace or args.target_id,
        "bones": len(spec["rigs"][args.namespace or args.target_id]["bones"]),
    }))
    return 0


if __name__ == "__main__":
    try:
        raise SystemExit(main())
    except SpecRefused as error:
        print("refused: {0}".format(error), file=sys.stderr)
        raise SystemExit(1)
