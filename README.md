# Virtual Memory Simulator & Page Replacement Analysis

Virtual memory simulator implementing OPT, CLOCK, Rand, and NRU page replacement algorithms using Valgrind memory traces to analyze page fault behavior, dirty page writebacks, and workload locality characteristics.

---

## Overview

This project compares multiple virtual memory page replacement strategies under varying physical memory configurations using real memory access traces generated with Valgrind Lackey.

The simulator evaluates algorithmic tradeoffs through quantitative analysis of:
- page faults
- dirty page writebacks
- memory locality behavior
- replacement efficiency
- frame allocation scaling

The project explores operating systems concepts including demand paging, virtual memory management, replacement heuristics, and systems performance benchmarking.

---

## Features

- OPT page replacement implementation
- CLOCK page replacement implementation
- Random replacement policy
- NRU (Not Recently Used) replacement policy
- Configurable frame allocation
- Single-level page table simulation
- Dirty page tracking and disk-write accounting
- Performance benchmarking with Valgrind traces
- Quantitative workload analysis

---

## Technologies Used

- C
- Linux
- Valgrind Lackey
- GNU Make

---

## Algorithms Implemented

| Algorithm | Description |
|---|---|
| **OPT** | Evicts the page whose next use is farthest in the future |
| **CLOCK** | Second-chance approximation of LRU using reference bits |
| **Rand** | Random frame eviction policy |
| **NRU** | Classifies pages using reference and dirty bits |

---

## Experimental Configuration

- Page size: 16 KiB
- Address mapping via bit-shift page translation
- Tested with multiple physical frame counts
- Evaluated using `gcc.trace` and `ls.trace`
- NRU refresh interval: 1000 references

---

## Results Summary

- Analyzed over **2.4 million** memory accesses across multiple workloads
- Compared page fault rates and disk-write behavior across replacement policies
- Demonstrated CLOCK consistently outperforming Random replacement on realistic workloads
- Evaluated how locality patterns influence replacement efficiency and memory performance

---

## Example Usage

```bash
./vmsim -q -n 8 -a opt gcc.trace

./vmsim -q -n 16 -a clock gcc.trace

./vmsim -q -n 8 -a nru -r 1000 gcc.trace
