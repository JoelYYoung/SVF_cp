; Pointer comparison fixture. Abstract Location IDs do not encode concrete
; address order, equal non-singleton target sets do not prove concrete pointer
; equality, and a comparison result must make an impossible branch infeasible.

define i32 @main(i1 %choose_left, i1 %choose_right, i64 %raw_pointer) {
entry:
  %left = alloca i32
  %right = alloca i32
  %known_different = icmp ne ptr %left, %right
  br i1 %known_different, label %different, label %unexpected_equal

unexpected_equal:
  br label %branch_join

different:
  %known_same = icmp eq ptr %left, %left
  br i1 %known_same, label %expected, label %unexpected_not_same

unexpected_not_same:
  br label %branch_join

expected:
  br label %branch_join

branch_join:
  %pointer_branch_result = phi i32 [ 101, %unexpected_equal ], [ 202, %unexpected_not_same ], [ 7, %expected ]
  %selected_left = select i1 %choose_left, ptr %left, ptr %right
  %selected_right = select i1 %choose_right, ptr %left, ptr %right
  %overlapping_equality = icmp eq ptr %selected_left, %selected_right
  %null_equality = icmp eq ptr null, null
  %opaque_pointer = inttoptr i64 %raw_pointer to ptr
  %opaque_equality = icmp eq ptr %opaque_pointer, %left
  %pointer_ordering_result = icmp ult ptr %left, %right
  %same_pointer_ordering_result = icmp ule ptr %left, %left
  %both = and i1 %pointer_ordering_result, %same_pointer_ordering_result
  %result = zext i1 %both to i32
  ret i32 %result
}
