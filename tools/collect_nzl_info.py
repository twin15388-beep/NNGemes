#!/usr/bin/env python3
"""NZL Studio collector — run ON WINDOWS, game installed (not necessarily running).
Collects ONLY files/hashes/versions needed to rebuild offsets for the CURRENT build.
Usage:
  python tools/collect_nzl_info.py --game-dir "C:\\Program Files (x86)\\Steam\\steamapps\\common\\ROB RUN" --out nzl_info
Output: nzl_info/report.json + copies list. Send back the whole nzl_info folder (zip).
It does NOT read process memory, does NOT load drivers, does NOT touch anti-cheat.
"""
import argparse, hashlib, json, os, platform, sys
from pathlib import Path

def sha256(p: Path):
    h=hashlib.sha256()
    with p.open("rb") as f:
        for chunk in iter(lambda: f.read(1<<20), b""): h.update(chunk)
    return h.hexdigest()

def main():
    ap=argparse.ArgumentParser()
    ap.add_argument("--game-dir", required=True, help="ROB RUN install folder (contains GameAssembly.dll)")
    ap.add_argument("--out", default="nzl_info")
    a=ap.parse_args()
    g=Path(a.game_dir); out=Path(a.out); out.mkdir(parents=True, exist_ok=True)
    if not g.exists(): print(f"[NZL] game-dir not found: {g}"); return 2
    wanted=["GameAssembly.dll","UnityPlayer.dll","ROB RUN.exe","RobRun.exe","RUN.exe"]
    found={}
    for root,_,files in os.walk(g):
        for fn in files:
            if fn in wanted or fn=="global-metadata.dat":
                p=Path(root)/fn
                try: found[str(p.relative_to(g))]={"size":p.stat().st_size,"sha256":sha256(p)}
                except Exception as e: found[str(p.relative_to(g))]={"error":str(e)}
        # metadata usually under *_Data/il2cpp_data/Metadata
    report={"tool":"NZL Studio collector v1","os":platform.platform(),"python":sys.version,"game_dir":str(g),"files":found,
            "need_next":["Run Il2CppDumper.exe GameAssembly.dll global-metadata.dat -> send dump.cs, script.json, il2cpp.h",
                         "In match: run NZL probe (next build) OR ReClass addresses listed in docs/EXTRACT.md section 3",
                         "Tell me: game version from Steam (Properties > Installed Files) + date of last update"]}
    (out/"report.json").write_text(json.dumps(report,indent=2,ensure_ascii=False),encoding="utf-8")
    print(f"[NZL] wrote {out/'report.json'}")
    for k,v in found.items(): print(" ",k,v)
    if not any("GameAssembly" in k for k in found): print("[NZL] WARNING: GameAssembly.dll not found — wrong folder?"); return 3
    if not any("global-metadata" in k for k in found): print("[NZL] WARNING: global-metadata.dat not found — search *_Data/il2cpp_data/Metadata")
    return 0
if __name__=="__main__": raise SystemExit(main())
