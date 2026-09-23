#!/usr/bin/env python3
"""Build a "Foxfire Review" scene collection in the real OBS, to look at what we just made.

junkie asked for this as a standing thing: whenever a piece of Foxfire gets built, there should be
scenes in his OWN OBS that show it, so he can open the app and see it rather than read about it.

Two rules this file exists to keep:

  * It writes ONE file -- basic/scenes/Foxfire_Review.json -- and nothing else. In particular it
    does NOT touch user.ini. OBS loads the collection named in user.ini's [Basic] SceneCollection
    (on OBS 32 that key lives in user.ini, not global.ini), so pointing it here would silently
    switch which collection he opens into. The file being present is enough: it appears under
    Scene Collection in the menu bar, and he chooses when to look.

  * The internal "name" has to match the FILENAME STEM. OBS resolves a collection by that name,
    and one whose name disagrees with its file is ignored outright -- measured in
    tools/dock-proof.py, which hit the same thing ("No scene file found, creating default scene").
    Hence Foxfire_Review, no spaces, in both places.

The scenes deliberately cover all THREE Foxfire source kinds -- a visualizer, an effects filter on
an ordinary source, and the scene transitions -- because the dock lists one row per Foxfire source
and the three kinds are how you can see at a glance that it is finding all of them.
"""
import argparse
import json
import shutil
import uuid
from pathlib import Path

CONFIG = Path.home() / ".config" / "obs-studio"
PACKS = CONFIG / "plugin_config" / "foxfire" / "packs"
STEM = "Foxfire_Review"

W, H = 1920, 1080


def _uuid() -> str:
    return str(uuid.uuid4())


def src(name: str, sid: str, settings: dict, filters=None) -> dict:
    """A source in the shape OBS itself writes, including the uuid and prev_ver it stamps on every
    one. Neither is required to load -- OBS fills them in -- but writing a file that matches what
    it would have written keeps a hand-made collection from looking foreign in a diff."""
    d = {"prev_ver": 0x20200000, "name": name, "uuid": _uuid(), "id": sid, "versioned_id": sid,
         "settings": settings, "mixers": 0, "sync": 0, "flags": 0, "volume": 1.0, "balance": 0.5,
         "enabled": True, "muted": False, "push-to-mute": False, "push-to-mute-delay": 0,
         "push-to-talk": False, "push-to-talk-delay": 0, "hotkeys": {}, "deinterlace_mode": 0,
         "deinterlace_field_order": 0, "monitoring_type": 0, "private_settings": {}}
    if filters:
        d["filters"] = filters
    return d


def item(name: str, i: int, pos=(0.0, 0.0), scale=(1.0, 1.0)) -> dict:
    return {"name": name, "source_uuid": "", "visible": True, "locked": False, "rot": 0.0,
            "pos": {"x": pos[0], "y": pos[1]}, "scale": {"x": scale[0], "y": scale[1]},
            "align": 5, "bounds_type": 0, "bounds_align": 0, "bounds": {"x": 0.0, "y": 0.0},
            "crop_left": 0, "crop_top": 0, "crop_right": 0, "crop_bottom": 0, "id": i,
            "group_item_backup": False, "scale_filter": "disable", "blend_method": "default",
            "blend_type": "normal", "show_transition": {"duration": 0},
            "hide_transition": {"duration": 0}, "private_settings": {}}


def scene(name: str, items: list) -> dict:
    s = src(name, "scene", {"id_counter": len(items) + 1, "custom_size": False, "items": items})
    return s


def presets(pack: str, kind: str):
    """The preset ids of one kind in an INSTALLED pack, in pack order.

    Read from the installed copy rather than from the packs repo on purpose: this builds scenes for
    the OBS on this machine, and a preset that is not installed here is a scene that opens on
    "Preset Missing". Returns [] for a pack that is not installed, and the caller reports that
    rather than writing a scene that quietly shows nothing."""
    p = PACKS / pack / "pack.json"
    if not p.is_file():
        return []
    d = json.loads(p.read_text())
    return [q["id"] for q in d.get("presets", []) if q.get("kind") == kind]


def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__,
                                 formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--out", type=Path, default=CONFIG / "basic" / "scenes" / f"{STEM}.json")
    a = ap.parse_args()

    viz = presets("basics", "visualizer")
    fx = presets("basics", "effects")
    trans = [("basics", t) for t in presets("basics", "transition")]
    trans += [("kitsune-transitions", t) for t in presets("kitsune-transitions", "transition")]

    missing = [n for n, got in (("basics visualizer", viz), ("basics effects", fx),
                                ("transitions", trans)) if not got]
    if missing:
        print(f"refusing to write a collection that would open on 'Preset Missing': "
              f"nothing installed for {', '.join(missing)} under {PACKS}")
        return 1

    sources = []

    # 1 -- the visualizer, the thing with meters in the dock.
    bg = src("FR Backdrop", "color_source_v3", {"color": 0xFF141118, "width": W, "height": H})
    v = src("FR Visualizer", "foxfire_visualizer",
            {"pack": "basics", "preset": viz[0], "width": W, "height": H, "audio_mode": 0})
    sources += [bg, v]
    s1 = scene("1 — Visualizer", [item("FR Backdrop", 1), item("FR Visualizer", 2)])

    # 2 -- an ORDINARY source wearing a Foxfire effects filter. The dock only started listing
    # filters when rescan moved to obs_enum_all_sources (obs_enum_sources is inputs only), so this
    # scene is the one that shows that fix working.
    plate = src("FR Plate", "color_source_v3", {"color": 0xFF2A1F2E, "width": W, "height": H},
                filters=[src("FR Effects", "foxfire_effects", {"pack": "basics", "preset": fx[0]})])
    sources.append(plate)
    s2 = scene("2 — Effects filter", [item("FR Plate", 1)])

    # 3 -- somewhere to cut TO, so the transitions have two scenes to sit between.
    card = src("FR Card", "color_source_v3", {"color": 0xFF0B0B0E, "width": W, "height": H})
    sources.append(card)
    s3 = scene("3 — Plain card", [item("FR Card", 1)])

    sources += [s1, s2, s3]

    # One transition source per preset, so every one is in the Transitions dropdown by name and he
    # can flick through them against the two scenes above.
    tsources = [src(f"FF {pack}/{pid}", "foxfire_transition", {"pack": pack, "preset": pid})
                for pack, pid in trans]

    doc = {
        "name": STEM,  # MUST equal the filename stem -- see the module docstring
        "current_scene": "1 — Visualizer",
        "current_program_scene": "1 — Visualizer",
        "current_transition": tsources[0]["name"],
        "transition_duration": 500,
        "scene_order": [{"name": "1 — Visualizer"}, {"name": "2 — Effects filter"},
                        {"name": "3 — Plain card"}],
        "sources": sources,
        "transitions": tsources,
        "groups": [], "quick_transitions": [], "saved_projectors": [], "canvases": [],
        "resolution": {"x": W, "y": H}, "version": 2, "modules": {},
    }

    a.out.parent.mkdir(parents=True, exist_ok=True)
    if a.out.exists():
        shutil.copy(a.out, a.out.with_suffix(".json.bak"))
    a.out.write_text(json.dumps(doc, indent=4))
    print(f"wrote {a.out}")
    print(f"  3 scenes, {len([s for s in sources if s['id'] != 'scene'])} sources "
          f"(1 visualizer, 1 effects filter, 2 plain), {len(tsources)} transitions")
    print(f"  visualizer preset '{viz[0]}', effects preset '{fx[0]}'")
    print("  NOT made active: pick it under Scene Collection in the OBS menu bar.")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
