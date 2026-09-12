; Original-compatible AE ignores the integer-derived actual. The known caller
; must remain precise without manufacturing modeled-object value flow.

define i32 @read_pointer(ptr %unknown_pointer_argument) {
entry:
  %unknown_pointer_argument_result = load i32, ptr %unknown_pointer_argument
  ret i32 %unknown_pointer_argument_result
}

define i32 @main(i32 %argc) {
entry:
  %slot = alloca i32
  store i32 7, ptr %slot
  %known_result = call i32 @read_pointer(ptr %slot)
  %raw_address = zext i32 %argc to i64
  %unknown_address = inttoptr i64 %raw_address to ptr
  %unknown_result = call i32 @read_pointer(ptr %unknown_address)
  ret i32 %known_result
}
