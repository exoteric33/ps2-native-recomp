<#
.SYNOPSIS
  Why does one entry address of a game have hundreds of recorded vector programs? Prints addresses, counts and categories only.

.DESCRIPTION
  The game's vector programs are recorded as games\<name>\vu1\<key>_<entry>.bin (a 16 KiB snapshot of the vector unit's code memory each).
  The key covers everything the program can reach from its entry (runtime/include/ps2/vu_ops.hpp, reachable() and program_key()), so
  several files at one entry mean the reachable code differs between the recordings. For the busiest entries this script walks the code
  of every file exactly like the runner does and reports:
    - how many variants there are, and the reachable instruction pairs (union, and common to all) as address ranges;
    - which pair addresses differ between the variants (ranges), whether the upper word, the lower word or both differ, whether the
      lower word is a constant of an I-bit pair (LOI), whether only the immediate field of an otherwise equal instruction differs
      (IADDIU/ISUBIU, branch offsets, LQ/SQ and ILW/ISW offsets, flag tests such as FCSET), and how many different values a pair takes;
    - how many of the variants would still differ if the LOI constants, then the immediates, then the branch offsets were ignored;
    - whether the walk wraps past 3FF8 to the start of memory or reaches far from the entry.
  It prints no instruction word and no game data: addresses (hex, bytes in the vector unit's code memory), counts and category names.
  It reads the files and writes nothing.

  Run it in a PowerShell window opened in the app's folder (the one with Start.cmd), after saving this text as vu_variants.ps1 there:
      powershell -NoProfile -ExecutionPolicy Bypass -File .\vu_variants.ps1
  Anywhere else: -Root <folder that holds games\> or -Dir <a vu1 folder>.  About 3,500 files (6 entries) take under a minute.

.PARAMETER Top
  How many of the busiest entry addresses to analyse (default 6).
.PARAMETER Entry
  Entry addresses to analyse instead (hex, e.g. -Entry 3418,35C8).
.PARAMETER Game
  Only this game folder under games\ (default: every game that has a vu1 folder).
.PARAMETER MaxRanges
  How many address ranges to list per entry (default 24; the totals are always complete).
.PARAMETER MaxFiles
  Read at most this many files per entry (default 0 = all).
#>
param(
    [string]$Root = '.',
    [string]$Dir = '',
    [string]$Game = '',
    [int]$Top = 6,
    [string[]]$Entry = @(),
    [int]$MaxRanges = 24,
    [int]$MaxFiles = 0
)
$ErrorActionPreference = 'Stop'

$source = @'
using System;
using System.Collections.Generic;
using System.Globalization;
using System.IO;
using System.Linq;
using System.Text;

public class VuVariants
{
    const int Size = 16384;
    const uint Mask = 16383u;
    const int Slots = 2048;   // instruction pairs of 8 bytes in 16 KiB

    class Rec { public string Path; public ulong Key; public long Ticks; }
    class Walk { public uint[] Pcs; public int WrapSeq; public int WrapOther; }
    class Var { public byte[] Code; public Walk W; public bool[] Reach; public ulong Key; public long Ticks; public int Build; }
    class Diff { public uint Addr; public string Label; public int Present, Values; public bool Partial; }
    class Range { public uint From, To; public string Label; public int Pairs, MinPresent, MaxPresent, MinValues, MaxValues; public bool Partial; }

    readonly Dictionary<uint, List<Rec>> byEntry = new Dictionary<uint, List<Rec>>();
    public int Files, Skipped;
    readonly List<uint> analysed = new List<uint>();
    readonly List<long> allTicks = new List<long>();
    readonly HashSet<ulong> rel0 = new HashSet<ulong>(), rel2 = new HashSet<ulong>();
    int relTotal;
    readonly int[] diffEntries = new int[Slots];

    public VuVariants(string vu1Dir)
    {
        foreach (FileInfo fi in new DirectoryInfo(vu1Dir).EnumerateFiles("*.bin"))
        {
            string path = fi.FullName;
            string stem = System.IO.Path.GetFileNameWithoutExtension(path);
            int us = stem.IndexOf('_');
            ulong key; uint entry;
            if (us <= 0 || !ulong.TryParse(stem.Substring(0, us), NumberStyles.HexNumber, CultureInfo.InvariantCulture, out key) ||
                !uint.TryParse(stem.Substring(us + 1), NumberStyles.HexNumber, CultureInfo.InvariantCulture, out entry) || entry >= Size || (entry & 7u) != 0)
            { Skipped++; continue; }
            List<Rec> list;
            if (!byEntry.TryGetValue(entry, out list)) { list = new List<Rec>(); byEntry[entry] = list; }
            Rec r = new Rec(); r.Path = path; r.Key = key; r.Ticks = fi.LastWriteTimeUtc.Ticks;
            allTicks.Add(r.Ticks);
            list.Add(r);
            Files++;
        }
        foreach (List<Rec> list in byEntry.Values) list.Sort((a, b) => string.CompareOrdinal(a.Path, b.Path));
    }

    public int Entries { get { return byEntry.Count; } }

    public uint[] Busiest(int count)
    {
        return byEntry.OrderByDescending(p => p.Value.Count).ThenBy(p => p.Key).Take(count).Select(p => p.Key).ToArray();
    }

    public int Count(uint entry) { List<Rec> l; return byEntry.TryGetValue(entry, out l) ? l.Count : 0; }

    public string[] Header()
    {
        List<string> o = new List<string>();
        int[] edges = new int[] { 1, 2, 6, 21, 101, 301 };
        string[] names = new string[] { "1", "2-5", "6-20", "21-100", "101-300", "301+" };
        int[] hist = new int[edges.Length];
        foreach (List<Rec> l in byEntry.Values) { int b = 0; for (int i = 0; i < edges.Length; i++) if (l.Count >= edges[i]) b = i; hist[b]++; }
        StringBuilder sb = new StringBuilder();
        for (int i = 0; i < edges.Length; i++) { if (i > 0) sb.Append("  "); sb.Append(names[i]).Append("=").Append(hist[i]); }
        o.Add(Files + " files at " + byEntry.Count + " entries" + (Skipped > 0 ? ", " + Skipped + " skipped (name not <key>_<entry>.bin)" : ""));
        o.Add("entries by number of files: " + sb);
        o.Add("recorded: " + Times(allTicks));
        return o.ToArray();
    }

    // the pairs a build reads from one recording: reachable from the entry, and from the jump targets it adds (the return address after
    // a BAL/JALR and an IADDIU with base vi00 read as a code address; recomp/vurecomp_gen.cpp, generate_files)
    static int BuildPairs(byte[] code, uint[] pcs)
    {
        bool[] set = new bool[Slots];
        foreach (uint pc in pcs) set[pc >> 3] = true;
        HashSet<uint> targets = new HashSet<uint>();
        foreach (uint pc in pcs)
        {
            uint lo = W(code, pc), hi = W(code, pc + 4);
            if ((hi & 0x80000000u) != 0) continue;
            uint op = lo >> 25;
            if (op == 0x21 || op == 0x25) targets.Add((pc + 16u) & Mask);
            if (op == 0x08 && ((lo >> 11) & 15u) == 0) targets.Add(((((lo >> 10) & 0x7800u) | (lo & 0x7FFu)) * 8u) & Mask);
        }
        foreach (uint t in targets) foreach (uint pc in Reach(code, t).Pcs) set[pc >> 3] = true;
        int n = 0;
        foreach (bool b in set) if (b) n++;
        return n;
    }

    // when the files were written: the span, and how many fall into each quarter of it (do new ones keep coming?)
    static string Times(List<long> ticks)
    {
        if (ticks.Count == 0) return "no files";
        long first = long.MaxValue, last = long.MinValue;
        foreach (long t in ticks) { if (t < first) first = t; if (t > last) last = t; }
        long span = last - first;
        if (span < 600000000L) return "all files have nearly the same time (a copied folder?), no history to show";
        int[] q = new int[4];
        foreach (long t in ticks) { int i = (int)((t - first) * 4 / span); if (i > 3) i = 3; q[i]++; }
        TimeSpan ts = TimeSpan.FromTicks(span);
        string len = ts.TotalHours >= 48 ? ((int)ts.TotalDays + " days") : ts.TotalMinutes >= 120 ? ((int)ts.TotalHours + " h") : ((int)ts.TotalMinutes + " min");
        return "over " + len + " from the first to the last file; files in each quarter of that time: " + q[0] + " " + q[1] + " " + q[2] + " " + q[3];
    }

    // ---- the walk of reachable() in runtime/include/ps2/vu_ops.hpp, step for step (also the order of the work stack) ----
    static uint W(byte[] c, uint a) { return (uint)c[a] | ((uint)c[a + 1] << 8) | ((uint)c[a + 2] << 16) | ((uint)c[a + 3] << 24); }
    static bool IsBranch(uint lo) { uint op = lo >> 25; return op == 0x20 || op == 0x21 || op == 0x24 || op == 0x25 || (op >= 0x28 && op <= 0x2F && op != 0x2A && op != 0x2B); }
    static bool IsJumpReg(uint lo) { uint op = lo >> 25; return op == 0x24 || op == 0x25; }
    static bool IsUncond(uint lo) { uint op = lo >> 25; return op == 0x20 || op == 0x21; }
    static bool Links(uint lo) { uint op = lo >> 25; return op == 0x21 || op == 0x25; }

    static Walk Reach(byte[] code, uint entry)
    {
        Walk w = new Walk();
        byte[] seen = new byte[Slots];
        List<uint> work = new List<uint>();
        List<uint> outp = new List<uint>();
        work.Add(entry & Mask);
        while (work.Count > 0)
        {
            uint pc = work[work.Count - 1];
            work.RemoveAt(work.Count - 1);
            int stopAfter = -1;
            bool stepped = false;
            for (int guard = 0; guard < 4096; ++guard)
            {
                if (seen[pc >> 3] != 0) break;
                seen[pc >> 3] = 1;
                outp.Add(pc);
                if (stepped && pc == 0) w.WrapSeq++;     // fell from 3FF8 to 0000
                if (stopAfter == 0) break;
                if (stopAfter > 0) --stopAfter;
                uint lo = W(code, pc), hi = W(code, pc + 4);
                if ((hi & 0x40000000u) != 0) stopAfter = 0;
                if ((hi & 0x80000000u) == 0 && IsBranch(lo))
                {
                    if (!IsJumpReg(lo))
                    {
                        int imm11 = (int)(lo << 21) >> 21;
                        long raw = (long)pc + 8 + (long)imm11 * 8;
                        if (raw < 0 || raw > Mask) w.WrapOther++;
                        work.Add((uint)(raw & Mask));
                    }
                    if (Links(lo)) { if (pc + 16u > Mask) w.WrapOther++; work.Add((pc + 16u) & Mask); }
                    if (IsUncond(lo) || IsJumpReg(lo)) stopAfter = 0;
                }
                pc = (pc + 8u) & Mask;
                stepped = true;
            }
        }
        outp.Sort();
        w.Pcs = outp.ToArray();
        return w;
    }

    static ulong Fnv(ulong h, uint v)
    {
        unchecked
        {
            for (int i = 0; i < 4; i++) { h ^= (v >> (i * 8)) & 0xFFu; h *= 0x100000001B3UL; }
        }
        return h;
    }

    // The immediate field of a lower instruction (0 = none) and which kind it is.
    // classes: 1 LQ/SQ offset, 2 ILW/ISW offset, 3 IADDIU/ISUBIU, 4 IADDI (5 bits), 5 flag test, 6 branch offset
    static uint ImmMask(uint lo, out int cls)
    {
        cls = 0;
        switch (lo >> 25)
        {
            case 0x00: case 0x01: cls = 1; return 0x7FFu;
            case 0x04: case 0x05: cls = 2; return 0x7FFu;
            case 0x08: case 0x09: cls = 3; return 0x01E007FFu;
            case 0x10: case 0x11: case 0x12: case 0x13: cls = 5; return 0x00FFFFFFu;
            case 0x14: case 0x15: case 0x16: case 0x17: cls = 5; return 0x002007FFu;
            case 0x20: case 0x21: case 0x28: case 0x29: case 0x2C: case 0x2D: case 0x2E: case 0x2F: cls = 6; return 0x7FFu;
            case 0x40: if ((lo & 0x3Fu) == 0x32u) { cls = 4; return 0x7C0u; } return 0;
        }
        return 0;
    }

    static string ClassName(uint lo, int cls)
    {
        switch (cls)
        {
            case 1: return "LQ/SQ offset";
            case 2: return "ILW/ISW offset";
            case 3: return ((lo >> 11) & 15u) == 0 ? "IADDIU/ISUBIU, base vi00 (a constant or an address)" : "IADDIU/ISUBIU";
            case 4: return "IADDI";
            case 5: return "flag test (FCEQ/FCSET/FCAND/FCOR/FSEQ/FSSET/FSAND/FSOR)";
            case 6: return "branch offset";
        }
        return "?";
    }

    // The key as the runner makes it (level 0), or with parts ignored to see how many variants would be left:
    // 1 = constants of I-bit pairs ignored, 2 = also the immediates of non-branch instructions, 3 = also branch offsets,
    // 4 = no instruction word at all (only which pairs are reachable)
    static ulong Hash(byte[] code, uint[] pcs, uint entry, int level, bool relative)
    {
        // relative: the entry address is left out and every pair is placed by its distance from the entry
        ulong h = relative ? 0xCBF29CE484222325UL : 0xCBF29CE484222325UL ^ entry;
        foreach (uint pc in pcs)
        {
            uint lo = W(code, pc), hi = W(code, pc + 4);
            h = Fnv(h, relative ? (unchecked(pc - entry) & Mask) : pc);
            if (level >= 4) continue;
            if (level >= 1)
            {
                if ((hi & 0x80000000u) != 0) lo = 0;
                else if (level >= 2)
                {
                    int cls;
                    uint m = ImmMask(lo, out cls);
                    if (m != 0 && (cls != 6 || level >= 3)) lo &= ~m;
                }
            }
            h = Fnv(h, lo);
            h = Fnv(h, hi);
        }
        return h;
    }

    static string A(uint a) { return a.ToString("X4"); }
    static string Rng(uint a, uint b) { return a == b ? A(a) : A(a) + "-" + A(b); }

    static List<string> Ranges(Func<int, bool> member)
    {
        List<string> r = new List<string>();
        int i = 0;
        while (i < Slots)
        {
            if (!member(i)) { i++; continue; }
            int j = i;
            while (j + 1 < Slots && member(j + 1)) j++;
            r.Add(Rng((uint)i * 8, (uint)j * 8));
            i = j + 1;
        }
        return r;
    }

    static string Join(List<string> parts, int max)
    {
        StringBuilder sb = new StringBuilder();
        for (int i = 0; i < parts.Count && i < max; i++) { if (i > 0) sb.Append(' '); sb.Append(parts[i]); }
        if (parts.Count > max) sb.Append(" ... (" + (parts.Count - max) + " more)");
        return sb.Length == 0 ? "none" : sb.ToString();
    }

    static string LowerKind(uint lo0, uint dLo, bool sameOp)
    {
        if (!sameOp) return "instruction (opcode)";
        int cls;
        uint m = ImmMask(lo0, out cls);
        if (m != 0 && (dLo & ~m) == 0) return "immediate only: " + ClassName(lo0, cls);
        return "instruction (same opcode, other fields)";
    }

    public string[] Analyse(uint entry, int maxRanges, int maxFiles)
    {
        List<string> o = new List<string>();
        List<Rec> recs = byEntry[entry];
        int take = maxFiles > 0 && maxFiles < recs.Count ? maxFiles : recs.Count;
        List<Var> vs = new List<Var>();
        int keyOk = 0, keyBad = 0, badSize = 0;
        for (int i = 0; i < take; i++)
        {
            byte[] code = File.ReadAllBytes(recs[i].Path);
            if (code.Length != Size) { badSize++; continue; }
            Var v = new Var();
            v.Code = code;
            v.W = Reach(code, entry);
            v.Key = Hash(code, v.W.Pcs, entry, 0, false);
            v.Ticks = recs[i].Ticks;
            v.Build = BuildPairs(code, v.W.Pcs);
            if (v.Key == recs[i].Key) keyOk++; else keyBad++;
            v.Reach = new bool[Slots];
            foreach (uint pc in v.W.Pcs) v.Reach[pc >> 3] = true;
            vs.Add(v);
        }
        int n = vs.Count;
        o.Add("== entry " + A(entry) + ": " + recs.Count + " files" + (take < recs.Count ? " (reading " + take + ")" : "") + " ==");
        if (badSize > 0) o.Add("  " + badSize + " files are not 16384 bytes and were skipped");
        if (n == 0) return o.ToArray();
        o.Add("  key check: " + keyOk + " of " + n + " keys recomputed from the code equal the file names" + (keyBad > 0 ? "  <-- " + keyBad + " DIFFER: this walk is not the runner's, do not trust the rest" : ""));

        int[] presence = new int[Slots];
        List<int> sizes = new List<int>();
        foreach (Var v in vs) { foreach (uint pc in v.W.Pcs) presence[pc >> 3]++; sizes.Add(v.W.Pcs.Length); }
        sizes.Sort();
        int unionN = 0, interN = 0;
        for (int s = 0; s < Slots; s++) { if (presence[s] > 0) unionN++; if (presence[s] == n) interN++; }
        long sumPairs = 0, sumBuild = 0;
        foreach (Var v in vs) { sumPairs += v.W.Pcs.Length; sumBuild += v.Build; }
        o.Add("  reachable pairs per variant: " + sizes[0] + " to " + sizes[n - 1] + " (middle " + sizes[n / 2] + "); in the union " + unionN + ", in every variant " + interN);
        o.Add("  a file holding only the pairs a build reads (reachable, plus its jump targets) would average " + (sumBuild * 8 / n) + " bytes, " + (sumBuild * 800 / ((long)n * Size)).ToString() + " % of the 16384 a recording takes; reachable alone: " + (sumPairs * 8 / n) + " bytes");
        List<long> ticks = new List<long>();
        foreach (Var v in vs) ticks.Add(v.Ticks);
        o.Add("  recorded: " + Times(ticks));
        o.Add("  union:  " + Join(Ranges(s => presence[s] > 0), 12));
        o.Add("  common: " + Join(Ranges(s => presence[s] == n), 12));

        // the pairs that differ
        List<Diff> diffs = new List<Diff>();
        for (int s = 0; s < Slots; s++)
        {
            if (presence[s] == 0) continue;
            uint a = (uint)s * 8;
            HashSet<ulong> values = new HashSet<ulong>();
            uint lo0 = 0, hi0 = 0, dLo = 0, dHi = 0;
            int iSet = 0, cnt = 0;
            bool sameOp = true;
            foreach (Var v in vs)
            {
                if (!v.Reach[s]) continue;
                uint lo = W(v.Code, a), hi = W(v.Code, a + 4);
                if (cnt == 0) { lo0 = lo; hi0 = hi; }
                dLo |= lo ^ lo0; dHi |= hi ^ hi0;
                if ((hi & 0x80000000u) != 0) iSet++;
                if ((lo >> 25) != (lo0 >> 25)) sameOp = false;
                values.Add(((ulong)hi << 32) | lo);
                cnt++;
            }
            bool partial = presence[s] < n;
            if (values.Count == 1 && !partial) continue;
            string label;
            if (values.Count == 1) label = "same words, reachable in some variants only";
            else if (iSet > 0 && iSet < cnt) label = "I bit differs (lower word: constant in some variants, instruction in others)";
            else
            {
                string lower = null, upper = null;
                if (dLo != 0) lower = iSet == cnt ? "LOI constant" : LowerKind(lo0, dLo, sameOp);
                if (dHi != 0) upper = (dHi & ~0xF8000000u) == 0 ? "flag bits E/M/D/T only" : "instruction";
                if (upper != null && lower != null) label = "upper " + upper + " + lower " + lower;
                else if (upper != null) label = "upper " + upper;
                else label = "lower " + lower;
                if (partial) label += " + reach differs";
            }
            Diff d = new Diff();
            d.Addr = a; d.Label = label; d.Present = presence[s]; d.Values = values.Count; d.Partial = partial;
            diffs.Add(d);
        }
        bool[] differs = new bool[Slots];
        foreach (Diff d in diffs) differs[d.Addr >> 3] = true;
        analysed.Add(entry);
        for (int s = 0; s < Slots; s++) if (differs[s]) diffEntries[s]++;

        o.Add("  pairs that differ between variants: " + diffs.Count + " of " + unionN + ", in " + Ranges(s => differs[s]).Count + " ranges");
        if (diffs.Count > 0)
        {
            Dictionary<string, int> byLabel = new Dictionary<string, int>();
            foreach (Diff d in diffs) { int c; byLabel.TryGetValue(d.Label, out c); byLabel[d.Label] = c + 1; }
            foreach (KeyValuePair<string, int> p in byLabel.OrderByDescending(p => p.Value))
                o.Add("    " + p.Value.ToString().PadLeft(5) + " pairs: " + p.Key);
            // ranges: neighbouring pairs with the same kind of difference
            List<Range> rs = new List<Range>();
            foreach (Diff d in diffs)
            {
                Range last = rs.Count > 0 ? rs[rs.Count - 1] : null;
                if (last != null && last.To + 8 == d.Addr && last.Label == d.Label)
                {
                    last.To = d.Addr; last.Pairs++;
                    last.MinPresent = Math.Min(last.MinPresent, d.Present); last.MaxPresent = Math.Max(last.MaxPresent, d.Present);
                    last.MinValues = Math.Min(last.MinValues, d.Values); last.MaxValues = Math.Max(last.MaxValues, d.Values);
                }
                else
                {
                    Range r = new Range();
                    r.From = r.To = d.Addr; r.Label = d.Label; r.Pairs = 1; r.Partial = d.Partial;
                    r.MinPresent = r.MaxPresent = d.Present; r.MinValues = r.MaxValues = d.Values;
                    rs.Add(r);
                }
            }
            o.Add("  ranges (address, pairs, kind, values per pair, in how many of the " + n + " variants):");
            for (int i = 0; i < rs.Count && i < maxRanges; i++)
            {
                Range r = rs[i];
                string vals = r.MinValues == r.MaxValues ? r.MinValues.ToString() : r.MinValues + ".." + r.MaxValues;
                string pres = r.MinPresent == n ? "all" : (r.MinPresent == r.MaxPresent ? r.MinPresent.ToString() : r.MinPresent + ".." + r.MaxPresent);
                o.Add("    " + Rng(r.From, r.To).PadRight(9) + " " + r.Pairs.ToString().PadLeft(4) + " pairs  " + r.Label + "  | values " + vals + " | in " + pres);
            }
            if (rs.Count > maxRanges) o.Add("    ... " + (rs.Count - maxRanges) + " more ranges");
        }

        // how many variants are left when parts of the code are ignored
        int[] left = new int[5];
        for (int level = 0; level < 5; level++)
        {
            HashSet<ulong> hs = new HashSet<ulong>();
            foreach (Var v in vs) hs.Add(Hash(v.Code, v.W.Pcs, entry, level, false));
            left[level] = hs.Count;
        }
        o.Add("  different programs when ignoring: nothing " + left[0] + " | LOI constants " + left[1] + " | + immediates (not branch offsets) " + left[2] + " | + branch offsets " + left[3] + " | all words (only the reachable addresses) " + left[4]);

        foreach (Var v in vs)
        {
            rel0.Add(Hash(v.Code, v.W.Pcs, entry, 0, true));
            rel2.Add(Hash(v.Code, v.W.Pcs, entry, 2, true));
            relTotal++;
        }

        // runs of neighbouring reachable pairs, and how many of them are the same in several variants
        HashSet<ulong> runSet = new HashSet<ulong>();
        int runTotal = 0;
        foreach (Var v in vs)
        {
            uint[] p = v.W.Pcs;
            int i = 0;
            while (i < p.Length)
            {
                int j = i;
                while (j + 1 < p.Length && p[j + 1] == p[j] + 8) j++;
                ulong h = Hash(v.Code, SubArray(p, i, j), p[i], 0, false);
                runSet.Add(h);
                runTotal++;
                i = j + 1;
            }
        }
        o.Add("  runs of neighbouring reachable pairs: " + runTotal + " in all variants, " + runSet.Count + " different (address and words)");

        // does the walk leave the neighbourhood of the entry?
        int wrapSeq = 0, wrapOther = 0, far = 0, zeroV = 0, zeroMax = 0;
        uint farMax = 0;
        foreach (Var v in vs)
        {
            if (v.W.WrapSeq > 0) wrapSeq++;
            if (v.W.WrapOther > 0) wrapOther++;
            uint mx = 0;
            int zeros = 0;
            foreach (uint pc in v.W.Pcs)
            {
                uint dist = pc > entry ? pc - entry : entry - pc;
                if (dist > mx) mx = dist;
                if (W(v.Code, pc) == 0 && W(v.Code, pc + 4) == 0) zeros++;
            }
            if (zeros > 0) zeroV++;
            if (zeros > zeroMax) zeroMax = zeros;
            if (mx > 0x1000) far++;
            if (mx > farMax) farMax = mx;
        }
        o.Add("  walk: runs past 3FF8 into 0000 in " + wrapSeq + " variants; a branch or call target wraps in " + wrapOther + "; a pair more than 1000 bytes from the entry in " + far + " (farthest: " + farMax.ToString("X") + " bytes)");
        o.Add("  walk: reachable pairs that are empty memory (all zero): in " + zeroV + " variants, up to " + zeroMax + " pairs");
        return o.ToArray();
    }

    static uint[] SubArray(uint[] p, int from, int to)
    {
        uint[] r = new uint[to - from + 1];
        Array.Copy(p, from, r, 0, r.Length);
        return r;
    }

    public string[] Summary(int maxRanges)
    {
        List<string> o = new List<string>();
        if (analysed.Count < 2) return o.ToArray();
        int total = analysed.Count;
        int any = 0, all = 0;
        for (int s = 0; s < Slots; s++) { if (diffEntries[s] > 0) any++; if (diffEntries[s] == total) all++; }
        o.Add("== the " + total + " entries together ==");
        o.Add("  pairs that differ in at least one entry: " + any + "; in every entry: " + all);
        o.Add("  differ in every entry: " + Join(Ranges(s => diffEntries[s] == total), maxRanges));
        o.Add("  differ in at least one: " + Join(Ranges(s => diffEntries[s] > 0), maxRanges));
        o.Add("  variants of these entries in all: " + relTotal + "; different when the entry address is ignored (code compared by distance from its entry): " + rel0.Count + "; also ignoring LOI constants and immediates: " + rel2.Count);
        return o.ToArray();
    }
}
'@

if (-not ('VuVariants' -as [type])) {
    Add-Type -TypeDefinition $source -Language CSharp
}

# which vu1 folders
$games = @()
if ($Dir) {
    $full = (Resolve-Path -LiteralPath $Dir).Path.TrimEnd([char[]]@(92, 47))
    $label = Split-Path $full -Leaf
    if ($label -eq 'vu1') { $label = Split-Path (Split-Path $full -Parent) -Leaf }
    $games += [pscustomobject]@{ Name = $label; Path = $full }
} else {
    $gamesRoot = Join-Path $Root 'games'
    if (-not (Test-Path -LiteralPath $gamesRoot) -and $PSScriptRoot) { $gamesRoot = Join-Path $PSScriptRoot '..\games' }
    if (-not (Test-Path -LiteralPath $gamesRoot)) { Write-Output "No games folder found (looked in $Root). Run this in the app's folder, or pass -Root <folder that holds games> or -Dir <a vu1 folder>."; exit 1 }
    foreach ($g in Get-ChildItem -LiteralPath $gamesRoot -Directory) {
        if ($Game -and $g.Name -ne $Game) { continue }
        $v = Join-Path $g.FullName 'vu1'
        if (Test-Path -LiteralPath $v) { $games += [pscustomobject]@{ Name = $g.Name; Path = $v } }
    }
}
if ($games.Count -eq 0) { Write-Output 'No vu1 folder with recorded programs found.'; exit 1 }

Write-Output 'vu_variants: prints addresses, counts and category names only (no instruction words, no paths).'
$clock = [System.Diagnostics.Stopwatch]::StartNew()
foreach ($g in $games) {
    $vv = New-Object VuVariants($g.Path)
    if ($vv.Files -eq 0) { continue }
    Write-Output ''
    Write-Output "##### game $($g.Name)"
    $vv.Header()
    if ($Entry.Count -gt 0) {
        $list = @($Entry | ForEach-Object { $_ -split '[,\s]+' } | Where-Object { $_ } | ForEach-Object { [Convert]::ToUInt32($_, 16) } | Where-Object { $vv.Count($_) -gt 0 })
    } else {
        $list = @($vv.Busiest($Top))
    }
    foreach ($e in $list) {
        Write-Host ('reading entry {0:X4}: {1} files ...' -f $e, $vv.Count($e))   # progress (a screen line; it is part of the text when the output is piped)
        $vv.Analyse([uint32]$e, $MaxRanges, $MaxFiles)
    }
    $vv.Summary($MaxRanges)
}
Write-Output ''
Write-Output ('done in {0:N1} s' -f $clock.Elapsed.TotalSeconds)
