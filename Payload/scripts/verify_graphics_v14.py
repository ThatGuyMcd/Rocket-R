#!/usr/bin/env python3
from pathlib import Path
import argparse, ast

def span(text, marker):
    start=text.find(marker)
    if start<0: raise RuntimeError("missing function")
    brace=text.find("{",start); depth=0; state="code"; i=brace
    while i<len(text):
        c=text[i]; n=text[i+1] if i+1<len(text) else ""
        if state=="code":
            if c=="/" and n=="/": state="line"; i+=2; continue
            if c=="/" and n=="*": state="block"; i+=2; continue
            if c=='"': state="string"
            elif c=="'": state="char"
            elif c=="{": depth+=1
            elif c=="}":
                depth-=1
                if depth==0: return start,i+1
        elif state=="line":
            if c=="\n": state="code"
        elif state=="block":
            if c=="*" and n=="/": state="code"; i+=2; continue
        elif state=="string":
            if c=="\\": i+=2; continue
            if c=='"': state="code"
        elif state=="char":
            if c=="\\": i+=2; continue
            if c=="'": state="code"
        i+=1
    raise RuntimeError("unterminated function")

def main():
    ap=argparse.ArgumentParser(); ap.add_argument("--root",required=True)
    root=Path(ap.parse_args().root).resolve()
    text=(root/"src/widescreen_culling.cpp").read_text(encoding="utf-8-sig")
    s,e=span(text,'extern "C" void rocket_widescreen_frustum_begin')
    body=text[s:e]
    req=(
        "kNoSideCullRadiusBits = 0x7F7FFFFFU",
        "rocket::graphics::widescreen_active(4.0F / 3.0F)",
        "settings.fov_offset_degrees > 0.001F",
        "context->r6 = static_cast<gpr>(kNoSideCullRadiusBits)",
        "culling/fade remains active",
    )
    bad=(
        "target_diagonal_half","target_planes",
        "requested_horizontal_half","requested_vertical_half",
        "required_radius = std::max(required_radius, plane_distance)",
        "ReadVec3(rdram, camera","WriteVec3(rdram, camera",
        "context->r7",
    )
    for t in req:
        if t not in body: raise SystemExit("[ERROR] missing v14 token: "+t)
    for t in bad:
        if t in body: raise SystemExit("[ERROR] retired/unsafe v14 token present: "+t)
    print("[OK] Rocket-R Graphics v14 viewport-safe culling verification PASS.")

if __name__=="__main__": main()
