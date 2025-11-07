// REQUIRES: lld-available

// Building the instrumented binary will fail because lld doesn't support
// big-endian ELF for PPC (aka ABI 1).
// ld.lld: error: /lib/../lib64/Scrt1.o: ABI version 1 is not supported
// UNSUPPORTED: ppc && host-byteorder-big-endian

// RUN: rm -rf %t && mkdir %t && split-file %s %t && cd %t

// RUN: %clangxx_pgogen -fuse-ld=lld -fprofile-generate=. -mllvm -enable-argument-value-profiling lib.cpp main.cpp -o test
// RUN: env LLVM_PROFILE_FILE=test.profraw ./test

// Show argument value profiles from raw profile.
// RUN: llvm-profdata show --function=main --argument-values test.profraw | FileCheck %s

// Generate indexed profile from raw profile and show the data.
// RUN: llvm-profdata merge test.profraw -o test.profdata
// RUN: llvm-profdata show --function=main --argument-values test.profdata | FileCheck %s

// Generate text profile and verify
// RUN: llvm-profdata merge --text test.profraw -o raw.proftext
// RUN: llvm-profdata show --function=main --argument-values --text raw.proftext | FileCheck %s --check-prefix=ARGTEXT

// CHECK: Counters:
// CHECK:   main:
// CHECK:   Hash: 0x{{[0-9a-f]+}}
// CHECK:   Counters: 9
// CHECK:   Number of instrumented argument values: 8

// Site 0
// CHECK:  Argument Value Results:
// CHECK-DAG:       [  0, 42, 100 ] (100.00%)

// Site 1
// CHECK-DAG:       [  1, 1000, {{[0-9]+}} ]
// CHECK-DAG:       [  1,  999, {{[0-9]+}} ]
// CHECK-DAG:       [  1,  998, {{[0-9]+}} ]
// CHECK-DAG:       [  1,  997, {{[0-9]+}} ]

// Site 2
// CHECK-DAG:       [  2, 100, 50 ] (100.00%)

// Site 3
// CHECK-DAG:       [  3, 200, 50 ] (100.00%)

// Site 4: arg0
// CHECK-DAG:       [  4, 10, 80 ] (80.00%)
// CHECK-DAG:       [  4, 20, 20 ] (20.00%)

// Site 5: arg1
// CHECK-DAG:       [  5, 777, 100 ] (100.00%)

// Site 6: arg2
// CHECK-DAG:       [  6, 5, 70 ] (70.00%)
// CHECK-DAG:       [  6, 15, 30 ] (30.00%)

// Site 7
// CHECK-DAG:       [  7, 333932400, 1 ] (100.00%)

// CHECK: Instrumentation level: IR  entry_first = 0
// CHECK: Functions shown: 1
// CHECK: Total functions: 5
// CHECK: Statistics for argument values profile:
// CHECK:   Total number of sites: 8
// CHECK:   Total number of sites with values: 8
// CHECK:   Total number of profiled values: 33
// CHECK:   Value sites histogram:
// CHECK:     NumTargets, SiteCount
// CHECK:     1, 5
// CHECK:     2, 2
// CHECK:     24, 1

// Check text format output
// ARGTEXT: :ir
// ARGTEXT: main
// ARGTEXT: # Func Hash:
// ARGTEXT: {{[0-9]+}}
// ARGTEXT: # Num Counters:
// ARGTEXT: 9
// ARGTEXT: # Counter Values:
// ARGTEXT: 500500
// ARGTEXT: 100
// ARGTEXT: 1000
// ARGTEXT: 50
// ARGTEXT: 50
// ARGTEXT: 100
// ARGTEXT: # Num Value Kinds:
// ARGTEXT: 1
// ARGTEXT: # ValueKind = IPVK_ArgumentValue:
// ARGTEXT: 3
// ARGTEXT: # NumValueSites:
// ARGTEXT: 8
// Site 0
// ARGTEXT: 1
// ARGTEXT: 42:100
// Site 1
// ARGTEXT: 24
// ARGTEXT-DAG: 1000:{{[0-9]+}}
// ARGTEXT-DAG: 999:{{[0-9]+}}
// ARGTEXT-DAG: 998:{{[0-9]+}}
// Site 2
// ARGTEXT: 1
// ARGTEXT: 100:50
// Site 3
// ARGTEXT: 1
// ARGTEXT: 200:50
// Site 4: arg0
// ARGTEXT: 2
// ARGTEXT: 10:80
// ARGTEXT: 20:20
// Site 5: arg1
// ARGTEXT: 1
// ARGTEXT: 777:100
// Site 6: arg2
// ARGTEXT: 2
// ARGTEXT: 5:70
// ARGTEXT: 15:30
// Site 7
// ARGTEXT: 1
// ARGTEXT: 333932400:1

//--- lib.h
#include <stdio.h>

__attribute__((noinline)) int foo(int x);

__attribute__((noinline)) int bar(int x, int y, int z);

__attribute__((noinline)) float fooFloat(float x);

__attribute__((noinline)) double fooDouble(double x);

//--- lib.cpp
#include "lib.h"

int foo(int x) {
  return x;
}

int bar(int x, int y, int z) {
  return x + y + z;
}

float fooFloat(float x) {
  return x;
}

double fooDouble(double x) {
  return x;
}

//--- main.cpp
#include "lib.h"

int main() {
  int sum = 0;

  for (int i = 0; i < 100; i++) {
    sum += foo(42);
  }

  for (int i = 1; i <= 1000; i++) {
    for (int j = 0; j < i; j++) {
      sum += foo(i);
    }
  }

  for (int i = 0; i < 50; i++) {
    sum += foo(100);
  }

  for (int i = 0; i < 50; i++) {
    sum += foo(200);
  }

  for (int i = 0; i < 100; i++) {
    sum += bar((i < 80) ? 10 : 20, 777, (i < 70) ? 5 : 15);
  }

  printf("sum is %d\n", sum);
  return 0;
}
