; Cross-function fixture for the analysis-wide Box scalar carrier. Caller and
; callee variables use stable identities, so calls require no state-local
; vocabulary growth or coordinate alignment during fixpoint iteration.

define i32 @callee(i32 %callee_x) {
entry:
  %callee_y = add nsw i32 %callee_x, 1
  ret i32 %callee_y
}

define i32 @main(i32 %caller_x, ptr %argv) {
entry:
  %scalar_result = call i32 @callee(i32 %caller_x)
  %scalar_condition = icmp sgt i32 %scalar_result, 0
  br i1 %scalar_condition, label %positive, label %non_positive

positive:
  br label %merge

non_positive:
  br label %merge

merge:
  %scalar_phi = phi i32 [ %scalar_result, %positive ], [ 0, %non_positive ]
  ret i32 %scalar_phi
}
