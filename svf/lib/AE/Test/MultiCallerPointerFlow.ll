; Regression fixture for joining pointer arguments from multiple call sites.
; A context-insensitive callee must retain both field objects. Keeping only
; the most recently visited call site would make the scalar carrier unsound.

%Pair = type { i32, i32 }

define internal i32 @read_any(ptr %multi_pointer_argument) {
entry:
  %multi_pointer_argument_result = load i32, ptr %multi_pointer_argument
  ret i32 %multi_pointer_argument_result
}

define internal void @shared_callee() {
entry:
  ret void
}

define internal void @other_caller() {
entry:
  %other_slot = alloca ptr, align 8
  store ptr null, ptr %other_slot
  call void @shared_callee()
  ret void
}

define i32 @main() {
entry:
  %pair = alloca %Pair, align 4
  %first = getelementptr inbounds %Pair, ptr %pair, i64 0, i32 0
  %second = getelementptr inbounds %Pair, ptr %pair, i64 0, i32 1
  store i32 7, ptr %first
  store i32 11, ptr %second
  call void @other_caller()
  %caller_slot = alloca ptr, align 8
  store ptr %first, ptr %caller_slot
  call void @shared_callee()
  %caller_local_after_shared_callee = load ptr, ptr %caller_slot
  %caller_local_result = load i32, ptr %caller_local_after_shared_callee
  %first_result = call i32 @read_any(ptr %first)
  %second_result = call i32 @read_any(ptr %second)
  %partial_sum = add i32 %first_result, %second_result
  %sum = add i32 %partial_sum, %caller_local_result
  ret i32 %sum
}
