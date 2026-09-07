; Regression fixture for semi-sparse scalar propagation across a direct call.
; The callee's formal pointer must preserve the singleton field object passed
; by its only caller.

%Pair = type { i32, i32 }

define internal i32 @read_second(ptr %pointer_argument) {
entry:
  %pointer_argument_result = load i32, ptr %pointer_argument
  ret i32 %pointer_argument_result
}

define i32 @main() {
entry:
  %pair = alloca %Pair, align 4
  %first = getelementptr inbounds %Pair, ptr %pair, i64 0, i32 0
  %second = getelementptr inbounds %Pair, ptr %pair, i64 0, i32 1
  store i32 7, ptr %first
  store i32 11, ptr %second
  %result = call i32 @read_second(ptr %second)
  ret i32 %result
}
