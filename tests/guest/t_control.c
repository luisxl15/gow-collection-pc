/* Control flow: jump tables, function pointers, recursion, loops, tail calls. */
#include "../../guest/glibc.h"

__attribute__((noinline)) static int classify(int x) {
  switch (x) {
    case 0: return 11;
    case 1: return 23;
    case 2: return 37;
    case 3: return 41;
    case 4: return 59;
    case 5: return 61;
    case 6: return 79;
    case 7: return 83;
    case 9: return 97;
    case 12: return 101;
    default: return -1;
  }
}

__attribute__((noinline)) static int fib(int n) { return n < 2 ? n : fib(n - 1) + fib(n - 2); }

__attribute__((noinline)) static int ack(int m, int n) {
  if (m == 0) return n + 1;
  if (n == 0) return ack(m - 1, 1);
  return ack(m - 1, ack(m, n - 1));
}

typedef int (*binop)(int, int);
__attribute__((noinline)) static int op_add(int a, int b) { return a + b; }
__attribute__((noinline)) static int op_sub(int a, int b) { return a - b; }
__attribute__((noinline)) static int op_mul(int a, int b) { return a * b; }
__attribute__((noinline)) static int op_xor(int a, int b) { return a ^ b; }
static binop ops[] = {op_add, op_sub, op_mul, op_xor};

/* A tiny bytecode VM: the kind of interpreter loop many games embed. */
enum { PUSH, ADD, MUL, DUP, SWAP, JNZ, DEC, HALT, POP };
__attribute__((noinline)) static int run_vm(const u8 *code) {
  int stack[16], sp = 0, pc = 0;
  for (;;) {
    switch (code[pc++]) {
      case PUSH: stack[sp++] = code[pc++]; break;
      case ADD: sp--, stack[sp - 1] += stack[sp]; break;
      case MUL: sp--, stack[sp - 1] *= stack[sp]; break;
      case DUP: stack[sp] = stack[sp - 1], sp++; break;
      case SWAP: { int t = stack[sp - 1]; stack[sp - 1] = stack[sp - 2]; stack[sp - 2] = t; } break;
      case JNZ: { int target = code[pc++]; if (stack[sp - 1]) pc = target; } break;
      case DEC: stack[sp - 1]--; break;
      case POP: sp--; break;
      case HALT: return stack[sp - 1];
    }
  }
}

__attribute__((noinline)) static unsigned collatz_steps(unsigned n) {
  unsigned steps = 0;
  while (n != 1) n = (n & 1) ? 3 * n + 1 : n / 2, steps++;
  return steps;
}

int main(void) {
  int acc = 0;
  for (int i = -2; i < 15; i++) acc = acc * 31 + classify(i);
  g_printf("switch acc=%d\n", acc);
  g_printf("fib(24)=%d ack(2,3)=%d\n", fib(24), ack(2, 3));
  int v = 7;
  for (int i = 0; i < 20; i++) v = ops[i & 3](v, i + 3);
  g_printf("fnptr v=%d\n", v);
  /* [acc=3, n=10]; loop: n--; acc += acc; while n -> 3 << 10 */
  static const u8 prog[] = {PUSH, 3, PUSH, 10, DEC, SWAP, DUP, ADD, SWAP, JNZ, 4, POP, HALT};
  g_printf("vm=%d\n", run_vm(prog));
  unsigned best = 0, arg = 0;
  for (unsigned n = 1; n < 3000; n++) {
    unsigned s = collatz_steps(n);
    if (s > best) best = s, arg = n;
  }
  g_printf("collatz best n=%u steps=%u\n", arg, best);
  return 0;
}
