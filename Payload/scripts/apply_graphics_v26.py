#!/usr/bin/env python3
from pathlib import Path
import argparse, ast, datetime, json, re, shutil, subprocess, sys

V23_BEGIN='    // === ROCKET-R GRAPHICS V23 AUTHORED SUBMODEL VISIBILITY RETENTION BEGIN ===\n'
V23_END='    // === ROCKET-R GRAPHICS V23 AUTHORED SUBMODEL VISIBILITY RETENTION END ===\n'
OWNER_HOOK_MARKER='rocket_presentation_model_entry_owner'
V25_TARGETS = (
    ('func_8001E954', '0x8001E974', '0x8001E97C'),
    ('func_800261D0', '0x800261E4', '0x800261EC'),
    ('func_80050728', '0x8005073C', '0x80050744'),
)


SELF_CHECK_V26 = r'''    # v26: keep the proven v21 queue recovery and retail draw-distance path, but bind
    # interpolated model/submodel presentation to the actual GameObject owner instead of
    # post-hoc nearest-neighbour RenderEntry matching. The retired v23 mask bypass must be off.
    _v26_root = __import__('pathlib').Path(__file__).resolve().parents[1]
    _v26_culling = (_v26_root / 'src' / 'widescreen_culling.cpp').read_text(encoding='utf-8-sig')
    _v26_graphics = (_v26_root / 'src' / 'graphics_enhancements.cpp').read_text(encoding='utf-8-sig')
    _v26_presentation = (_v26_root / 'src' / 'presentation_identity.cpp').read_text(encoding='utf-8-sig')
    _v26_header = (_v26_root / 'src' / 'presentation_identity.hpp').read_text(encoding='utf-8-sig')
    _v26_oneclick = (_v26_root / 'scripts' / 'OneClickBuild.ps1').read_text(encoding='utf-8-sig')
    _v26_policy = json.loads((_v26_root / 'runtime-recomp' / 'rocket.us.recomp-policy.json').read_text(encoding='utf-8-sig'))
    _v26_queue_path = _v26_root / 'scripts' / 'patch_render_queue_expansion_v21_generated.py'
    require(_v26_queue_path.is_file(), 'v21 generated queue patcher missing')
    _v26_queue = _v26_queue_path.read_text(encoding='utf-8-sig')
    _v26_hooks = _v26_policy.get('functionHooks', [])
    _v26_owner_hooks = [h for h in _v26_hooks if h.get('function') == 'func_8001ECEC' and str(h.get('beforeVram','')).upper() == '0X8001F084' and 'rocket_presentation_model_entry_owner' in h.get('text','')]
    _v26_required = (
        'v16 viewport-locked FOV/aspect guard active' in _v26_culling and
        'kDisableCpuSidePlanesBits = 0x7F7FFFFFU' not in _v26_culling and
        'static_cast<std::uint32_t>(context->r7)' in _v26_graphics and
        's.draw_distance_multiplier' in _v26_graphics and
        'ROCKET-R GRAPHICS V21 IN-FUNCTION RENDER QUEUE EXPANSION BEGIN' in _v26_presentation and
        'rocket_render_queue_prepare_first_batch' in _v26_presentation and
        'rocket_render_queue_prepare_next_batch' in _v26_presentation and
        'PendingModelOwner' in _v26_presentation and
        'OwnerTrack' in _v26_presentation and
        'owner_key' in _v26_presentation and
        'DURABLE-OWNER-V26' in _v26_presentation and
        'pre-render-object-gate=RETAIL' in _v26_presentation and
        'rocket_presentation_model_entry_owner' in _v26_header and
        len(_v26_owner_hooks) == 1 and
        'patch_render_queue_expansion_v21_generated.py' in _v26_oneclick and
        'patch_visibility_retention_v23_generated.py' not in _v26_oneclick and
        'ROCKET_QUEUE_V21_PROCESS_BATCH' in _v26_queue
    )
    _v26_forbidden = any(token in (_v26_presentation + _v26_oneclick) for token in (
        'authored-submodel-mask=RECOVERED-V23',
        'ROCKET-R GRAPHICS V23 AUTHORED SUBMODEL VISIBILITY RETENTION BEGIN',
        'ExpandedRenderEntry', 'RocketBuildGlobalRenderOrder',
        'rocket_render_queue_begin_batch(', 'patch_render_queue_generated.py',
        'rocket_original_func_8008B694', 'rocket_capture_func_8008B694',
        'rocket_draw_func_8008B694', '[render-queue] GLOBAL',
        'pre-render-object-gate=RECOVERED-V25', 'rocket_popdiag_object_gate_result',
        'object_gate_recovered', 'patch_prerender_object_gate_v25_generated.py',
        'ROCKET-R GRAPHICS V25 PRE-RENDER OBJECT GATE RETENTION BEGIN',
    ))
    if not (_v26_required and not _v26_forbidden):
        raise SystemExit('SOURCE SELF-CHECK FAILED: v26 durable presentation ownership state missing')
'''

def read_text(path: Path):
    raw=path.read_bytes(); return raw.decode('utf-8-sig').replace('\r\n','\n'), raw.startswith(b'\xef\xbb\xbf'), ('\r\n' if b'\r\n' in raw else '\n')

def write_text(path: Path, text: str, bom: bool, nl: str):
    text=text.replace('\r\n','\n')
    if nl=='\r\n': text=text.replace('\n','\r\n')
    data=text.encode('utf-8'); path.write_bytes((b'\xef\xbb\xbf' if bom else b'')+data)

def backup(root: Path, files):
    stamp=datetime.datetime.now().strftime('%Y%m%d-%H%M%S')
    base=root/'build'/'repair-backups'/f'graphics-v26-{stamp}'
    for rel in files:
        src=root/rel
        if src.is_file():
            dst=base/rel; dst.parent.mkdir(parents=True,exist_ok=True); shutil.copy2(src,dst)
    pointer=root/'build'/'repair-backups'/'LAST-GRAPHICS-V26-BACKUP.txt'
    pointer.parent.mkdir(parents=True,exist_ok=True)
    pointer.write_text(str(base),encoding='utf-8')
    print(f'[OK] Backup: {base}')
    return base

def restore_backup(root: Path, base: Path):
    if base is None or not base.is_dir(): return
    for src in base.rglob('*'):
        if not src.is_file(): continue
        rel=src.relative_to(base); dst=root/rel
        dst.parent.mkdir(parents=True,exist_ok=True); shutil.copy2(src,dst)
    print(f'[OK] Restored pre-v26 files from: {base}')

def function_span(text: str, marker: str):
    start=text.find(marker)
    if start<0: raise RuntimeError('Function marker not found: '+marker)
    brace=text.find('{',start)
    if brace<0: raise RuntimeError('Opening brace not found: '+marker)
    depth=0; state='code'; i=brace
    while i<len(text):
        c=text[i]; n=text[i+1] if i+1<len(text) else ''
        if state=='code':
            if c=='/' and n=='/': state='line'; i+=2; continue
            if c=='/' and n=='*': state='block'; i+=2; continue
            if c=='"': state='string'
            elif c=="'": state='char'
            elif c=='{': depth+=1
            elif c=='}':
                depth-=1
                if depth==0: return start,i+1
        elif state=='line':
            if c=='\n': state='code'
        elif state=='block':
            if c=='*' and n=='/': state='code'; i+=2; continue
        elif state=='string':
            if c=='\\': i+=2; continue
            if c=='"': state='code'
        elif state=='char':
            if c=='\\': i+=2; continue
            if c=="'": state='code'
        i+=1
    raise RuntimeError('Unterminated function: '+marker)

def alloc_token_code():
    return '''[[nodiscard]] std::uint32_t NextPresentationToken() {
    for (;;) {
        const std::uint32_t token = g_next_token++;
        if (token != 0U && token != 0xFFFFFFFFU) return token;
    }
}

'''

def strip_v25_presentation_state(text: str):
    # v25 repurposed the v23 diagnostic counter for an upstream object-gate bypass.
    # Remove it completely: v26 restores that gate to retail and never feeds this
    # counter from generated code.
    text = text.replace('std::atomic<std::uint64_t> g_popdiag_object_gate_recovered{0U};\n','')
    text = re.sub(
        r'extern "C" void rocket_popdiag_object_gate_result\(recomp_context\* context\) \{\n'
        r'    if \(context != nullptr && context->r2 == 0\) \{\n'
        r'        g_popdiag_object_gate_recovered\.fetch_add\(1U, std::memory_order_relaxed\);\n'
        r'    \}\n\}\n\n', '', text)
    text = text.replace(
        '    const std::uint64_t object_gate_recovered =\n'
        '        g_popdiag_object_gate_recovered.exchange(0U, std::memory_order_relaxed);\n', '')
    text = text.replace('would_drop_at_256_recovered object_gate_recovered\\n',
                        'would_drop_at_256_recovered\\n')
    text = text.replace('would_drop_at_256_recovered=%llu object_gate_recovered=%llu\\n',
                        'would_drop_at_256_recovered=%llu\\n')
    text = text.replace(
        '        static_cast<unsigned long long>(rejected),\n'
        '        static_cast<unsigned long long>(object_gate_recovered));',
        '        static_cast<unsigned long long>(rejected));')
    text = text.replace(
        '=== Rocket-R v25 PRE-RENDER OBJECT-GATE RETENTION + QUEUE-RECOVERY diagnostic session ===',
        '=== Rocket-R v26 DURABLE-MODEL-OWNERSHIP + QUEUE-RECOVERY diagnostic session ===')
    text = text.replace(
        'side-plane-cpu-cull=V17.1-VIEWPORT-GUARD authored-submodel-mask=RETAIL pre-render-object-gate=RECOVERED-V25',
        'side-plane-cpu-cull=V17.1-VIEWPORT-GUARD authored-submodel-mask=RETAIL pre-render-object-gate=RETAIL presentation-identity=DURABLE-OWNER-V26')
    return text

def patch_presentation(path: Path):
    text,bom,nl=read_text(path); original=text
    text=strip_v25_presentation_state(text)
    for tok in ('ROCKET-R GRAPHICS V21 IN-FUNCTION RENDER QUEUE EXPANSION BEGIN','rocket_render_queue_prepare_first_batch','rocket_render_queue_prepare_next_batch'):
        if tok not in text: raise RuntimeError('Required v21 queue recovery missing: '+tok)

    # Remove the disproven v23 telemetry/label additions without disturbing v21 diagnostics.
    text=text.replace('=== Rocket-R v23 AUTHORED-VISIBILITY RETENTION + QUEUE-RECOVERY diagnostic session ===','=== Rocket-R v26 DURABLE-PRESENTATION-IDENTITY + QUEUE-RECOVERY diagnostic session ===')
    text=text.replace('authored-submodel-mask=RECOVERED-V23','authored-submodel-mask=RETAIL presentation-identity=DURABLE-OWNER-V26')
    text=text.replace('V17.1-VIEWPORT-GUARD authored-submodel-mask=RETAIL presentation-identity=DURABLE-OWNER-V26','V17.1-VIEWPORT-GUARD authored-submodel-mask=RETAIL presentation-identity=DURABLE-OWNER-V26')
    text=text.replace('std::atomic<std::uint64_t> g_popdiag_visibility_mask_recovered{0U};\n','')
    text=re.sub(r'extern "C" void rocket_popdiag_visibility_mask_result\(recomp_context\* context\) \{\n    if \(context != nullptr && context->r2 == 0\) \{\n        g_popdiag_visibility_mask_recovered\.fetch_add\(1U, std::memory_order_relaxed\);\n    \}\n\}\n\n','',text)
    text=text.replace('    const std::uint64_t mask_recovered =\n        g_popdiag_visibility_mask_recovered.exchange(0U, std::memory_order_relaxed);\n','')
    text=text.replace('would_drop_at_256_recovered authored_mask_recovered\\n','would_drop_at_256_recovered\\n')
    text=text.replace('would_drop_at_256_recovered=%llu authored_mask_recovered=%llu\\n','would_drop_at_256_recovered=%llu\\n')
    text=text.replace('        static_cast<unsigned long long>(rejected),\n        static_cast<unsigned long long>(mask_recovered));','        static_cast<unsigned long long>(rejected));')
    # Ensure v26 label even if source was still v21/v22.
    for old in ('=== Rocket-R v21 IN-FUNCTION QUEUE-EXPANSION diagnostic session ===','=== Rocket-R v22 NO-SIDE-CULL + QUEUE-RECOVERY diagnostic session ===','=== Rocket-R v25 PRE-RENDER OBJECT-GATE RETENTION + QUEUE-RECOVERY diagnostic session ==='):
        text=text.replace(old,'=== Rocket-R v26 DURABLE-PRESENTATION-IDENTITY + QUEUE-RECOVERY diagnostic session ===')
    if 'presentation-identity=DURABLE-OWNER-V26' not in text:
        text=text.replace('side-plane-cpu-cull=V17.1-VIEWPORT-GUARD','side-plane-cpu-cull=V17.1-VIEWPORT-GUARD authored-submodel-mask=RETAIL presentation-identity=DURABLE-OWNER-V26',1)

    # RecordedEntry gains real model/submodel owner metadata.
    if 'std::uint64_t owner_key = 0U;' not in text:
        anchor='''    bool mtx2_position_valid = false;\n    bool dynamic_gfx = false;\n    std::uint32_t track_token = 0U;\n};'''
        repl='''    bool mtx2_position_valid = false;\n    bool dynamic_gfx = false;\n    std::uint64_t owner_key = 0U;\n    bool owner_valid = false;\n    std::uint32_t track_token = 0U;\n};'''
        if text.count(anchor)!=1: raise RuntimeError('RecordedEntry anchor changed')
        text=text.replace(anchor,repl,1)

    if 'struct OwnerTrack {' not in text:
        anchor='''struct Track {\n    std::uint64_t key = 0U;'''
        insert='''struct OwnerTrack {\n    std::uint64_t key = 0U;\n    std::uint32_t token = 0U;\n    std::uint64_t last_frame = 0U;\n    bool claimed = false;\n};\n\nstruct PendingModelOwner {\n    std::uint64_t key = 0U;\n    std::uint32_t gfx_physical = 0U;\n    bool valid = false;\n};\n\nstruct Track {\n    std::uint64_t key = 0U;'''
        if text.count(anchor)!=1: raise RuntimeError('Track anchor changed')
        text=text.replace(anchor,insert,1)

    if 'std::vector<OwnerTrack> g_owner_tracks;' not in text:
        anchor='std::vector<RecordedEntry> g_entries;\nstd::vector<Track> g_tracks;'
        repl='std::vector<RecordedEntry> g_entries;\nstd::vector<OwnerTrack> g_owner_tracks;\nstd::vector<Track> g_tracks;'
        if text.count(anchor)!=1: raise RuntimeError('global tracks anchor changed')
        text=text.replace(anchor,repl,1)
    if 'thread_local PendingModelOwner g_pending_model_owner{};' not in text:
        anchor='thread_local MatrixMap g_active_matrices;'
        text=text.replace(anchor,anchor+'\nthread_local PendingModelOwner g_pending_model_owner{};',1)
    if 'g_trace_owned_entries' not in text:
        anchor='std::atomic<std::uint64_t> g_trace_entries{0U};'
        text=text.replace(anchor,anchor+'\nstd::atomic<std::uint64_t> g_trace_owned_entries{0U};',1)

    if 'NextPresentationToken()' not in text:
        anchor='[[nodiscard]] std::uint32_t MatrixIdentity(std::uint32_t token,'
        pos=text.find(anchor)
        if pos<0: raise RuntimeError('MatrixIdentity anchor missing')
        text=text[:pos]+alloc_token_code()+text[pos:]
    text=text.replace('''            std::uint32_t token = g_next_token++;\n            if (token == 0U || token == 0xFFFFFFFFU) token = g_next_token++;''','''            const std::uint32_t token = NextPresentationToken();''')

    # Owner tracks expire on the same one-frame continuity rule as heuristic tracks.
    if 'std::erase_if(g_owner_tracks' not in text:
        anchor='''void ExpireTracks() {\n    std::erase_if(g_tracks, [](const Track& track) {'''
        repl='''void ExpireTracks() {\n    std::erase_if(g_owner_tracks, [](const OwnerTrack& track) {\n        return g_frame > track.last_frame + kMaximumTrackAge;\n    });\n    std::erase_if(g_tracks, [](const Track& track) {'''
        if text.count(anchor)!=1: raise RuntimeError('ExpireTracks anchor changed')
        text=text.replace(anchor,repl,1)

    # Insert durable-owner assignment before generic nearest-neighbour candidates.
    marker='void FinalizeTracksAndBindings(MatrixMap& out) {'
    s,e=function_span(text,marker); body=text[s:e]
    if 'v26 durable owner path' not in body:
        anchor='''    ExpireTracks();\n    for (Track& track : g_tracks) track.claimed = false;\n\n    std::vector<Candidate> candidates;'''
        repl='''    ExpireTracks();\n    for (OwnerTrack& track : g_owner_tracks) track.claimed = false;\n    for (Track& track : g_tracks) track.claimed = false;\n\n    // v26 durable owner path: model/submodel entries captured directly from\n    // func_8001ECEC never enter the positional/ordinal matcher. If the exact\n    // GameObject/submodel was present in the immediately previous authored\n    // frame it retains its token; otherwise it starts a fresh presentation\n    // lifetime and snaps to the current authored endpoint.\n    std::vector<bool> entry_claimed(g_entries.size(), false);\n    for (std::size_t ei = 0; ei < g_entries.size(); ++ei) {\n        RecordedEntry& entry = g_entries[ei];\n        if (!entry.owner_valid || entry.owner_key == 0U) continue;\n        auto found = std::find_if(g_owner_tracks.begin(), g_owner_tracks.end(),\n            [&](const OwnerTrack& track) {\n                return track.key == entry.owner_key && !track.claimed &&\n                       track.last_frame + 1U == g_frame;\n            });\n        if (found == g_owner_tracks.end()) {\n            OwnerTrack track{};\n            track.key = entry.owner_key;\n            track.token = NextPresentationToken();\n            track.last_frame = g_frame;\n            track.claimed = true;\n            entry.track_token = track.token;\n            g_owner_tracks.push_back(track);\n        } else {\n            entry.track_token = found->token;\n            found->last_frame = g_frame;\n            found->claimed = true;\n        }\n        entry_claimed[ei] = true;\n        g_trace_owned_entries.fetch_add(1U, std::memory_order_relaxed);\n    }\n\n    std::vector<Candidate> candidates;'''
        if body.count(anchor)!=1: raise RuntimeError('FinalizeTracks start anchor changed')
        body=body.replace(anchor,repl,1)
        body=body.replace('''    for (std::size_t ei = 0; ei < g_entries.size(); ++ei) {\n        const RecordedEntry& entry = g_entries[ei];\n        for (std::size_t ti = 0; ti < g_tracks.size(); ++ti) {''','''    for (std::size_t ei = 0; ei < g_entries.size(); ++ei) {\n        const RecordedEntry& entry = g_entries[ei];\n        if (entry.owner_valid) continue;\n        for (std::size_t ti = 0; ti < g_tracks.size(); ++ti) {''',1)
        old='    std::vector<bool> entry_claimed(g_entries.size(), false);\n'
        # remove the old declaration after candidate ranking, not the new one at top
        idx=body.find(old, body.find('const std::size_t none'))
        if idx<0: raise RuntimeError('Old entry_claimed declaration missing')
        body=body[:idx]+body[idx+len(old):]
        text=text[:s]+body+text[e:]

    # Add actual GameObject/submodel capture hook and consume it in add_render_entry.
    if 'extern "C" void rocket_presentation_model_entry_owner' not in text:
        anchor='extern "C" void rocket_presentation_render_entry(std::uint8_t* rdram,\n                                                   recomp_context* context) {'
        hook='''extern "C" void rocket_presentation_model_entry_owner(std::uint8_t* rdram,\n                                                       recomp_context* context) {\n    g_pending_model_owner = {};\n    if (rdram == nullptr || context == nullptr) return;\n    const std::uint32_t object = static_cast<std::uint32_t>(context->r19);\n    const std::uint32_t submodel = static_cast<std::uint32_t>(context->r16);\n    if (!ValidRange(object, 0xFCU) || !ValidRange(submodel, 0x28U)) return;\n    const std::uint32_t submodels = ReadU32(rdram, object + 0xF4U);\n    const std::uint32_t count = ReadU32(rdram, object + 0xF8U);\n    if (count == 0U || count > 512U || !ValidRange(submodels, count * 0x28U) ||\n        submodel < submodels) return;\n    const std::uint32_t delta = submodel - submodels;\n    if ((delta % 0x28U) != 0U) return;\n    const std::uint32_t index = delta / 0x28U;\n    if (index >= count) return;\n\n    const std::uint32_t gfx = static_cast<std::uint32_t>(context->r4);\n    const GfxSemanticRef gfx_ref = CanonicalGfxRef(rdram, gfx);\n    const std::uint32_t object_class = ReadU32(rdram, object);\n    std::uint64_t key = Mix64(static_cast<std::uint64_t>(Physical(object)) |\n                              (static_cast<std::uint64_t>(index) << 32U));\n    key = Mix64(key ^ (static_cast<std::uint64_t>(Physical(object_class)) << 1U));\n    key = Mix64(key ^ (static_cast<std::uint64_t>(gfx_ref.key) << 17U));\n    if (key == 0U) key = 1U;\n    g_pending_model_owner = {key, Physical(gfx), true};\n}\n\n'''
        if text.count(anchor)!=1: raise RuntimeError('render_entry function anchor changed')
        text=text.replace(anchor,hook+anchor,1)

    # Consume pending owner after canonical gfx is known.
    marker='extern "C" void rocket_presentation_render_entry(std::uint8_t* rdram,'
    s,e=function_span(text,marker); body=text[s:e]
    if 'pending_owner' not in body:
        anchor='''    const GfxSemanticRef gfx_ref = CanonicalGfxRef(rdram, entry.gfx);\n    entry.key = EntryKey('''
        repl='''    const GfxSemanticRef gfx_ref = CanonicalGfxRef(rdram, entry.gfx);\n    const PendingModelOwner pending_owner = std::exchange(\n        g_pending_model_owner, PendingModelOwner{});\n    if (pending_owner.valid &&\n        pending_owner.gfx_physical == Physical(entry.gfx)) {\n        entry.owner_key = pending_owner.key;\n        entry.owner_valid = true;\n    }\n    entry.key = EntryKey('''
        if body.count(anchor)!=1: raise RuntimeError('CanonicalGfxRef render anchor changed')
        body=body.replace(anchor,repl,1); text=text[:s]+body+text[e:]

    # Clear any stale pending owner at authored frame boundaries.
    marker='extern "C" void rocket_presentation_frame_begin(std::uint8_t*,'
    s,e=function_span(text,marker); body=text[s:e]
    if 'g_pending_model_owner = {};' not in body:
        anchor='''    std::scoped_lock lock(g_mutex);\n    RocketPopDiagFlushPreviousFrame();'''
        repl='''    g_pending_model_owner = {};\n    std::scoped_lock lock(g_mutex);\n    RocketPopDiagFlushPreviousFrame();'''
        if body.count(anchor)!=1: raise RuntimeError('frame_begin anchor changed')
        body=body.replace(anchor,repl,1); text=text[:s]+body+text[e:]

    # Shared parent matrices must inherit the same real-owner descriptors. Otherwise
    # an articulated model could still fall back to v6's positional shared-matrix
    # heuristic even though each child now has a durable GameObject identity.
    text=text.replace(
        'AddSharedOccurrence(accumulators, entry.mtx1, 1U, entry.key,',
        'AddSharedOccurrence(accumulators, entry.mtx1, 1U,\n                            entry.owner_valid ? entry.owner_key : entry.key,', 1)
    text=text.replace(
        'AddSharedOccurrence(accumulators, entry.mtx2, 2U, entry.key,',
        'AddSharedOccurrence(accumulators, entry.mtx2, 2U,\n                            entry.owner_valid ? entry.owner_key : entry.key,', 1)

    # Non-destructive DKR-style task sidecar ownership: search immutable DL root,
    # consume stale older entries through the match, and never nuke interpolation history on one mismatch.
    marker='rocket::presentation::TaskIdentityScope::TaskIdentityScope('
    s,e=function_span(text,marker)
    signature=text[s:text.find('{',s)]
    replacement=signature+'''{\n    g_active_matrices.clear();\n    g_active_task_fail_closed = true;\n    const std::uint32_t physical = Physical(display_list_address);\n\n    std::uint32_t task_address = 0U;\n    std::uint32_t context_dl_start = 0U;\n    std::uint32_t context_size = 0U;\n    if (rdram_snapshot != nullptr) {\n        const std::uint32_t task = ReadCurrentGfxTask(rdram_snapshot);\n        if (task != 0U) {\n            task_address = Physical(task);\n            const std::uint32_t buffer = ReadU32(\n                rdram_snapshot, task + kGfxTaskCtxDlStartOffset);\n            context_dl_start = ValidRange(buffer, 8U) ? Physical(buffer) : 0U;\n            context_size = ReadU32(rdram_snapshot, task + kGfxTaskCtxSizeOffset);\n        }\n    }\n\n    std::scoped_lock lock(g_mutex);\n    if (g_submitted.empty()) {\n        g_trace_task_misses.fetch_add(1U, std::memory_order_relaxed);\n        return;\n    }\n\n    // DKR-style durable task ownership: the immutable OSTask display-list root\n    // is authoritative. Mutable task/context parity can already have flipped by\n    // decode time, so it is diagnostic only and must never invalidate a valid\n    // sidecar or clear good object identity history.\n    const auto matching = std::find_if(\n        g_submitted.begin(), g_submitted.end(),\n        [physical](const SubmittedFrame& candidate) {\n            return candidate.display_list == physical;\n        });\n    if (matching == g_submitted.end()) {\n        g_trace_task_misses.fetch_add(1U, std::memory_order_relaxed);\n        g_trace_sidecar_mismatches.fetch_add(1U, std::memory_order_relaxed);\n        if (TraceEnabled()) {\n            const SubmittedFrame& expected = g_submitted.front();\n            std::fprintf(stderr,\n                "[rocket-presentation] sidecar mismatch: expected dl=%06X task=%06X ctx=%06X/%u got dl=%06X task=%06X ctx=%06X/%u; interpolation disabled for this task\\n",\n                expected.display_list, expected.task_address,\n                expected.context_dl_start, expected.context_size, physical,\n                task_address, context_dl_start, context_size);\n        }\n        return;\n    }\n\n    SubmittedFrame frame = std::move(*matching);\n    g_submitted.erase(g_submitted.begin(), std::next(matching));\n    g_active_matrices = std::move(frame.matrices);\n    g_trace_task_matches.fetch_add(1U, std::memory_order_relaxed);\n}'''
    text=text[:s]+replacement+text[e:]

    # Queue ownership loss is a real discontinuity; reset durable owner tracks there too.
    text=text.replace('''        g_tracks.clear();\n        g_shared_matrix_tracks.clear();\n        if (TraceEnabled()) {\n            std::fprintf(stderr,\n                "[rocket-presentation] sidecar queue overflow; history reset\\n");''','''        g_owner_tracks.clear();\n        g_tracks.clear();\n        g_shared_matrix_tracks.clear();\n        if (TraceEnabled()) {\n            std::fprintf(stderr,\n                "[rocket-presentation] sidecar queue overflow; history reset\\n");''')
    text=text.replace('''        if (g_empty_frames >= 2U) {\n            g_tracks.clear();\n            g_shared_matrix_tracks.clear();\n        }''','''        if (g_empty_frames >= 2U) {\n            g_owner_tracks.clear();\n            g_tracks.clear();\n            g_shared_matrix_tracks.clear();\n        }''')

    # Trace line: expose that the real-owner path is actually being exercised.
    if 'owned=%llu' not in text:
        text=text.replace(
            '[rocket-presentation] frame=%llu entries=%llu matched=%llu new=%llu ',
            '[rocket-presentation] frame=%llu entries=%llu owned=%llu matched=%llu new=%llu ', 1)
        text=text.replace('''        static_cast<unsigned long long>(g_trace_entries.exchange(0U)),\n        static_cast<unsigned long long>(g_trace_matches.exchange(0U)),''','''        static_cast<unsigned long long>(g_trace_entries.exchange(0U)),\n        static_cast<unsigned long long>(g_trace_owned_entries.exchange(0U)),\n        static_cast<unsigned long long>(g_trace_matches.exchange(0U)),''')

    if text!=original: write_text(path,text,bom,nl)

def patch_header(path: Path):
    text,bom,nl=read_text(path)
    if 'rocket_presentation_model_entry_owner' not in text:
        anchor='extern "C" void rocket_presentation_render_entry(std::uint8_t* rdram,\n                                                   recomp_context* context);'
        insert='extern "C" void rocket_presentation_model_entry_owner(std::uint8_t* rdram,\n                                                       recomp_context* context);\n'+anchor
        if text.count(anchor)!=1: raise RuntimeError('header render hook anchor changed')
        text=text.replace(anchor,insert,1); write_text(path,text,bom,nl)

def patch_policy(path: Path):
    text,bom,nl=read_text(path); policy=json.loads(text); hooks=policy.setdefault('functionHooks',[])
    hooks[:]=[h for h in hooks if OWNER_HOOK_MARKER not in h.get('text','')]
    hooks.append({
        'function':'func_8001ECEC','beforeVram':'0x8001F084',
        'text':'extern void rocket_presentation_model_entry_owner(uint8_t*, recomp_context*); rocket_presentation_model_entry_owner(rdram, ctx);',
        'reason':'Capture the actual GameObject/submodel owner immediately before its add_render_entry call so visible model interpolation uses durable authored ownership instead of positional RenderEntry guessing.'
    })
    write_text(path,json.dumps(policy,indent=2)+'\n',bom,nl)

def patch_oneclick(path: Path):
    text,bom,nl=read_text(path)
    lines=text.replace('\r\n','\n').split('\n')
    out=[]
    for line in lines:
        # v25 must never be regenerated after N64Recomp. Remove both its invocation
        # and its explanatory comments. Also retire the older v23 bypass.
        if 'patch_prerender_object_gate_v25_generated.py' in line: continue
        if 'Graphics v25:' in line: continue
        if 'pre-render whole-object gate' in line: continue
        if 'model-render callsites' in line: continue
        if 'Retail submodel mask + frustum + r7 distance remain authoritative' in line: continue
        if 'patch_visibility_retention_v23_generated.py' in line: continue
        if 'Graphics v23:' in line or 'authored submodel visibility-mask reject' in line: continue
        if 'viewport-aware forward frustum, r7 distance/fade and v21 queue recovery remain active' in line: continue
        out.append(line)
    text='\n'.join(out)
    if 'patch_render_queue_expansion_v21_generated.py' not in text:
        raise RuntimeError('v21 queue hook missing from OneClickBuild.ps1')
    if 'patch_prerender_object_gate_v25_generated.py' in text:
        raise RuntimeError('v25 object-gate patcher is still scheduled in OneClickBuild.ps1')
    write_text(path,text,bom,nl)

def patch_self_check(path: Path):
    text,bom,nl=read_text(path)
    scan='    scan_checked_source(root)'
    end=text.find(scan)
    if end<0: raise RuntimeError('self_check scan_checked_source anchor missing')
    starts=[text.rfind('    # v26:',0,end),text.rfind('    # v25:',0,end),text.rfind('    # v24:',0,end),text.rfind('    # v23:',0,end),text.rfind('    # v22:',0,end),text.rfind('    # v21:',0,end),text.rfind('    # v19.2:',0,end)]
    start=next((x for x in starts if x>=0),-1)
    if start<0: raise RuntimeError('graphics self-check block start not found')
    text=text[:start]+SELF_CHECK_V26+text[end:]
    ast.parse(text); write_text(path,text,bom,nl)

def strip_v25_generated(root: Path):
    out=root/'runtime-recomp'/'RecompiledFuncs'
    if not out.is_dir(): return
    for path in out.glob('funcs_*.c'):
        text,bom,nl=read_text(path)
        original=text
        for func,call,branch in V25_TARGETS:
            begin=f'    // === ROCKET-R GRAPHICS V25 PRE-RENDER OBJECT GATE RETENTION BEGIN: {func} ===\n'
            endm=f'    // === ROCKET-R GRAPHICS V25 PRE-RENDER OBJECT GATE RETENTION END: {func} ===\n'
            while begin in text:
                b=text.find(begin); e=text.find(endm,b)
                if e<0: raise RuntimeError(f'corrupt v25 generated marker in {path}: {func}')
                e+=len(endm); text=text[:b]+text[e:]
        if text!=original:
            write_text(path,text,bom,nl)
            print(f'[OK] Restored retail pre-render object gate: {path}')

    # Prove all three original call/branch pairs survive and the v25 continuation is gone.
    all_text='\n'.join(p.read_text(encoding='utf-8-sig') for p in out.glob('funcs_*.c'))
    if 'ROCKET-R GRAPHICS V25 PRE-RENDER OBJECT GATE RETENTION BEGIN' in all_text or \
       'ctx->r2 = 1; // v25: let downstream submodel/frustum/distance logic decide' in all_text:
        raise RuntimeError('v25 generated object-gate override remains after rollback')
    for func,call,branch in V25_TARGETS:
        hits=[]
        marker=f'RECOMP_FUNC void {func}(uint8_t* rdram, recomp_context* ctx)'
        for path in out.glob('funcs_*.c'):
            t=path.read_text(encoding='utf-8-sig')
            if marker in t: hits.append((path,t))
        if not hits: continue
        if len(hits)!=1: raise RuntimeError(f'Expected one generated {func}, found {len(hits)}')
        _,t=hits[0]; fs,fe=function_span(t,marker); body=t[fs:fe]
        if body.count('func_8001E824(rdram, ctx);')!=1:
            raise RuntimeError(f'Retail func_8001E824 call not restored exactly once in {func}')
        if f'// {call}: jal' not in body or f'// {branch}: beq' not in body:
            raise RuntimeError(f'Retail object-gate call/branch addresses missing in {func}')

def strip_v23_generated(root: Path):
    out=root/'runtime-recomp'/'RecompiledFuncs'
    if not out.is_dir(): return
    for path in out.glob('funcs_*.c'):
        text,bom,nl=read_text(path)
        if 'ROCKET-R GRAPHICS V23 AUTHORED SUBMODEL VISIBILITY RETENTION BEGIN' not in text: continue
        begin=text.find(V23_BEGIN)
        end=text.find(V23_END,begin)
        if begin<0 or end<0: raise RuntimeError(f'corrupt v23 generated marker in {path}')
        end+=len(V23_END)
        text=text[:begin]+text[end:]
        write_text(path,text,bom,nl)
        print(f'[OK] Removed disproven v23 generated visibility bypass: {path}')

def patch_generated_owner_hook(root: Path):
    out=root/'runtime-recomp'/'RecompiledFuncs'
    if not out.is_dir(): return
    hits=[]
    for path in out.glob('funcs_*.c'):
        text,bom,nl=read_text(path)
        marker='RECOMP_FUNC void func_8001ECEC(uint8_t* rdram, recomp_context* ctx)'
        if marker in text: hits.append((path,text,bom,nl))
    if not hits: return
    if len(hits)!=1: raise RuntimeError(f'Expected one generated func_8001ECEC, found {len(hits)}')
    path,text,bom,nl=hits[0]
    fs,fe=function_span(text,'RECOMP_FUNC void func_8001ECEC')
    body=text[fs:fe]
    if OWNER_HOOK_MARKER in body:
        print(f'[OK] Generated v26 owner hook already present: {path}')
        return
    anchor=(
        '    // 0x8001F084: jal         0x8008B24C\n'
        '    // 0x8001F088: addu        $a2, $s5, $zero\n'
    )
    if body.count(anchor)!=1:
        raise RuntimeError('Could not uniquely locate generated func_8001ECEC add_render_entry call')
    injection=(
        '    // === ROCKET-R GRAPHICS V26 DURABLE MODEL OWNER BEGIN ===\n'
        '    // Capture s3=GameObject and s0=current Submodel before add_render_entry overwrites argument registers.\n'
        '    extern void rocket_presentation_model_entry_owner(uint8_t*, recomp_context*);\n'
        '    rocket_presentation_model_entry_owner(rdram, ctx);\n'
        '    // === ROCKET-R GRAPHICS V26 DURABLE MODEL OWNER END ===\n'
    )
    body=body.replace(anchor,injection+anchor,1)
    text=text[:fs]+body+text[fe:]
    write_text(path,text,bom,nl)
    print(f'[OK] Patched current generated model-owner hook: {path}')

def verify(root: Path):
    p=(root/'src/presentation_identity.cpp').read_text(encoding='utf-8-sig')
    h=(root/'src/presentation_identity.hpp').read_text(encoding='utf-8-sig')
    o=(root/'scripts/OneClickBuild.ps1').read_text(encoding='utf-8-sig')
    sc=(root/'scripts/self_check.py').read_text(encoding='utf-8-sig')
    c=(root/'src/widescreen_culling.cpp').read_text(encoding='utf-8-sig')
    g=(root/'src/graphics_enhancements.cpp').read_text(encoding='utf-8-sig')
    policy=json.loads((root/'runtime-recomp/rocket.us.recomp-policy.json').read_text(encoding='utf-8-sig'))
    hooks=[x for x in policy.get('functionHooks',[]) if x.get('function')=='func_8001ECEC' and str(x.get('beforeVram','')).upper()=='0X8001F084' and OWNER_HOOK_MARKER in x.get('text','')]
    required=('ROCKET-R GRAPHICS V21 IN-FUNCTION RENDER QUEUE EXPANSION BEGIN','PendingModelOwner','OwnerTrack','owner_key','v26 durable owner path','presentation-identity=DURABLE-OWNER-V26')
    for tok in required:
        if tok not in p: raise RuntimeError('v26 source token missing: '+tok)
    if 'v16 viewport-locked FOV/aspect guard active' not in c or 'kDisableCpuSidePlanesBits = 0x7F7FFFFFU' in c:
        raise RuntimeError('v17.1 viewport-aware frustum guard is not active')
    if 'static_cast<std::uint32_t>(context->r7)' not in g or 's.draw_distance_multiplier' not in g:
        raise RuntimeError('r7 Draw Distance path is not intact')
    for bad in ('pre-render-object-gate=RECOVERED-V25','rocket_popdiag_object_gate_result',
                'object_gate_recovered','patch_prerender_object_gate_v25_generated.py'):
        if bad in p + o: raise RuntimeError('retired v25 state still active: '+bad)
    if 'pre-render-object-gate=RETAIL' not in p:
        raise RuntimeError('v26 diagnostics do not confirm retail pre-render object gate')
    if OWNER_HOOK_MARKER not in h or len(hooks)!=1: raise RuntimeError('v26 actual-owner policy hook missing/duplicated')
    if 'patch_render_queue_expansion_v21_generated.py' not in o: raise RuntimeError('v21 queue hook lost')
    if 'patch_visibility_retention_v23_generated.py' in o: raise RuntimeError('v23 visibility bypass still scheduled')
    if '# v26: keep the proven v21 queue recovery' not in sc: raise RuntimeError('v26 self-check missing')
    if 'authored-submodel-mask=RECOVERED-V23' in p: raise RuntimeError('v23 diagnostic state still active')
    generated_owner_seen=False
    generated_dir=root/'runtime-recomp'/'RecompiledFuncs'
    for f in generated_dir.glob('funcs_*.c') if generated_dir.is_dir() else []:
        ft=f.read_text(encoding='utf-8-sig')
        if 'ROCKET-R GRAPHICS V23 AUTHORED SUBMODEL VISIBILITY RETENTION BEGIN' in ft:
            raise RuntimeError('v23 generated bypass remains: '+str(f))
        if 'ROCKET-R GRAPHICS V25 PRE-RENDER OBJECT GATE RETENTION BEGIN' in ft or \
           'ctx->r2 = 1; // v25: let downstream submodel/frustum/distance logic decide' in ft:
            raise RuntimeError('v25 generated object-gate override remains: '+str(f))
        if 'RECOMP_FUNC void func_8001ECEC' in ft:
            generated_owner_seen = OWNER_HOOK_MARKER in ft
    if generated_dir.is_dir() and not generated_owner_seen:
        raise RuntimeError('current generated func_8001ECEC lacks v26 owner hook')
    print('[OK] v26 durable presentation identity verification PASS.')

def run_optional_checks(root: Path):
    selfcheck=root/'scripts'/'self_check.py'
    if selfcheck.is_file():
        cp=subprocess.run([sys.executable,str(selfcheck)],cwd=str(root))
        if cp.returncode: raise RuntimeError('source self-check failed')
    ninja=root/'build'/'windows'/'build.ninja'
    if ninja.is_file():
        exe=shutil.which('ninja')
        if exe:
            cp=subprocess.run([exe,'-C',str(ninja.parent)],cwd=str(root))
            if cp.returncode: raise RuntimeError('incremental Windows build failed')

def main():
    ap=argparse.ArgumentParser(); ap.add_argument('--root',required=True); ap.add_argument('--verify-only',action='store_true'); ap.add_argument('--rollback-latest',action='store_true'); ns=ap.parse_args()
    root=Path(ns.root).resolve()
    if ns.rollback_latest:
        pointer=root/'build'/'repair-backups'/'LAST-GRAPHICS-V26-BACKUP.txt'
        if not pointer.is_file(): raise RuntimeError('No v26 backup pointer exists yet')
        base=Path(pointer.read_text(encoding='utf-8').strip())
        if not base.is_dir(): raise RuntimeError('Latest v26 backup directory is missing: '+str(base))
        restore_backup(root,base)
        print('[OK] Restored the repository files captured immediately before the latest v26 install.')
        return
    required=['src/presentation_identity.cpp','src/presentation_identity.hpp','src/widescreen_culling.cpp','src/graphics_enhancements.cpp','scripts/OneClickBuild.ps1','scripts/self_check.py','runtime-recomp/rocket.us.recomp-policy.json','scripts/patch_render_queue_expansion_v21_generated.py']
    for rel in required:
        if not (root/rel).is_file(): raise RuntimeError('Required Rocket-R file missing: '+rel)
    backup_dir=None
    try:
        if not ns.verify_only:
            files=list(required)
            for optional in ('scripts/patch_prerender_object_gate_v25_generated.py',
                             'scripts/patch_visibility_retention_v23_generated.py'):
                if (root/optional).is_file(): files.append(optional)
            generated_dir=root/'runtime-recomp'/'RecompiledFuncs'
            if generated_dir.is_dir(): files += [str(x.relative_to(root)) for x in generated_dir.glob('funcs_*.c')]
            backup_dir=backup(root,files)
            patch_presentation(root/'src/presentation_identity.cpp')
            patch_header(root/'src/presentation_identity.hpp')
            patch_policy(root/'runtime-recomp/rocket.us.recomp-policy.json')
            patch_oneclick(root/'scripts/OneClickBuild.ps1')
            patch_self_check(root/'scripts/self_check.py')
            strip_v25_generated(root)
            strip_v23_generated(root)
            patch_generated_owner_hook(root)
            # Retired patchers are backed up above but must not remain live in the source tree.
            for retired in ('scripts/patch_prerender_object_gate_v25_generated.py',
                            'scripts/patch_visibility_retention_v23_generated.py'):
                rp=root/retired
                if rp.is_file(): rp.unlink()
        verify(root)
        run_optional_checks(root)
    except Exception:
        if not ns.verify_only and backup_dir is not None:
            restore_backup(root,backup_dir)
        raise
    print('[OK] Graphics v26 installed: durable GameObject/submodel identities + non-destructive task sidecars.')
    print('[OK] v25 pre-render object-gate override removed; retail func_8001E824 behavior restored.')
    print('[OK] v21 queue recovery retained; viewport frustum and r7 draw-distance/fade remain authoritative.')

if __name__=='__main__':
    try: main()
    except Exception as exc:
        print('Graphics v26 migration failed: '+str(exc),file=sys.stderr)
        raise SystemExit(1)
