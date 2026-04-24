; RUN: opt -S %s -passes=pgo-function-specialization \
; RUN:   -force-pgo-specialization \
; RUN:   -pgofuncspec-min-function-size=1 \
; RUN:   -pgofuncspec-min-latency-thresh=0 \
; RUN:   -pgofuncspec-blowup-factor=999999 \
; RUN:   -pgofuncspec-analysis-cutoff-thresh=1 | FileCheck %s

target datalayout = "e-m:e-p270:32:32-p271:32:32-p272:64:64-i64:64-f80:128-n8:16:32:64-S128"
target triple = "x86_64-unknown-linux-gnu"

define float @compute_float(float %x) !prof !27 {
entry:
  %cmp = fcmp oeq float %x, 1.000000e+00
  br i1 %cmp, label %if.then, label %if.else, !prof !28

if.then:
  %a = fmul float %x, 3.000000e+00
  %b = fadd float %a, 7.000000e+00
  %c = fdiv float %b, 2.000000e+00
  %d = fadd float %c, 1.000000e+01
  br label %if.end

if.else:
  %f = fsub float %x, 1.000000e+00
  %g = fmul float %f, 5.000000e+00
  %h = fadd float %g, 1.300000e+01
  %i = fdiv float %h, 3.000000e+00
  %j = fsub float %i, 4.000000e+00
  br label %if.end

if.end:
  %result = phi float [ %d, %if.then ], [ %j, %if.else ]
  ret float %result
}

define float @caller(float %arg) !prof !27 {
entry:
  ; CHECK-LABEL: @caller
  ; CHECK: bitcast float %arg to i32
  ; CHECK: switch i32
  ; CHECK: CS.Case.0:
  ; CHECK: call float @compute_float.spec.0.
  ; CHECK: CS.Default:
  ; CHECK: call float @compute_float(float %arg)
  ; CHECK: CS.Merge:
  %result = call float @compute_float(float %arg), !prof !29
  ret float %result
}

; CHECK: define {{.*}} @compute_float.spec.0.

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
!28 = !{!"branch_weights", i32 800, i32 200}
; VP metadata: kind=3, total=1000, 1 entry
; float 1.0 = 0x3F800000 = 1065353216 in i64
!29 = !{!"VP", i32 3, i64 1000, i32 1, i64 1065353216, i64 800}
