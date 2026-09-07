; Regression fixture for joining pointer arguments from multiple call sites.
; A context-insensitive callee must retain both field objects. Keeping only
; the most recently visited call site would make the scalar carrier unsound.

%Pair = type { i32, i32 }

define internal i32 @read_any(ptr %multi_pointer_argument) {
entry:
  %multi_pointer_argument_result = load i32, ptr %multi_pointer_argument
  ret i32 %multi_pointer_argument_result
}

define i32 @main() {
entry:
  %pair = alloca %Pair, align 4
  %first = getelementptr inbounds %Pair, ptr %pair, i64 0, i32 0
  %second = getelementptr inbounds %Pair, ptr %pair, i64 0, i32 1
  store i32 7, ptr %first
  store i32 11, ptr %second
  %first_result = call i32 @read_any(ptr %first)
  %second_result = call i32 @read_any(ptr %second)
  %sum = add i32 %first_result, %second_result
  ret i32 %sum
}
