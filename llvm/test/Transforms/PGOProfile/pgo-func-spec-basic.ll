; RUN: opt -S %s -passes=pgo-function-specialization \
; RUN:   -force-pgo-specialization \
; RUN:   -pgofuncspec-min-function-size=1 \
; RUN:   -pgofuncspec-min-latency-thresh=0 \
; RUN:   -pgofuncspec-blowup-factor=999999 \
; RUN:   -pgofuncspec-analysis-cutoff-thresh=1 | FileCheck %s

target datalayout = "e-m:e-p270:32:32-p271:32:32-p272:64:64-i64:64-f80:128-n8:16:32:64-S128"
target triple = "x86_64-unknown-linux-gnu"

define i32 @compute(i32 %x) !prof !27 {
entry:
  %cmp = icmp eq i32 %x, 42
  br i1 %cmp, label %if.then, label %if.else, !prof !28

if.then:
  %a = add i32 %x, 1
  %b = mul i32 %a, 3
  %c = add i32 %b, 7
  %d = sdiv i32 %c, 2
  %e = add i32 %d, 10
  br label %if.end

if.else:
  %f = sub i32 %x, 1
  %g = mul i32 %f, 5
  %h = add i32 %g, 13
  %i = sdiv i32 %h, 3
  %j = sub i32 %i, 4
  br label %if.end

if.end:
  %result = phi i32 [ %e, %if.then ], [ %j, %if.else ]
  ret i32 %result
}

define i32 @caller(i32 %arg) !prof !27 {
entry:
  ; CHECK-LABEL: @caller
  ; CHECK: switch i32 %arg, label %CS.Default
  ; CHECK: CS.Case.0:
  ; CHECK: call i32 @compute.spec.0.42(i32 42)
  ; CHECK: CS.Default:
  ; CHECK: call i32 @compute(i32 %arg)
  ; CHECK: CS.Merge:
  ; CHECK: phi i32
  %result = call i32 @compute(i32 %arg), !prof !29
  ret i32 %result
}

; CHECK: define {{.*}} @compute.spec.0.42(i32 %x)


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
; function_entry_count
!27 = !{!"function_entry_count", i64 1000}
; branch_weights for the icmp
!28 = !{!"branch_weights", i32 800, i32 200}
; VP metadata: kind=3 (IPVK_ArgumentValue), total=1000, 1 entry: value=42 count=800
!29 = !{!"VP", i32 3, i64 1000, i32 1, i64 42, i64 800}
