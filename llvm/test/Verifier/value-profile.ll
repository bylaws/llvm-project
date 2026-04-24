; Test MD_prof "VP" validation

; RUN: split-file %s %t
; RUN: opt -passes=verify %t/valid.ll --disable-output
; RUN: not opt -passes=verify %t/invalid-kind.ll --disable-output 2>&1 | FileCheck %s --check-prefix=INVALID-KIND
; RUN: not opt -passes=verify %t/invalid-count.ll --disable-output 2>&1 | FileCheck %s --check-prefix=INVALID-COUNT
; RUN: not opt -passes=verify %t/invalid-place.ll --disable-output 2>&1 | FileCheck %s --check-prefix=INVALID-PLACE

;--- valid.ll
define void @test(ptr %0) {
  call void %0(), !prof !0
  ret void
}
!0 = !{!"VP", i32 0, i64 20, i32 2, i64 1234, i64 10, i64 5678, i64 5}

;--- invalid-kind.ll
define void @test(ptr %0) {
  call void %0(), !prof !0
  ret void
}
!0 = !{!"VP", i32 5, i64 20, i32 2, i64 1234, i64 10, i64 5678, i64 5}
; INVALID-KIND: Invalid VP !prof kind

;--- invalid-count.ll
define void @test(ptr %0) {
  call void %0(), !prof !0
  ret void
}
!0 = !{!"VP", i32 1, i64 1234, i32 10, i64 5678, i64 5}
; INVALID-COUNT: VP !prof is too small for its associated entry count

;--- invalid-place.ll
define i32 @test(i32 %0) {
  %r = add i32 %0, 1, !prof !0
  ret i32 %r
}
!0 = !{!"VP", i32 1, i64 20, i32 2, i64 1234, i64 10, i64 5678, i64 5}
; INVALID-PLACE: VP !prof indirect call or memop size expected to be applied to CallBase instructions only
