; RUN: opt -S %s -passes=pgo-vtable-function-specialization \
; RUN:   -enable-pgo-vtable-func-spec | FileCheck %s

target datalayout = "e-m:e-p270:32:32-p271:32:32-p272:64:64-i64:64-f80:128-n8:16:32:64-S128"
target triple = "x86_64-unknown-linux-gnu"

@_ZTV4Base = constant { [3 x ptr] } { [3 x ptr] [
  ptr null,
  ptr null,
  ptr @vfunc
] }, !type !0

declare void @vfunc()

declare i1 @llvm.type.test(ptr, metadata)

define void @process(ptr nonnull %obj) {
entry:
  %vptr = load ptr, ptr %obj
  %typetest = call i1 @llvm.type.test(ptr %vptr, metadata !"_ZTS4Base")
  %fptr = load ptr, ptr %vptr
  call void %fptr()
  ret void
}

; MD5 hash of "_ZTV4Base" = 1960855528937986108
define void @caller(ptr %p) {
entry:
  ; CHECK-LABEL: @caller
  ; CHECK: %CS.vptr = load ptr, ptr %p
  ; CHECK: %CS.cmp = icmp eq ptr %CS.vptr,
  ; CHECK: br i1 %CS.cmp, label %CS.Case, label %CS.Default
  ; CHECK: CS.Case:
  ; CHECK: call void @process.spec.vtable.
  ; CHECK: CS.Default:
  ; CHECK: call void @process(ptr %p)
  call void @process(ptr %p), !prof !1
  ret void
}

; CHECK: define {{.*}} @process.spec.vtable.1960855528937986108

!0 = !{i64 16, !"_ZTS4Base"}
!1 = !{!"VP", i32 3, i64 1000, i32 1, i64 1960855528937986108, i64 950}
