from pathlib import Path
import json, shutil, sys
POINTER=Path('build/repair-backups/LAST-V30-TO-V29-RECOVERY-BACKUP.txt')

def locate_root():
    c=[]
    if len(sys.argv)>1: c.append(Path(sys.argv[1]))
    c += [Path.cwd(),Path(__file__).resolve().parent]
    for x in c:
        r=x.resolve()
        if (r/'scripts/OneClickBuild.ps1').is_file(): return r
    raise RuntimeError('Could not locate Rocket-R root.')

def main():
    root=locate_root(); ptr=root/POINTER
    if not ptr.is_file(): raise RuntimeError('No recovery backup pointer found.')
    backup=Path(ptr.read_text(encoding='utf-8-sig').strip())
    manifest=json.loads((backup/'manifest.json').read_text(encoding='utf-8'))
    for rel in manifest.get('backed_up',[]): 
        src=backup/rel; dst=root/rel; dst.parent.mkdir(parents=True,exist_ok=True); shutil.copy2(src,dst)
    for rel in manifest.get('absent',[]):
        p=root/rel
        if p.exists(): p.unlink()
    print(f'[OK] Restored state from before v30-to-v29 recovery: {backup}')

if __name__=='__main__':
    try: main()
    except Exception as e:
        print('Recovery rollback FAILED: '+str(e),file=sys.stderr); raise SystemExit(1)
