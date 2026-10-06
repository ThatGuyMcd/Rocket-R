// Read-only Windows x64 diagnostic for the Rocket Colour Studio 0.4.0 / 0.3.0.
// Reports formerly omitted registered parts, including RGB constants and solids.
// C# 5 compatible; Windows PowerShell's installed .NET Framework compiles it.
// No write, injection, remote-thread, suspend, or process-termination APIs.
using System;
using System.Collections.Generic;
using System.ComponentModel;
using System.Diagnostics;
using System.Runtime.InteropServices;
using System.Text;
using System.Threading;

namespace RocketColourLiveCheck
{
    public sealed class Reader : IDisposable
    {
        const uint QueryAndRead = 0x0410; // QUERY_INFORMATION | VM_READ only.
        const uint Commit = 0x1000, Private = 0x20000;
        const int GameRamBytes = 0x800000;
        const int ModStart = 0x1000000; // Published runtime: guest 0x81000000.
        const int ScanEnd = 0x8000000;  // Bound: guest 0x88000000, not all process memory.
        const uint GuestStart = 0x80000000;
        const uint PlayerSlot = 0x800AAF5C;
        IntPtr handle;
        readonly StringBuilder text = new StringBuilder();
        bool currentVersion = true;
        string VersionLabel { get { return currentVersion ? "v0.4.0" : "v0.3.0"; } }
        int DiagnosticOffset { get { return currentVersion ? 11572 : 8100; } }
        int DiagnosticFields { get { return currentVersion ? 32 : 24; } }
        uint DiagnosticMagic { get { return currentVersion ? 0x52435334U : 0x52435333U; } }
        uint DiagnosticVersion { get { return currentVersion ? 0x00040000U : 0x00030000U; } }

        // Explicit 64-bit layout; also valid on versions predating PartitionId.
        [StructLayout(LayoutKind.Explicit, Size = 48)]
        struct Region
        {
            [FieldOffset(0)] public ulong Base;
            [FieldOffset(8)] public ulong AllocationBase;
            [FieldOffset(16)] public uint AllocationProtect;
            [FieldOffset(24)] public ulong Size;
            [FieldOffset(32)] public uint State;
            [FieldOffset(36)] public uint Protect;
            [FieldOffset(40)] public uint Type;
        }
        [DllImport("kernel32.dll", SetLastError = true)]
        static extern IntPtr OpenProcess(uint access, bool inherit, int pid);
        [DllImport("kernel32.dll", SetLastError = true)]
        static extern bool ReadProcessMemory(IntPtr process, IntPtr address,
            [Out] byte[] buffer, UIntPtr size, out UIntPtr bytesRead);
        [DllImport("kernel32.dll", SetLastError = true)]
        static extern UIntPtr VirtualQueryEx(IntPtr process, IntPtr address,
            out Region region, UIntPtr size);
        [DllImport("kernel32.dll", SetLastError = true)]
        static extern bool CloseHandle(IntPtr handle);

        public Reader(int pid)
        {
            if (IntPtr.Size != 8) throw new InvalidOperationException("Run the 64-bit START-LIVE-CHECK.cmd launcher.");
            handle = OpenProcess(QueryAndRead, false, pid);
            if (handle == IntPtr.Zero) throw new Win32Exception(Marshal.GetLastWin32Error(),
                "Could not open Rocket-R for read-only inspection. The game may have closed, or be running as administrator.");
        }
        public void Dispose()
        {
            if (handle != IntPtr.Zero) { CloseHandle(handle); handle = IntPtr.Zero; }
        }
        byte[] Read(ulong address, int count)
        {
            if (count < 0 || count > 0x1000000 || address > Int64.MaxValue)
                throw new ArgumentOutOfRangeException("count/address");
            byte[] b = new byte[count]; UIntPtr got;
            if (!ReadProcessMemory(handle, new IntPtr((long)address), b, new UIntPtr((uint)count), out got)
                || got.ToUInt64() != (ulong)count) return null;
            return b;
        }
        static uint W(byte[] b, int offset)
        {
            if (b == null || offset < 0 || offset > b.Length - 4) throw new ArgumentOutOfRangeException("offset");
            return BitConverter.ToUInt32(b, offset);
        }
        static string H(uint a) { return "0x" + a.ToString("X8"); }
        static bool Range(uint a, uint n)
        { return (a & 3) == 0 && a >= 0x80000400 && n <= GameRamBytes && (ulong)a + n <= 0x80800000UL; }
        static uint Direct(uint a)
        {
            if (a >= 0x80000400 && a < 0x80800000) return a;
            if (a >= 0xA0000400 && a < 0xA0800000) return a - 0x20000000;
            if (a >= 0x400 && a < GameRamBytes) return a | GuestStart;
            return 0;
        }
        static uint G(byte[] b, uint a) { return W(b, checked((int)(a - GuestStart))); }
        static uint MaterialAddress(byte[] ram, uint token)
        {
            if (token >> 24 != 1) return Direct(token);
            uint b = Direct(G(ram, 0x800AAF7C)), off = token & 0xFFFFFF;
            if (b == 0 || (ulong)b + off > UInt32.MaxValue) return 0;
            uint a = b + off;
            return Range(a, 72) ? a : 0;
        }
        static bool Red(uint rgba)
        {
            uint r=rgba>>24,g=(rgba>>16)&255,b=(rgba>>8)&255;
            uint lo=Math.Min(g,b),diff=g>b?g-b:b-g;
            return (rgba&255)!=0 && r>=8 && r>g && r>b && (r-lo)*4>=r && diff*2<=r-lo;
        }
        static uint Half(byte[] ram,uint a) { return (G(ram,a&~3U)>>(int)((2-(a&2))*8))&65535; }
        static uint Expand16(uint p)
        { return (((p>>11)&31)*255/31<<24)|(((p>>6)&31)*255/31<<16)|(((p>>1)&31)*255/31<<8)|((p&1)!=0?255U:0U); }
        static string RGB(uint rgba)
        { return (rgba>>24) + "," + ((rgba>>16)&255) + "," + ((rgba>>8)&255) + " alpha=" + (rgba&255); }
        static bool Matches(byte[] b, int off, byte[] signature)
        {
            if (b == null || off < 0 || off > b.Length-signature.Length) return false;
            for (int j=0; j<signature.Length; ++j) if (b[off+j] != signature[j]) return false;
            return true;
        }
        static bool GameSignature(byte[] first, byte[] second)
        {
            return first != null && second != null && first.Length >= 16 && second.Length >= 16
                && W(first,0) == 0x27BDFFE8 && W(first,4) == 0x3C02800B
                && W(first,8) == 0xAFB00010 && W(first,12) == 0x8C50F4F8
                && W(second,0) == 0x27BDFBD0 && W(second,4) == 0xAFB00420
                && W(second,8) == 0x00808021 && W(second,12) == 0xAFB10424;
        }
        List<ulong> FindGameAllocations()
        {
            var found = new List<ulong>(); var checkedBases = new HashSet<ulong>();
            ulong cursor = 0; int regions = 0;
            // Query region metadata only. Read only two tiny Rocket instruction signatures
            // at fixed offsets inside sufficiently large private allocations.
            while (cursor < 0x00007FFFFFFF0000UL && regions++ < 250000)
            {
                Region r;
                if (VirtualQueryEx(handle,new IntPtr((long)cursor),out r,new UIntPtr(48)).ToUInt64() == 0) break;
                ulong next = r.Base + r.Size;
                if (next <= cursor) break;
                if (r.State == Commit && r.Type == Private && (r.Protect & 0x101) == 0
                    && r.Size >= 0x1000000 && r.AllocationBase > 0 && checkedBases.Add(r.AllocationBase))
                {
                    if (GameSignature(Read(r.AllocationBase+0x92050,16),Read(r.AllocationBase+0x922C4,16)))
                        found.Add(r.AllocationBase);
                }
                cursor = next;
            }
            text.AppendLine("Virtual memory regions queried: " + regions);
            text.AppendLine("Verified Rocket RDRAM allocations: " + found.Count);
            return found;
        }
        List<uint> FindMod(ulong ramBase, byte[] signature)
        {
            var found = new List<uint>(); const int chunk = 0x100000;
            int unreadable = 0;
            // The runtime leaves code instructions unchanged in guest memory. Only
            // R_MIPS_32 data relocations are written there; neither supported payload has such code relocations.
            for (int offset=ModStart; offset<ScanEnd; offset+=chunk)
            {
                int n = Math.Min(chunk+signature.Length-1, ScanEnd-offset);
                byte[] b = Read(ramBase+(uint)offset,n);
                if (b == null) { ++unreadable; continue; }
                for (int p=0; p<chunk && p<=b.Length-signature.Length; p+=16)
                    if (b[p] == signature[0] && b[p+1] == signature[1]
                        && b[p+2] == signature[2] && b[p+3] == signature[3] && Matches(b,p,signature))
                        found.Add(GuestStart+(uint)(offset+p));
                if (found.Count != 0) break;
            }
            text.AppendLine(VersionLabel+" full-code matches in guest memory: " + found.Count);
            if (found.Count == 0)
                text.AppendLine("Search bound: guest 0x81000000..0x88000000; unreadable chunks=" + unreadable +
                    ". A missing match is NOT proof that an arbitrary different version is unloaded.");
            return found;
        }
        static bool ConstantUsed(uint a,uint b,uint selector) {
            return ((a>>20)&15)==selector || ((b>>28)&15)==selector || ((a>>15)&31)==selector ||
                ((b>>15)&7)==selector || ((a>>5)&15)==selector || ((b>>24)&15)==selector ||
                (a&31)==selector || ((b>>6)&7)==selector;
        }
        static bool IsSolid(byte[] r,uint a,uint size) {
            return size==72 && G(r,a+8)==0xFCFFFFFF && G(r,a+12)==0xFF0E783F &&
                G(r,a+16)==0xD9FFFFFF && G(r,a+20)==0x00200404 && G(r,a+24)==0xD7000000 && G(r,a+28)==0 &&
                G(r,a+32)==0xDB0A0000 && G(r,a+40)==0xDB0A0004 &&
                G(r,a+48)==0xDB0A0018 && G(r,a+56)==0xDB0A001C &&
                G(r,a+36)==G(r,a+44) && G(r,a+52)==G(r,a+60);
        }
        void DescribeMaterial(byte[] ram,uint call,uint owner,uint modelStart,ref int emitted)
        {
            if(emitted++>=80)return;
            uint token=G(ram,call+4),current=Direct(token),original=token;bool clone=false;
            if(current>=0x800F0020 && Range(current-32,32))
            {
                uint h=current-32;
                uint tag=G(ram,h);
                clone=(tag==0x52435333 || tag==0x52435334) && G(ram,h+4)==~tag && G(ram,h+8)==call &&
                    G(ram,h+12)==owner && G(ram,h+16)==modelStart && G(ram,h+28)==(current^call^owner^tag);
                if(clone)original=G(ram,h+20);
            }
            uint source=MaterialAddress(ram,original);
            text.AppendLine("    material call="+H(call)+" target="+H(token)+" original="+H(original)+" resolved="+H(source)+" private_copy="+clone);
            if(!Range(source,8))return;
            uint size=0;bool rgbaLut=false,otherLut=false;
            var opcodes=new List<string>();
            for(uint j=0;j<256;++j)
            {
                uint pc=source+j*8;if(!Range(pc,8))break;
                uint w=G(ram,pc),v=G(ram,pc+4),op=w>>24;
                if(opcodes.Count<20)opcodes.Add(H(w));
                if(w==0xE3001001) {if(v==0x8000)rgbaLut=true;else if(v!=0)otherLut=true;}
                if(op==0xDF){size=(j+1)*8;break;}
                if(op==0xDE || op<0xD0)break;
            }
            text.AppendLine("      bounded list bytes="+size+" RGBA16_TLUT="+rgbaLut+" other_TLUT="+otherLut+" first opcodes="+String.Join(",",opcodes.ToArray()));
            if(size==0)return;
            bool envUsed=false,primUsed=false;
            for(uint j=0;j<size-8;j+=8) if(G(ram,source+j)>>24==0xFC) {
                uint a=G(ram,source+j),b=G(ram,source+j+4);
                envUsed=envUsed||ConstantUsed(a,b,5);primUsed=primUsed||ConstantUsed(a,b,3);
                text.AppendLine("      combine="+H(a)+"/"+H(b));
            }
            bool solid=IsSolid(ram,source,size);
            text.AppendLine("      solid_envelope="+solid+" ENV_RGB_used="+envUsed+" PRIM_RGB_used="+primUsed);
            for(uint j=0;j<size-8;j+=8) {
                uint op=G(ram,source+j),v=G(ram,source+j+4);
                if(op==0xFB000000 || op>>24==0xFA || op==0xDB0A0000 || op==0xDB0A0018) {
                    uint cv=clone && Range(current+j,8)?G(ram,current+j+4):v;
                    string kind=op==0xFB000000?"ENV":op>>24==0xFA?"PRIM":op==0xDB0A0000?"diffuse":"ambient";
                    text.AppendLine("      "+kind+" original="+RGB(v)+" copy="+RGB(cv)+" red_RGB="+Red(v|255U));
                }
            }
            int transfers=0;
            for(uint i=0;i<size-8 && transfers<32;i+=8)
            {
                if(G(ram,source+i)>>24!=0xFD)continue;
                ++transfers;
                uint bytes=0,kind=0,fmt=99,siz=99;
                for(uint j=i+8;j<size-8;j+=8)
                {
                    uint w=G(ram,source+j),v=G(ram,source+j+4),op=w>>24;
                    if(op==0xFD)break;
                    if(op==0xF0){kind=1;bytes=(((v>>14)&0x3FF)+1)*2;}
                    if(op==0xF3){kind=2;bytes=(((v>>12)&0xFFF)+1)*2;}
                    if(op==0xF5 && kind==2 && ((v>>24)&7)!=7){fmt=(w>>21)&7;siz=(w>>19)&3;break;}
                }
                uint data=MaterialAddress(ram,G(ram,source+i+4));
                uint dst=clone && Range(current+i,8)?MaterialAddress(ram,G(ram,current+i+4)):0;
                bool color=kind==1?rgbaLut&&!otherLut:kind==2&&fmt==0&&(siz==2||siz==3);
                uint red=0,changed=0,firstOriginal=0,firstChanged=0;
                if(color && bytes>0 && bytes<=4096 && Range(data,bytes))
                {
                    uint stride=kind==2&&siz==3?4U:2U;
                    for(uint k=0;k+stride<=bytes;k+=stride)
                    {
                        uint a=stride==4?G(ram,data+k):Expand16(Half(ram,data+k));
                        if(Red(a))++red;
                        if(dst!=0 && Range(dst,bytes))
                        {
                            uint b=stride==4?G(ram,dst+k):Expand16(Half(ram,dst+k));
                            if(a!=b){if(changed==0){firstOriginal=a;firstChanged=b;}++changed;}
                        }
                    }
                }
                text.AppendLine("      transfer="+transfers+" kind="+(kind==1?"TLUT":kind==2?"pixel block":"unknown")+
                    " format="+fmt+" size="+siz+" bytes="+bytes+" original="+H(data)+" copy="+H(dst)+
                    " color_target="+color+" red_entries="+red+" changed_entries="+changed);
                if(changed>0)text.AppendLine("        first changed colour: "+RGB(firstOriginal)+" -> "+RGB(firstChanged));
            }
        }
        void DescribeModels(byte[] ram)
        {
            uint player=Direct(G(ram,PlayerSlot));
            text.AppendLine("Player pointer: "+H(player)+"; segment-1 base="+H(G(ram,0x800AAF7C)));
            uint heap=G(ram,0x800E48AC), top=G(ram,0x800E48B0);
            text.AppendLine("Main pool cursor="+H(heap)+"; second-pool head="+H(top));
            bool allocationPossible=(heap&7)==0 && heap>=0x800F0000 && heap<=0x80600000-32
                && top>=heap && top<=0x80800000 && top-heap>=32+8192;
            text.AppendLine(VersionLabel+" clone allocator pointer/space guards pass: "+allocationPossible);
            if (!Range(player,0x28C)) { text.AppendLine("Player not present/valid in this sample."); return; }
            // Include the formerly uninspected part slots even when the old
            // package is running. Read-only reporting does NOT recolour them.
            uint[] offsets={0,0x268,0x26C,0x270,0x274,0x278,0x27C,0x280,0x284,0x288}; var seen=new HashSet<uint>();
            int total=0, materialRecords=0;
            foreach (uint off in offsets)
            {
                uint model=off==0?player:Direct(G(ram,player+off));
                text.AppendLine("  "+(off==0?"player root":"player +0x"+off.ToString("X"))+" => "+H(model));
                if (!seen.Add(model) || !Range(model,0xFC)) { text.AppendLine("    duplicate/null/invalid"); continue; }
                bool proof=false;
                if(off>=0x26C && off<=0x27C && Range(model,0x23C)) {
                    uint parent=Direct(G(ram,model+0x10)),index=G(ram,model+0x230),special=Direct(G(ram,model+0x238));
                    proof=(parent==player && index==(off-0x268)/4) || (off==0x278 && special==player);
                    text.AppendLine("    registered_part parent="+H(parent)+" index="+index+" special_parent="+H(special)+" ownership_proof="+proof);
                }
                bool eligible=(off==0 || off>=0x280 || (currentVersion && proof)) && model!=Direct(G(ram,player+0x268));
                if(off==0x268)text.AppendLine("    MAIN WHEEL: inspection only, excluded from recolouring.");
                else text.AppendLine("    eligible_for_running_version="+eligible+(off>=0x26C && off<=0x27C && !currentVersion?" (v0.3.0 omitted this slot entirely)":""));
                uint start=Direct(G(ram,model+0xF0)),subs=Direct(G(ram,model+0xF4)),count=G(ram,model+0xF8);
                text.AppendLine("    display-list start="+H(start)+" submodels="+H(subs)+" count="+count);
                if (start==0 || count==0 || count>128 || !Range(subs,count*0x28))
                { text.AppendLine("    Model has no traversable own display lists (header guard)."); continue; }
                for (uint n=0;n<count && total<8192;++n)
                {
                    uint pc=Direct(G(ram,subs+n*0x28));
                    if (pc==0 || pc<start || pc-start>0x20000)
                    { text.AppendLine("    submodel "+n+" skipped: start="+H(pc)); continue; }
                    for (int j=0;j<2048 && total<8192;++j,++total,pc+=8)
                    {
                        if (!Range(pc,8)) break;
                        uint w=G(ram,pc),op=w>>24;
                        if (op==0xDF) break;
                        if (op==0xDE)
                        {
                            if (w==0xDE000000) DescribeMaterial(ram,pc,model,start,ref materialRecords);
                            else { text.AppendLine("    traversal stops at non-call DL command "+H(w)+" @"+H(pc));break; }
                        }
                        else if (op!=1 && op!=5 && op!=6 && op!=0xD9 && op!=0)
                        { text.AppendLine("    traversal stops at opcode "+H(w)+" @"+H(pc));break; }
                    }
                }
            }
            if (materialRecords>80) text.AppendLine("Material report capped at 80 calls; observed calls="+materialRecords);
            text.AppendLine("Commands inspected: "+total+" (bounded; includes previously omitted slots for diagnosis).");
        }
        public string Run(byte[] signature, byte[] previousSignature)
        {
            if (signature==null || signature.Length!=11520 || previousSignature==null || previousSignature.Length!=8048)
                throw new InvalidOperationException("Wrong/missing exact-version signature files.");
            var allocations=FindGameAllocations();
            if (allocations.Count==0)
            {
                text.AppendLine("NO VERIFIED GAME RAM: gameplay may not have started, the executable may use another layout, or memory access failed.");
                return text.ToString();
            }
            foreach (ulong baseAddress in allocations)
            {
                text.AppendLine("\nVerified game allocation (host address deliberately omitted).");
                currentVersion=true;
                var mods=FindMod(baseAddress,signature);
                if(mods.Count==0) {currentVersion=false;mods=FindMod(baseAddress,previousSignature);}
                foreach (uint guestMod in mods)
                {
                    text.AppendLine("\nLoaded "+VersionLabel+" guest address: "+H(guestMod));
                    for (int sample=0;sample<5;++sample)
                    {
                        byte[] state=Read(baseAddress+(guestMod-GuestStart)+(uint)DiagnosticOffset,DiagnosticFields*4);
                        if(state==null){text.AppendLine("Could not read runtime diagnostics.");break;}
                        if(W(state,0)!=DiagnosticMagic || W(state,4)!=0x54585452 || W(state,8)!=DiagnosticVersion || W(state,12)!=(uint)DiagnosticFields)
                        {text.AppendLine("Diagnostic header mismatch; no counter interpretation.");break;}
                        text.AppendLine("sample "+sample+": ticks="+W(state,16)+" busy="+W(state,20)+" models="+W(state,24)+
                            " calls="+W(state,28)+" valid_materials="+W(state,32)+" palette_uploads="+W(state,36)+
                            " rgba_uploads="+W(state,40)+" red_targets="+W(state,44)+" active_private_calls="+W(state,48)+
                            " allocated_bytes="+W(state,52)+" allocation_failures="+W(state,56)+" rejected_lists="+W(state,60));
                        text.AppendLine("  Settings RGB="+H(W(state,64))+" original_mode="+W(state,68)+
                            " last_red_source="+H(W(state,80))+" last_private_material="+H(W(state,84))+
                            " index_gray_blocks_untouched="+W(state,88)+" scene_resets="+W(state,92));
                        if(currentVersion) text.AppendLine("  registered_parts="+W(state,96)+" ownership_rejects="+W(state,100)+
                            " solid_lists="+W(state,104)+" red_solid_lists="+W(state,108)+" red_RGB_constants="+W(state,112));
                        if (sample==0 || sample==4)
                        {
                            byte[] ram=Read(baseAddress,GameRamBytes);
                            if (ram==null) text.AppendLine("Game-RAM sample unavailable.");
                            else DescribeModels(ram);
                        }
                        if (sample<4) Thread.Sleep(250);
                    }
                }
                if (mods.Count==0)
                {
                    byte[] ram=Read(baseAddress,GameRamBytes);
                    if (ram!=null) DescribeModels(ram);
                }
            }
            text.AppendLine("\nSamples are read while the game runs: they are NOT atomic. Transient inconsistencies are not a diagnosis.");
            text.AppendLine("No game memory, executable, mod, profile or save was modified. No raw RAM/ROM/texture dump was written.");
            return text.ToString();
        }
        public static string SelfTest(byte[] signature)
        {
            if (!BitConverter.IsLittleEndian || IntPtr.Size!=8 || Marshal.SizeOf(typeof(Region))!=48)
                throw new InvalidOperationException("Unsupported host architecture or memory-info layout.");
            if (signature==null || signature.Length!=11520) throw new InvalidOperationException("Signature length mismatch.");
            byte[] fixture=new byte[12544]; Array.Copy(signature,0,fixture,0x100,signature.Length);
            if (!Matches(fixture,0x100,signature) || Matches(fixture,0x110,signature))
                throw new InvalidOperationException("Signature matcher self-test failed.");
            if (!Red(0xF02020FF) || Red(0xFFFFFFFF) || Red(0x00FF00FF) || Direct(0x01000000)!=0
                || Direct(0xA0012340)!=0x80012340 || !Range(0x807FFFFC,4) || Range(0x807FFFFC,8))
                throw new InvalidOperationException("Guest-reader/red-filter self-test failed.");
            return "Local reader self-test passed (not a game/mod functional test).";
        }
    }
}
