; RUN: opt -S %s -passes=pgo-function-specialization \
; RUN:   -pgofuncspec-min-function-size=50 \
; RUN:   -pgofuncspec-analysis-cutoff-thresh=1 | FileCheck %s

; Negative tests: various bail-out conditions. None of these functions
; should be specialized.

target datalayout = "e-m:e-p270:32:32-p271:32:32-p272:64:64-i64:64-f80:128-n8:16:32:64-S128"
target triple = "x86_64-unknown-linux-gnu"

define i32 @too_small(i32 %x) !prof !27 {
  %r = add i32 %x, 1
  ret i32 %r
}

define i32 @call_too_small(i32 %arg) !prof !27 {
  ; CHECK-LABEL: @call_too_small
  ; CHECK-NOT: .spec.
  ; CHECK: call i32 @too_small(i32 %arg)
  ; CHECK: ret
  %r = call i32 @too_small(i32 %arg), !prof !29
  ret i32 %r
}

define i32 @recursive_fn(i32 %x) !prof !27 {
entry:
  %cmp = icmp eq i32 %x, 0
  br i1 %cmp, label %base, label %rec, !prof !28

base:
  ret i32 1

rec:
  %x1 = sub i32 %x, 1
  %r = call i32 @recursive_fn(i32 %x1)
  %m = mul i32 %x, %r
  ret i32 %m
}

define i32 @call_recursive(i32 %arg) !prof !27 {
  ; CHECK-LABEL: @call_recursive
  ; CHECK-NOT: .spec.
  ; CHECK: call i32 @recursive_fn(i32 %arg)
  ; CHECK: ret
  %r = call i32 @recursive_fn(i32 %arg), !prof !29
  ret i32 %r
}

define i32 @noduplicate_fn(i32 %x) noduplicate !prof !27 {
  %r = add i32 %x, 42
  ret i32 %r
}

define i32 @call_noduplicate(i32 %arg) !prof !27 {
  ; CHECK-LABEL: @call_noduplicate
  ; CHECK-NOT: .spec.
  ; CHECK: call i32 @noduplicate_fn(i32 %arg)
  ; CHECK: ret
  %r = call i32 @noduplicate_fn(i32 %arg), !prof !29
  ret i32 %r
}

define i32 @already_specialized(i32 %x) "pgo.specialization" !prof !27 {
  %r = mul i32 %x, 3
  ret i32 %r
}

define i32 @call_already_specialized(i32 %arg) !prof !27 {
  ; CHECK-LABEL: @call_already_specialized
  ; CHECK-NOT: .spec.
  ; CHECK: call i32 @already_specialized(i32 %arg)
  ; CHECK: ret
  %r = call i32 @already_specialized(i32 %arg), !prof !29
  ret i32 %r
}

define i32 @no_args() !prof !27 {
  ret i32 0
}

define i32 @call_no_args() !prof !27 {
  ; CHECK-LABEL: @call_no_args
  ; CHECK-NOT: .spec.
  ; CHECK: call i32 @no_args()
  ; CHECK: ret
  %r = call i32 @no_args()
  ret i32 %r
}

; CHECK-NOT: define {{.*}} @too_small.spec
; CHECK-NOT: define {{.*}} @recursive_fn.spec
; CHECK-NOT: define {{.*}} @noduplicate_fn.spec
; CHECK-NOT: define {{.*}} @already_specialized.spec
; CHECK-NOT: define {{.*}} @no_args.spec

!llvm.module.flags = !{!0}

!0 = !{i32 1, !"ProfileSummary", !1}
!1 = !{!2, !3, !4, !5, !6, !7, !8, !9}
!2 = !{!"ProfileFormat", !"InstrProf"}
!3 = !{!"TotalCount", i64 10000}
!4 = !{!"MaxCount", i64 1000}
!5 = !{!"MaxInternalCount", i64 1000}
!6 = !{!"MaxFunctionCount", i64 1000}
!7 = !{!"NumCounts", i64 3}
!8 = !{!"NumFunctions", i64 3}
!9 = !{!"DetailedSummary", !10}
!10 = !{!11, !12, !13}
!11 = !{i32 10000, i64 1000, i32 1}
!12 = !{i32 990000, i64 1, i32 1}
!13 = !{i32 999999, i64 1, i32 1}
!27 = !{!"function_entry_count", i64 1000}
!28 = !{!"branch_weights", i32 500, i32 500}
!29 = !{!"VP", i32 3, i64 1000, i32 1, i64 42, i64 800}
