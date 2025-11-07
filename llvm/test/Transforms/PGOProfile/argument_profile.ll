; RUN: opt < %s -passes=pgo-instr-gen -enable-argument-value-profiling -S | FileCheck %s --check-prefix=GEN
; RUN: opt < %s -passes=pgo-instr-gen,instrprof -enable-argument-value-profiling -S | FileCheck %s --check-prefix=LOWER

target datalayout = "e-m:e-p270:32:32-p271:32:32-p272:64:64-i64:64-f80:128-n8:16:32:64-S128"
target triple = "x86_64-unknown-linux-gnu"

; GEN: __llvm_profile_raw_version = comdat any
; GEN: __llvm_profile_raw_version = hidden constant i64 72057594037927946, comdat
; GEN: __profn_main = private constant [4 x i8] c"main"

; IR is derived from the following C program
; extern int foo(int);
; int main() {
;   foo(42);
;   for (int i = 0; i < 10; i++)
;     foo(i);
;   return 0;
; }
declare i32 @foo(i32 %x)

define i32 @main() {
; GEN-LABEL: @main()
; LOWER-LABEL: @main()
entry:
  br label %for.body
for.body:
  %i = phi i32 [ 0, %entry ], [ %inc, %for.inc ]
; GEN: call void @llvm.instrprof.value.profile(ptr @__profn_main, i64 {{[0-9]+}}, i64 42, i32 3, i32 0)
; GEN-NEXT: %call1 = call i32 @foo(i32 42)
; LOWER: call void @__llvm_profile_instrument_target(i64 42, ptr @__profd_main, i32 {{[0-9]+}})
; LOWER-NEXT: %call1 = call i32 @foo(i32 42)
  %call1 = call i32 @foo(i32 42)
; GEN: [[ZEXT:%[0-9]+]] = zext i32 %i to i64
; GEN-NEXT: call void @llvm.instrprof.value.profile(ptr @__profn_main, i64 {{[0-9]+}}, i64 [[ZEXT]], i32 3, i32 1)
; GEN-NEXT: %call2 = call i32 @foo(i32 %i)
; LOWER: [[ZEXT:%[0-9]+]] = zext i32 %i to i64
; LOWER-NEXT: call void @__llvm_profile_instrument_target(i64 [[ZEXT]], ptr @__profd_main, i32 {{[0-9]+}})
; LOWER-NEXT: %call2 = call i32 @foo(i32 %i)
  %call2 = call i32 @foo(i32 %i)
  br label %for.inc
for.inc:
  %inc = add i32 %i, 1
  %cmp = icmp slt i32 %inc, 10
  br i1 %cmp, label %for.body, label %for.end
for.end:
  ret i32 0
}
