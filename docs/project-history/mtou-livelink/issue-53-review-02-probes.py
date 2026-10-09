"""Reproduce review-02 recovery defects in disposable Maya standalone scenes.

Run with mayapy, --fbx/--manifest pointing to a valid main fixture handoff,
and --result pointing to a scratch JSON file. Exit 1 means a defect reproduced;
exit 0 means these two probes did not reproduce it. No scene is saved.
"""
import argparse
import json
import sys
from pathlib import Path


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--fbx", required=True)
    parser.add_argument("--manifest", required=True)
    parser.add_argument("--result", required=True)
    args = parser.parse_args()
    root = Path(__file__).resolve().parents[3]
    sys.path.insert(0, str(root / "composite/MtoULiveLink/prototypes/scene-reference"
                         "/maya/MtoUSceneRefPrototype/scripts"))
    import MtoUSceneRefPrototype as ref
    ref.ensure_maya()
    mc = ref.commands()
    container = ref.DEFAULT_CONTAINER
    results = {}

    def call():
        return ref.run(args.fbx, args.manifest, dry_run=True)

    def baseline():
        mc.file(new=True, force=True)
        mc.currentUnit(linear="cm")
        report, code = ref.run(args.fbx, args.manifest)
        if code or not report["ok"]:
            raise RuntimeError("Baseline import failed: " + str(report["problems"]))
        return mc.ls(report["container"]["group_path"], uuid=True)[0]

    def snapshot(report, code, old, foreign=None):
        return {"exit": code, "ok": report["ok"], "problems": report["problems"],
                "warnings": report["warnings"], "update": report["update"],
                "old_paths": mc.ls(old, long=True),
                "foreign_paths": mc.ls(foreign, long=True) if foreign else None}

    try:
        old = baseline()
        mc.namespace(rename=(container, container + ref.RETIRING_SUFFIX))
        mc.namespace(add=container)
        half = mc.createNode("transform", name=container + ":" + container)
        mc.addAttr(half, longName=ref.OWNERSHIP_ATTRIBUTE, dataType="string")
        mc.setAttr(half + "." + ref.OWNERSHIP_ATTRIBUTE,
                   ref._ownership_mark(container + ref.STAGING_SUFFIX), type="string")
        foreign = mc.createNode("transform", name=container + ":ProductionObject")
        foreign_uuid = mc.ls(foreign, uuid=True)[0]
        report, code = call()
        results["mixed_destination"] = snapshot(report, code, old, foreign_uuid)
        results["R-002_reproduced"] = not bool(mc.ls(foreign_uuid))

        old = baseline()
        original_paths = ref.SceneRefImporter._rehome_paths
        original_namespace = mc.namespace

        def fail_paths(self):
            raise RuntimeError("review injected final-path query exception")

        def refuse_delete(*args, **kwargs):
            if kwargs.get("removeNamespace") == container and kwargs.get("deleteNamespaceContent"):
                raise RuntimeError("review injected destination cleanup refusal")
            return original_namespace(*args, **kwargs)

        ref.SceneRefImporter._rehome_paths = fail_paths
        mc.namespace = refuse_delete
        try:
            report, code = ref.run(args.fbx, args.manifest)
            results["failed_takeover"] = snapshot(report, code, old)
            ref.SceneRefImporter._rehome_paths = original_paths
            report, code = call()
            results["retry_delete_refused"] = snapshot(report, code, old)
            results["retry_delete_refused"]["container_token"] = ref._ownership_token(
                "|" + container + ":" + container)
        finally:
            ref.SceneRefImporter._rehome_paths = original_paths
            mc.namespace = original_namespace
        report, code = call()
        results["retry_delete_allowed"] = snapshot(report, code, old)
        results["R-001_reproduced"] = not bool(mc.ls(old))
    finally:
        Path(args.result).write_text(json.dumps(results, indent=2), encoding="utf-8")
        import maya.standalone
        maya.standalone.uninitialize()
    print(json.dumps({k: v for k, v in results.items() if k.endswith("_reproduced")}))
    return int(any(results.get(k) for k in ("R-001_reproduced", "R-002_reproduced")))


if __name__ == "__main__":
    raise SystemExit(main())
