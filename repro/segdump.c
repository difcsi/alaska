// Crash characterizer for the binarytrees UAF. LD_PRELOAD this; on SIGSEGV/SIGBUS it reports the
// fault address + PC (+ poison shape + move-ring hit), AND decodes every handle-looking value in the
// GP registers and the top of the stack into its mapping state (backing / refcount / pinned / invl).
// Runs full-speed in the signal handler (no gdb), so the timing-sensitive race is NOT masked.
#define _GNU_SOURCE
#include <signal.h>
#include <stdio.h>
#include <string.h>
#include <ucontext.h>
#include <unistd.h>
#include <dlfcn.h>

extern int alaska_addr_recent_move(void *p, void **old, void **neu, void **map) __attribute__((weak));

static int poison_shaped(unsigned long v) {
  if (v == 0xf0f0f0f0f0f0f0f0UL) return 1;
  int run = 0;
  for (int b = 0; b < 8; b++) if (((v >> (b * 8)) & 0xff) == 0xf0) run++;
  return run >= 3;
}

// Handle layout: H>>0x1d = Mapping* ; *M low 47 bits = backing ; bits47-60 refcount ; 61 pinned ;
// 62 invl. The handle table sits in [0x400000000, 0x401000000); only read M inside it.
static void decode(const char *name, unsigned long h) {
  if (!(h & 0x8000000000000000UL)) return;  // top bit set == handle
  unsigned long M = h >> 0x1dUL;
  if (M < 0x400000000UL || M >= 0x401000000UL) {
    fprintf(stderr, "[segdump]   %-9s handle=%#lx M=%#lx OUT-OF-TABLE\n", name, h, M);
    return;
  }
  unsigned long w = *(volatile unsigned long *)M;
  fprintf(stderr, "[segdump]   %-9s handle=%#lx M=%#lx backing=%#lx rc=%lu pinned=%lu invl=%lu\n",
      name, h, M, (w & 0x7fffffffffffUL), (w >> 47) & 0x3fffUL, (w >> 61) & 1UL, (w >> 62) & 1UL);
}

static volatile sig_atomic_t in_handler = 0;

static void handler(int sig, siginfo_t *si, void *uc_) {
  if (in_handler) _exit(140);  // nested fault while decoding -> bail
  in_handler = 1;
  ucontext_t *uc = (ucontext_t *)uc_;
  greg_t *g = uc->uc_mcontext.gregs;
  void *fault = si->si_addr;
  void *pc = (void *)g[REG_RIP];
  const char *mod = "?", *sym = "?";
  long off = 0, symoff = 0;
  Dl_info di;
  if (dladdr(pc, &di)) {
    if (di.dli_fname) mod = di.dli_fname;
    off = (char *)pc - (char *)di.dli_fbase;
    if (di.dli_sname) { sym = di.dli_sname; symoff = (char *)pc - (char *)di.dli_saddr; }
  }
  fprintf(stderr, "\n[segdump] sig=%d fault_addr=%p poison_shaped=%d\n", sig, fault,
      poison_shaped((unsigned long)fault));
  fprintf(stderr, "[segdump] pc=%p  ->  %s+0x%lx  (sym %s+0x%lx)\n", pc, mod, off, sym, symoff);

  void *old = 0, *neu = 0, *map = 0;
  if (&alaska_addr_recent_move && alaska_addr_recent_move(fault, &old, &neu, &map))
    fprintf(stderr, "[segdump] fault_addr IS a recent-move old: new=%p map=%p\n", neu, map);

  struct { const char *n; int i; } R[] = {{"rax", REG_RAX}, {"rbx", REG_RBX}, {"rcx", REG_RCX},
      {"rdx", REG_RDX}, {"rsi", REG_RSI}, {"rdi", REG_RDI}, {"rbp", REG_RBP}, {"r8", REG_R8},
      {"r9", REG_R9}, {"r10", REG_R10}, {"r11", REG_R11}, {"r12", REG_R12}, {"r13", REG_R13},
      {"r14", REG_R14}, {"r15", REG_R15}};
  for (int k = 0; k < 15; k++)
    fprintf(stderr, "[segdump]   %-3s=%p\n", R[k].n, (void *)g[R[k].i]);

  // Decode handle-looking registers into their mapping state.
  fprintf(stderr, "[segdump] -- decoded register handles --\n");
  for (int k = 0; k < 15; k++) decode(R[k].n, (unsigned long)g[R[k].i]);

  // Scan the top of the stack for handle-looking words and decode them (find the parent node[s]).
  fprintf(stderr, "[segdump] -- stack handles [rsp..rsp+0x300] --\n");
  unsigned long sp = (unsigned long)g[REG_RSP];
  for (int i = 0; i < 96; i++) {
    unsigned long v = *(volatile unsigned long *)(sp + i * 8);
    if (v & 0x8000000000000000UL) {
      char nm[24];
      snprintf(nm, sizeof nm, "[sp+%#x]", i * 8);
      decode(nm, v);
    }
  }
  // The faulting "node" is a garbage value with bit63 CLEAR (translate returned it as-is). Test
  // whether it is the HIGH 32 bits of a real handle: reconstruct (V<<32) and decode that mapping.
  unsigned long V = (unsigned long)g[REG_RAX];
  if (V != 0 && (V & 0x8000000000000000UL) == 0 && (V & 0xffffffff00000000UL) == 0) {
    unsigned long recon = V << 32;
    fprintf(stderr, "[segdump] -- reconstruct (rax<<32)=%#lx --\n", recon);
    decode("V<<32", recon);
  }

  // Dump the parent node's backing (rdx held it at the fault) to see the corrupted struct.
  unsigned long pb = (unsigned long)g[REG_RDX];
  if (pb >= 0x7f0000000000UL && pb < 0x800000000000UL) {
    fprintf(stderr, "[segdump] -- parent backing @rdx=%#lx --\n", pb);
    for (int i = 0; i < 8; i++)
      fprintf(stderr, "[segdump]   +%#x: %#018lx\n", i * 8, *(volatile unsigned long *)(pb + i * 8));
  }
  // The parent handle is the first valid stack handle ([sp+0x8]); dump ITS mapping word and the
  // memory its backing points at (is that backing == rdx? does it look like headers or a node?).
  unsigned long ph = *(volatile unsigned long *)((unsigned long)g[REG_RSP] + 8);
  if (ph & 0x8000000000000000UL) {
    unsigned long pM = ph >> 0x1dUL;
    if (pM >= 0x400000000UL && pM < 0x401000000UL) {
      unsigned long pw = *(volatile unsigned long *)pM;
      unsigned long pbk = pw & 0x7fffffffffffUL;
      fprintf(stderr, "[segdump] -- parent[sp+8] handle=%#lx M=%#lx word=%#lx backing=%#lx --\n", ph, pM, pw, pbk);
    }
  }
  // Dump the RECONSTRUCTED child's backing (V<<32) — if it's a clean {left,right} node, the real
  // tree is intact and only the parent's stored pointer is wrong.
  if (V != 0 && (V & 0xffffffff00000000UL) == 0) {
    unsigned long cM = (V << 32) >> 0x1dUL;
    if (cM >= 0x400000000UL && cM < 0x401000000UL) {
      unsigned long cbk = (*(volatile unsigned long *)cM) & 0x7fffffffffffUL;
      if (cbk >= 0x7f0000000000UL && cbk < 0x800000000000UL) {
        fprintf(stderr, "[segdump] -- reconstructed child backing @%#lx (should be a node) --\n", cbk);
        for (int i = 0; i < 4; i++)
          fprintf(stderr, "[segdump]   +%#x: %#018lx\n", i * 8, *(volatile unsigned long *)(cbk + i * 8));
      }
    }
  }
  fprintf(stderr, "[segdump] end\n");
  _exit(139);
}

__attribute__((constructor)) static void seg_init(void) {
  struct sigaction sa;
  memset(&sa, 0, sizeof sa);
  sa.sa_sigaction = handler;
  sa.sa_flags = SA_SIGINFO;
  sigemptyset(&sa.sa_mask);
  sigaction(SIGSEGV, &sa, 0);
  sigaction(SIGBUS, &sa, 0);
}
