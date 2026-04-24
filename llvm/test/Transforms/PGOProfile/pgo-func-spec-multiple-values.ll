; RUN: opt -S %s -passes=pgo-function-specialization \
; RUN:   -force-pgo-specialization \
; RUN:   -pgofuncspec-min-function-size=1 \
; RUN:   -pgofuncspec-min-latency-thresh=0 \
; RUN:   -pgofuncspec-blowup-factor=999999 \
; RUN:   -pgofuncspec-candidate-cutoff-thresh=100 \
; RUN:   -pgofuncspec-dispatch-cost=0 \
; RUN:   -pgofuncspec-analysis-cutoff-thresh=1 | FileCheck %s

target datalayout = "e-m:e-p270:32:32-p271:32:32-p272:64:64-i64:64-f80:128-n8:16:32:64-S128"
target triple = "x86_64-unknown-linux-gnu"

define i32 @compute(i32 %x) !prof !27 {
entry:
  %cmp1 = icmp eq i32 %x, 42
  br i1 %cmp1, label %path.a, label %check2, !prof !28

check2:
  %cmp2 = icmp eq i32 %x, 7
  br i1 %cmp2, label %path.b, label %path.c, !prof !28

path.a:
  %a = mul i32 %x, 3
  %b = add i32 %a, 7
  %c = sdiv i32 %b, 2
  %d = add i32 %c, 10
  br label %exit

path.b:
  %e = mul i32 %x, 5
  %f = sub i32 %e, 13
  %g = sdiv i32 %f, 3
  %h = add i32 %g, 4
  br label %exit

path.c:
  %i = add i32 %x, 100
  %j = mul i32 %i, 2
  %k = sdiv i32 %j, 7
  %l = sub i32 %k, 3
  br label %exit

exit:
  %result = phi i32 [ %d, %path.a ], [ %h, %path.b ], [ %l, %path.c ]
  ret i32 %result
}

define i32 @caller(i32 %arg) !prof !27 {
entry:
  ; VP metadata: 2 values — 42 (50%) and 7 (40%)
  ; CHECK-LABEL: @caller
  ; CHECK: switch i32 %arg, label %CS.Default [
  ; CHECK:   i32 {{42|7}}, label %CS.Case.
  ; CHECK:   i32 {{42|7}}, label %CS.Case.
  ; CHECK: ]
  ; CHECK: CS.Case.0:
  ; CHECK: CS.Case.1:
  ; CHECK: CS.Default:
  ; CHECK: call i32 @compute(i32 %arg)
  ; CHECK: CS.Merge:
  %result = call i32 @compute(i32 %arg), !prof !29
  ret i32 %result
}

; CHECK-DAG: define {{.*}} @compute.spec.0.42
; CHECK-DAG: define {{.*}} @compute.spec.0.7

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
; VP metadata: kind=3, total=1000, 2 entries: value=42 count=500, value=7 count=400
!29 = !{!"VP", i32 3, i64 1000, i32 2, i64 42, i64 500, i64 7, i64 400}
