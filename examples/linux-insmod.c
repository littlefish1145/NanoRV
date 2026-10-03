// SPDX-License-Identifier: MIT
// Tiny Linux finit_module helper for the fast initrd (no libc dependency).
static long call3(long number, long a, long b, long c) {
  register long a0 __asm__("a0") = a;
  register long a1 __asm__("a1") = b;
  register long a2 __asm__("a2") = c;
  register long a7 __asm__("a7") = number;
  __asm__ volatile("ecall" : "+r"(a0) : "r"(a1), "r"(a2), "r"(a7) : "memory");
  return a0;
}
static void message(const char* text) {
  long length = 0; while (text[length]) ++length;
  call3(64, 2, (long)text, length);
}
void start_c(long argc, char** argv) {
  long result = -1;
  if (argc == 2) {
    long fd = call3(56, -100, (long)argv[1], 0);
    if (fd >= 0) {
      result = call3(273, fd, (long)"", 0);
      call3(57, fd, 0, 0);
    }
    if (result < 0) { message("RVVM: finit_module failed: "); message(argv[1]); message("\n"); }
  }
  call3(93, result < 0 ? 1 : 0, 0, 0);
  for (;;) {}
}
__asm__(".global _start\n_start:\n ld a0,0(sp)\n addi a1,sp,8\n tail start_c\n");
