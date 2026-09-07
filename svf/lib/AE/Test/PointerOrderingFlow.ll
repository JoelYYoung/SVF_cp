; Abstract Location IDs identify objects but do not encode their concrete
; address order. Ordering distinct singleton locations must remain unknown.

define i32 @main() {
entry:
  %left = alloca i32
  %right = alloca i32
  %pointer_ordering_result = icmp ult ptr %left, %right
  %same_pointer_ordering_result = icmp ule ptr %left, %left
  %both = and i1 %pointer_ordering_result, %same_pointer_ordering_result
  %result = zext i1 %both to i32
  ret i32 %result
}
