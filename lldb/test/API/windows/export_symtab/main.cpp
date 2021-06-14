// Put function f into the export table.
__declspec(dllexport) int f() {
  __asm__("int3;");
  return 42;
}

int g() { return f(); }

int main() { return g(); }
