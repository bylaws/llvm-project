; RUN: opt -S %s -passes=pgo-vtable-function-specialization \
; RUN:   -enable-pgo-vtable-func-spec | FileCheck %s

target datalayout = "e-m:e-p270:32:32-p271:32:32-p272:64:64-i64:64-f80:128-n8:16:32:64-S128"
target triple = "x86_64-unknown-linux-gnu"

@_ZTV4Base = constant { [3 x ptr] } { [3 x ptr] [
  ptr null, ptr null, ptr @vfunc
] }, !type !0

declare void @vfunc()
declare i1 @llvm.type.test(ptr, metadata)

define void @no_nonnull(ptr %obj) {
  %vptr = load ptr, ptr %obj
  %tt = call i1 @llvm.type.test(ptr %vptr, metadata !"_ZTS4Base")
  %fptr = load ptr, ptr %vptr
  call void %fptr()
  ret void
}

define void @call_no_nonnull(ptr %p) {
  ; CHECK-LABEL: @call_no_nonnull
  ; CHECK-NOT: CS.cmp
  ; CHECK: call void @no_nonnull(ptr %p)
  call void @no_nonnull(ptr %p), !prof !1
  ret void
}

define void @no_vdispatch(ptr nonnull %obj) {
  store ptr null, ptr %obj
  ret void
}

define void @call_no_vdispatch(ptr %p) {
  ; CHECK-LABEL: @call_no_vdispatch
  ; CHECK-NOT: CS.cmp
  ; CHECK: call void @no_vdispatch(ptr %p)
  call void @no_vdispatch(ptr %p), !prof !1
  ret void
}

define void @low_dominance(ptr nonnull %obj) {
  %vptr = load ptr, ptr %obj
  %tt = call i1 @llvm.type.test(ptr %vptr, metadata !"_ZTS4Base")
  %fptr = load ptr, ptr %vptr
  call void %fptr()
  ret void
}

define void @call_low_dominance(ptr %p) {
  ; CHECK-LABEL: @call_low_dominance
  ; CHECK-NOT: CS.cmp
  ; CHECK: call void @low_dominance(ptr %p)
  call void @low_dominance(ptr %p), !prof !2
  ret void
}

define void @small_md5(ptr nonnull %obj) {
  %vptr = load ptr, ptr %obj
  %tt = call i1 @llvm.type.test(ptr %vptr, metadata !"_ZTS4Base")
  %fptr = load ptr, ptr %vptr
  call void %fptr()
  ret void
}

define void @call_small_md5(ptr %p) {
  ; CHECK-LABEL: @call_small_md5
  ; CHECK-NOT: CS.cmp
  ; CHECK: call void @small_md5(ptr %p)
  call void @small_md5(ptr %p), !prof !3
  ret void
}

; CHECK-NOT: define {{.*}}.spec.vtable.

!0 = !{i64 16, !"_ZTS4Base"}
!1 = !{!"VP", i32 3, i64 1000, i32 1, i64 1960855528937986108, i64 950}
!2 = !{!"VP", i32 3, i64 1000, i32 1, i64 1960855528937986108, i64 500}
!3 = !{!"VP", i32 3, i64 1000, i32 1, i64 42, i64 950}
