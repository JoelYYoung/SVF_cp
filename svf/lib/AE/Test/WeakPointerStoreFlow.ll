; A two-object points-to set requires weak updates. The interpreter follows
; Original AE's policy that fresh pointer storage reads as empty.

define i32 @main(i32 %argc) {
entry:
  %left = alloca ptr
  %right = alloca ptr
  %left_target = alloca i8
  %right_target = alloca i8
  store ptr %left_target, ptr %left
  store ptr %right_target, ptr %right
  %condition = icmp ne i32 %argc, 0
  %selected = select i1 %condition, ptr %left, ptr %right
  store ptr null, ptr %selected
  %weak_left_result = load ptr, ptr %left
  %weak_right_result = load ptr, ptr %right
  %empty_cell = alloca ptr
  %empty_load_result = load ptr, ptr %empty_cell
  %empty_nonnull = icmp ne ptr %empty_load_result, null
  %untyped_cell = alloca i64
  store ptr %left, ptr %untyped_cell
  %type_punned_pointer_result = load ptr, ptr %untyped_cell
  ret i32 0
}
