/*
 * CSc 452 Project 5 — Virtual Memory Simulator (vmsim)
 *
 * Simulates demand paging with a fixed number of physical frames. Each trace
 * line (lackey format: I/L/S/M, hex address, size) is one access; size is
 * ignored — only the page number matters. 16 KiB pages, 32-bit addresses.
 *
 * Usage:
 *   vmsim [-q] -n <numframes> -a <opt|clock|rand|nru> [-r <refresh>] <tracefile>
 *   -q   optional: summary only (not required by the handout; long traces)
 *   -r   only meaningful for NRU: clear R bits every <refresh> references
 *        (required and >0 when -a nru)
 *
 * Algorithms: OPT (future knowledge), CLOCK (second chance), Rand, NRU (R/D).
 */

#define _POSIX_C_SOURCE 200809L

#include <ctype.h>
#include <inttypes.h>
#include <limits.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define PAGE_SHIFT 14
#define PAGE_SIZE  (1u << PAGE_SHIFT)

/* Next reference “never” for OPT: larger than any trace index */
#define INF_NEXT UINT64_MAX

/* Per-access lines: use en dash (U+2013) after “fault” as in the assignment PDF */
#define MSG_PF_NONE    "page fault \xe2\x80\x93 no eviction"
#define MSG_PF_CLEAN   "page fault \xe2\x80\x93 evict clean"
#define MSG_PF_DIRTY   "page fault \xe2\x80\x93 evict dirty"

typedef enum {
	ACC_I = 0, /* instruction fetch — read */
	ACC_L,     /* load — read */
	ACC_S,     /* store — write */
	ACC_M      /* modify — read + write (counts as one access) */
} AccKind;

typedef struct {
	uint32_t page;
	AccKind  kind;
} TraceEnt;

typedef struct {
	uint32_t page;
	uint32_t idx;
} PageIdxPair;

typedef enum { ALG_OPT, ALG_CLOCK, ALG_RAND, ALG_NRU } Algo;

/* Resident slot: page == UINT32_MAX means frame is free */
typedef struct {
	uint32_t page;
	uint8_t  ref; /* reference bit (CLOCK, NRU) */
	uint8_t  dirty;
} Frame;

/* --- page number --- */

static uint32_t page_of(uint32_t addr)
{
	return addr >> PAGE_SHIFT;
}

/* Sort by page, then by trace index (for equal pages) */
static int cmp_pair(const void *a, const void *b)
{
	const PageIdxPair *pa = a;
	const PageIdxPair *pb = b;
	if (pa->page != pb->page)
		return pa->page < pb->page ? -1 : 1;
	if (pa->idx != pb->idx)
		return pa->idx < pb->idx ? -1 : 1;
	return 0;
}

/* --- OPT: sorted (page, trace_index) pairs; binary search next use of a page --- */

/* First k with pairs[k].page >= p */
static size_t lower_bound_page(PageIdxPair *pairs, size_t n, uint32_t p)
{
	size_t lo = 0, hi = n;
	while (lo < hi) {
		size_t mid = lo + (hi - lo) / 2;
		if (pairs[mid].page < p)
			lo = mid + 1;
		else
			hi = mid;
	}
	return lo;
}

/* First k with pairs[k].page > p */
static size_t upper_bound_page(PageIdxPair *pairs, size_t n, uint32_t p)
{
	size_t lo = 0, hi = n;
	while (lo < hi) {
		size_t mid = lo + (hi - lo) / 2;
		if (pairs[mid].page <= p)
			lo = mid + 1;
		else
			hi = mid;
	}
	return lo;
}

/* Smallest idx in pairs[lo..hi) with idx > pos, or INF_NEXT if none */
static uint64_t next_use_after(PageIdxPair *pairs, size_t lo, size_t hi,
				uint32_t pos)
{
	if (lo >= hi)
		return INF_NEXT;
	size_t a = lo, b = hi;
	while (a < b) {
		size_t mid = a + (b - a) / 2;
		if (pairs[mid].idx <= pos)
			a = mid + 1;
		else
			b = mid;
	}
	if (a >= hi)
		return INF_NEXT;
	return pairs[a].idx;
}

/* I/L: read only. S/M: mark dirty (M is one access but read+write semantically). */
static void apply_access_bits(Frame *f, AccKind k)
{
	f->ref = 1;
	switch (k) {
	case ACC_I:
	case ACC_L:
		break;
	case ACC_S:
	case ACC_M:
		f->dirty = 1;
		break;
	}
}

/* --- resident set lookup (small frame count: linear scan is fine) --- */

static int find_frame(Frame *frames, int nf, uint32_t page)
{
	for (int i = 0; i < nf; i++)
		if (frames[i].page == page)
			return i;
	return -1;
}

static int find_free_frame(Frame *frames, int nf)
{
	for (int i = 0; i < nf; i++)
		if (frames[i].page == UINT32_MAX)
			return i;
	return -1;
}

static void print_action(const char *s)
{
	puts(s);
}

static void nru_clear_refs(Frame *frames, int nf)
{
	for (int i = 0; i < nf; i++) {
		if (frames[i].page != UINT32_MAX)
			frames[i].ref = 0;
	}
}

/* --- NRU: 4 classes from (R,D); evict lowest class, tie → lower index --- */

static int nru_class(const Frame *f)
{
	return (f->ref ? 2 : 0) + (f->dirty ? 1 : 0);
}

static int pick_nru_victim(Frame *frames, int nf)
{
	int best = -1;
	int bestc = INT_MAX;
	for (int i = 0; i < nf; i++) {
		if (frames[i].page == UINT32_MAX)
			continue;
		int c = nru_class(&frames[i]);
		if (best < 0 || c < bestc || (c == bestc && i < best)) {
			bestc = c;
			best = i;
		}
	}
	return best;
}

/* Deterministic “random” for repeatability; not crypto-grade */
static uint32_t xorshift32(uint32_t *state)
{
	uint32_t x = *state;
	x ^= x << 13;
	x ^= x >> 17;
	x ^= x << 5;
	*state = x;
	return x;
}

/* Read lackey lines; skip non-matching garbage. One struct per valid access. */
static int parse_trace(FILE *fp, TraceEnt **out_trace, size_t *out_n)
{
	size_t cap = 4096;
	size_t n = 0;
	TraceEnt *tr = malloc(cap * sizeof *tr);
	if (!tr)
		return -1;

	char line[512];
	while (fgets(line, sizeof line, fp)) {
		char *s = line;
		while (*s && isspace((unsigned char)*s))
			s++;
		if (*s == '\0' || *s == '#')
			continue;

		char op = *s++;
		if (op != 'I' && op != 'L' && op != 'S' && op != 'M')
			continue;
		if (*s == '\0' || !isspace((unsigned char)*s))
			continue;

		uint32_t addr;
		unsigned sz;
		if (sscanf(s, "%" SCNx32 ",%u", &addr, &sz) != 2)
			continue;

		if (n >= cap) {
			cap *= 2;
			TraceEnt *nr = realloc(tr, cap * sizeof *nr);
			if (!nr) {
				free(tr);
				return -1;
			}
			tr = nr;
		}
		tr[n].page = page_of(addr);
		switch (op) {
		case 'I':
			tr[n].kind = ACC_I;
			break;
		case 'L':
			tr[n].kind = ACC_L;
			break;
		case 'S':
			tr[n].kind = ACC_S;
			break;
		default:
			tr[n].kind = ACC_M;
			break;
		}
		n++;
	}

	*out_trace = tr;
	*out_n = n;
	return 0;
}

/* --- main --- */

static void usage(FILE *fp, const char *argv0)
{
	fprintf(fp,
		"usage: %s [-q] -n <numframes> -a <opt|clock|rand|nru> "
		"[-r <refresh>] <tracefile>\n"
		"  -q  quiet: summary only (no per-access lines)\n",
		argv0);
}

int main(int argc, char **argv)
{
	int num_frames = -1;
	Algo algo = ALG_OPT;
	int refresh = -1;
	const char *path = NULL;
	int quiet = 0;

	/* Options and trace path can appear in any order (no getopt) */
	for (int i = 1; i < argc; i++) {
		if (strcmp(argv[i], "-q") == 0) {
			quiet = 1;
		} else if (strcmp(argv[i], "-n") == 0 && i + 1 < argc) {
			num_frames = atoi(argv[++i]);
		} else if (strcmp(argv[i], "-a") == 0 && i + 1 < argc) {
			const char *a = argv[++i];
			if (strcmp(a, "opt") == 0)
				algo = ALG_OPT;
			else if (strcmp(a, "clock") == 0)
				algo = ALG_CLOCK;
			else if (strcmp(a, "rand") == 0)
				algo = ALG_RAND;
			else if (strcmp(a, "nru") == 0)
				algo = ALG_NRU;
			else {
				fprintf(stderr, "unknown algorithm: %s\n", a);
				usage(stderr, argv[0]);
				return 1;
			}
		} else if (strcmp(argv[i], "-r") == 0 && i + 1 < argc) {
			refresh = atoi(argv[++i]);
		} else if (argv[i][0] == '-') {
			fprintf(stderr, "unknown option: %s\n", argv[i]);
			usage(stderr, argv[0]);
			return 1;
		} else {
			if (path) {
				fprintf(stderr, "extra argument: %s\n", argv[i]);
				usage(stderr, argv[0]);
				return 1;
			}
			path = argv[i];
		}
	}

	if (!path || num_frames <= 0) {
		usage(stderr, argv[0]);
		return 1;
	}
	if (algo == ALG_NRU && refresh <= 0) {
		fprintf(stderr, "NRU requires -r <refresh> with refresh > 0\n");
		return 1;
	}

	FILE *fp = fopen(path, "r");
	if (!fp) {
		perror(path);
		return 1;
	}

	TraceEnt *trace = NULL;
	size_t N = 0;
	if (parse_trace(fp, &trace, &N) != 0) {
		fclose(fp);
		fprintf(stderr, "out of memory or parse error\n");
		return 1;
	}
	fclose(fp);
	if (N > (size_t)INT_MAX) {
		fprintf(stderr, "trace too long for %%d summary fields\n");
		free(trace);
		return 1;
	}

	/* OPT: build sorted list of (page, index) for O(log n) “next use” queries */
	PageIdxPair *pairs = malloc(N * sizeof *pairs);
	if (!pairs) {
		free(trace);
		fprintf(stderr, "out of memory\n");
		return 1;
	}
	for (size_t i = 0; i < N; i++) {
		pairs[i].page = trace[i].page;
		pairs[i].idx = (uint32_t)i; /* trace length < 2^32 for this assignment */
	}
	qsort(pairs, N, sizeof *pairs, cmp_pair);

	Frame *frames = calloc((size_t)num_frames, sizeof *frames);
	if (!frames) {
		free(pairs);
		free(trace);
		fprintf(stderr, "out of memory\n");
		return 1;
	}
	for (int i = 0; i < num_frames; i++)
		frames[i].page = UINT32_MAX;

	uint64_t faults = 0;
	uint64_t disk_writes = 0;
	int clock_hand = 0;
	uint32_t rng = 0xC001D00Du;
	size_t nru_counter = 0;

	const char *algo_name = "opt";
	switch (algo) {
	case ALG_OPT:
		algo_name = "opt";
		break;
	case ALG_CLOCK:
		algo_name = "clock";
		break;
	case ALG_RAND:
		algo_name = "rand";
		break;
	case ALG_NRU:
		algo_name = "nru";
		break;
	}

	/* Walk trace once; ti is the access index for OPT “future” queries */
	for (size_t ti = 0; ti < N; ti++) {
		uint32_t p = trace[ti].page;
		AccKind k = trace[ti].kind;

		int fi = find_frame(frames, num_frames, p);
		if (fi >= 0) {
			apply_access_bits(&frames[fi], k);
			if (!quiet)
				print_action("hit");
			if (algo == ALG_NRU) {
				nru_counter++;
				if (refresh > 0 &&
				    (nru_counter % (size_t)refresh) == 0)
					nru_clear_refs(frames, num_frames);
			}
			continue;
		}

		faults++;
		int slot = find_free_frame(frames, num_frames);
		if (slot < 0) {
			/* Need a slot: evict one resident page per policy */
			int vic = -1;
			switch (algo) {
			case ALG_OPT: {
				/* Evict page whose next use is farthest in the future
				 * (largest next index > ti, or INF if never again).
				 * Tie: smallest frame index. */
				uint64_t best_next = 0;
				vic = -1;
				for (int i = 0; i < num_frames; i++) {
					uint32_t q = frames[i].page;
					size_t lo = lower_bound_page(pairs, N, q);
					size_t hi = upper_bound_page(pairs, N, q);
					uint64_t nu = next_use_after(pairs, lo, hi,
								     (uint32_t)ti);
					if (vic < 0 || nu > best_next ||
					    (nu == best_next &&
					     (vic < 0 || i < vic))) {
						best_next = nu;
						vic = i;
					}
				}
				break;
			}
			case ALG_CLOCK:
				/* Scan from clock_hand; ref 1 → second chance, clear and move */
				for (;;) {
					if (frames[clock_hand].ref == 0) {
						vic = clock_hand;
						clock_hand =
							(clock_hand + 1) %
							num_frames;
						break;
					}
					frames[clock_hand].ref = 0;
					clock_hand = (clock_hand + 1) %
						      num_frames;
				}
				break;
			case ALG_RAND:
				/* Uniform frame index in [0, num_frames) */
				vic = (int)(xorshift32(&rng) % (uint32_t)num_frames);
				break;
			case ALG_NRU:
				vic = pick_nru_victim(frames, num_frames);
				break;
			}

			if (vic < 0) {
				fprintf(stderr, "internal error: no victim\n");
				free(frames);
				free(pairs);
				free(trace);
				return 1;
			}

			if (frames[vic].dirty)
				disk_writes++; /* count dirty eviction as write-back */

			if (!quiet) {
				if (frames[vic].dirty)
					print_action(MSG_PF_DIRTY);
				else
					print_action(MSG_PF_CLEAN);
			}

			slot = vic;
			frames[slot].page = UINT32_MAX;
			frames[slot].ref = 0;
			frames[slot].dirty = 0;
		} else {
			if (!quiet)
				print_action(MSG_PF_NONE);
		}

		/* Install faulting page; apply this access (sets ref/dirty) */
		frames[slot].page = p;
		frames[slot].ref = 0;
		frames[slot].dirty = 0;
		apply_access_bits(&frames[slot], k);

		/* Incoming page is considered referenced for CLOCK */
		if (algo == ALG_CLOCK)
			frames[slot].ref = 1;

		if (algo == ALG_NRU) {
			nru_counter++;
			if (refresh > 0 &&
			    (nru_counter % (size_t)refresh) == 0)
				nru_clear_refs(frames, num_frames);
		}
	}

	/* Summary format matches assignment (printf-style field types) */
	printf("Algorithm: %s\n", algo_name);
	printf("Number of frames: %d\n", num_frames);
	printf("Total memory accesses: %d\n", (int)N);
	printf("Total page faults: %d\n", (int)faults);
	printf("Total writes to disk: %d\n", (int)disk_writes);

	free(frames);
	free(pairs);
	free(trace);
	return 0;
}
