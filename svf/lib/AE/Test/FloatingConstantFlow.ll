; Floating constants are semantic values, not absent abstract-state entries.
; All AE sparsity modes must expose their exact interval to transfers.

define i32 @main() {
entry:
  %floating_slot = alloca double
  store double 5.000000e-01, ptr %floating_slot
  %floating_memory_result = load double, ptr %floating_slot
  %memory_is_exact = fcmp oeq double %floating_memory_result, 5.000000e-01
  %fractional_result = fadd double 5.000000e-01, 0.000000e+00
  %floating_result = fadd double 2.1474836480000000e+09, 0.000000e+00
  %is_exact = fcmp oeq double %floating_result, 2.1474836480000000e+09
  %both_exact = and i1 %is_exact, %memory_is_exact
  %result = zext i1 %both_exact to i32
  ret i32 %result
}
