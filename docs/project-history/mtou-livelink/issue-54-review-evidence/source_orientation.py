"""Read-only world-space orientation evidence, independent of UE's pose oracle."""
import json
import sys
from pathlib import Path
import maya.standalone
maya.standalone.initialize(name='python')
from maya import cmds

scene_path = sys.argv[1]
output_path = Path(sys.argv[2])
cmds.file(scene_path, open=True, force=True, prompt=False)
rows = []
for time in (1, 120):
    cmds.currentTime(time)
    positions = {}
    for name in ('root', 'pelvis', 'head', 'neck_01'):
        nodes = cmds.ls('SK_C01:' + name, type='joint', long=True) or []
        if len(nodes) == 1:
            xyz = cmds.xform(nodes[0], query=True, worldSpace=True, translation=True)
            positions[name] = {'maya_world': xyz, 'ue_axes_world': [xyz[0], xyz[2], xyz[1]]}
    rows.append({'time': time, 'positions': positions})
output_path.write_text(json.dumps({'scene_saved': False, 'samples': rows}, indent=2), encoding='utf-8')
cmds.file(new=True, force=True)
maya.standalone.uninitialize()
