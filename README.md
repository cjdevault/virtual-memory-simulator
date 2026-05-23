# Virtual Memory Simulator (vmsim)

A demand-paging simulator that replays memory traces and compares four page-replacement policies under a fixed number of physical frames.

---

## Overview

`vmsim` reads a **lackey-format** trace (Valgrind), maps each access to a **16 KiB** page, and simulates a single-level page table with a configurable frame count. On each page fault it applies one of **OPT**, **CLOCK**, **Rand**, or **NRU** to choose a victim, tracks **dirty** pages for write-back I/O, and prints per-access events plus a summary.

OPT uses future knowledge of the trace (theoretical lower bound). CLOCK, Rand, and NRU are practical policies suitable for comparison in the accompanying report.

---

## Features

| Component | Description |
|-----------|-------------|
| **Trace parser** | Handles `I` / `L` / `S` / `M` lines; skips invalid lines |
| **Page mapping** | 32-bit addresses, **16 KiB** pages (`page = address >> 14`) |
| **OPT** | Evicts the resident page whose **next use** is farthest in the future (binary search on pre-sorted page indices) |
| **CLOCK** | Second-chance algorithm with reference bit |
| **Rand** | Random victim (deterministic xorshift seed for reproducibility) |
| **NRU** | Four classes from **(R, D)**; periodic **R**-bit refresh via `-r` |
| **Dirty tracking** | `S` and `M` set dirty; dirty evictions count as disk writes |
| **Quiet mode** | `-q` prints summary only (for large traces like `gcc.trace`) |

---

## Requirements

- **GCC** with C11 support
- **GNU Make**
- For full benchmarks: decompress bundled traces with `gunzip -k gcc.trace.gz ls.trace.gz` (creates `gcc.trace` and `ls.trace`)

---

## Build

```bash
make
```

Produces `./vmsim`.

```bash
make clean   # remove vmsim binary
```

Compiler flags: `-std=c11 -O2 -Wall -Wextra -pedantic`

---

## Usage

```text
vmsim [-q] -n <numframes> -a <opt|clock|rand|nru> [-r <refresh>] <tracefile>
```

| Option | Description |
|--------|-------------|
| `-n <numframes>` | Number of physical frames (required, must be > 0) |
| `-a <algorithm>` | `opt`, `clock`, `rand`, or `nru` (required) |
| `-r <refresh>` | **NRU only:** clear all **R** bits every `<refresh>` memory references (required and > 0 when using `nru`) |
| `-q` | Quiet: suppress per-access lines; print summary only |
| `<tracefile>` | Path to lackey trace (required) |

Options and the trace path may appear in any order.

### Trace format (lackey)

Each valid line:

```text
<op> <hex-address>,<size>
```

| Op | Meaning | Access type |
|----|---------|-------------|
| `I` | Instruction fetch | Read |
| `L` | Load | Read |
| `S` | Store | Write (sets dirty) |
| `M` | Modify | Read + write, **one** access (sets dirty) |

The **size** field is parsed but ignored; only the page number matters.

### Example commands

```bash
# Small bundled test traces
./vmsim -n 2 -a opt test.trace
./vmsim -n 2 -a clock test_dirty.trace

# Report-style runs (decompress gcc.trace.gz / ls.trace.gz first)
gunzip -k gcc.trace.gz ls.trace.gz
./vmsim -q -n 8  -a opt   gcc.trace
./vmsim -q -n 16 -a clock gcc.trace
./vmsim -q -n 32 -a rand  gcc.trace
./vmsim -q -n 8  -a nru -r 1000 gcc.trace
```

### Output

**Per access** (unless `-q`):

- `hit`
- `page fault – no eviction`
- `page fault – evict clean`
- `page fault – evict dirty`

**Summary** (always):

```text
Algorithm: <opt|clock|rand|nru>
Number of frames: <n>
Total memory accesses: <N>
Total page faults: <faults>
Total writes to disk: <dirty evictions>
```

---

## Algorithms (brief)

### OPT (optimal)

Precomputes, for each page, the sorted list of trace indices where it appears. On eviction, chooses the resident page whose **next** use after the current index is latest (or never used again). Tie-break: lower frame index.

### CLOCK

Circular scan with a reference bit. On eviction, advance the hand until a frame with `ref == 0`; give frames with `ref == 1` a second chance by clearing `ref` and moving on.

### Rand

Picks a uniform random frame index on each eviction (seed `0xC001D00D` for repeatable runs).

### NRU

Classifies each frame by **(R, D)** into four NRU classes; evicts from the **lowest** class (tie: lowest frame index). Every `-r` references, all **R** bits are cleared so aging continues.

---

## Project structure

```text
project5/
├── README.md                  # This file
├── Makefile                   # Build vmsim
├── vmsim.c                    # Simulator source
├── vmsim                      # Compiled binary (after make)
│
├── test.trace                 # Minimal sample trace
├── test_dirty.trace           # Sample trace with stores (dirty pages)
├── gcc.trace.gz               # Valgrind lackey trace — gcc (~1.7M accesses)
├── ls.trace.gz                # Valgrind lackey trace — ls (~766K accesses)
│
├── report.tex                 # LaTeX report source (Overleaf-ready)
├── REPORT_OUTLINE.md          # Report draft, tables, methodology notes
├── REPORT_OUTLINE.pdf         # Exported outline / draft PDF
├── CSc452Project5Report.pdf   # Final submitted report
├── gcc_chart.pdf              # Page faults vs frames — gcc.trace
├── ls_chart.pdf               # Page faults vs frames — ls.trace
```

**Traces:** `vmsim` reads plain `.trace` files. Decompress before running full experiments:

```bash
gunzip -k gcc.trace.gz ls.trace.gz
```

---

## Report & experiments

The written report compares all four algorithms on **`gcc.trace`** (~1.71M accesses) and **`ls.trace`** (~766K accesses) at frame counts **8, 16, 32, 64**. NRU experiments use **`-r 1000`**.

See **`REPORT_OUTLINE.md`** for measured fault counts, fault ratios vs OPT, disk-write tables, and analysis notes. **`report.tex`** is the LaTeX source; **`CSc452Project5Report.pdf`** is the final report. Charts **`gcc_chart.pdf`** and **`ls_chart.pdf`** are included for the results section.

### Sample results (`gcc.trace`, page faults)

| Frames | OPT   | CLOCK | Rand  | NRU (−r 1000) |
|--------|-------|-------|-------|---------------|
| 8      | 11357 | 20455 | 27469 | 44116         |
| 16     | 2581  | 6068  | 7643  | 7544          |
| 32     | 428   | 844   | 1265  | 1254          |
| 64     | 113   | 144   | 237   | 508           |

Full tables for `ls.trace`, fault ratios, and write-back counts are in `REPORT_OUTLINE.md` and `report.tex`.

---

## Design notes

- **Single-pass simulation:** The trace is loaded once; OPT uses an auxiliary sorted `(page, index)` array for O(log n) next-use queries.
- **Frame table:** Linear scan for resident lookup is sufficient for small frame counts used in grading.
- **Modify (`M`):** Treated as one memory access that sets the dirty bit (read + write semantics).
- **Disk writes:** Incremented only when evicting a **dirty** page (write-back model).

---

## License

Coursework submission. All rights reserved by the author unless your course specifies otherwise.
