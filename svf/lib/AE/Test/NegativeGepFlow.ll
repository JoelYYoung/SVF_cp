; A signed GEP offset must not be clamped to zero. Starting from values[3],
; stepping back by one element must load values[2].

@values = global [4 x i32] [i32 10, i32 20, i32 30, i32 40]

define i32 @main() {
entry:
  %end = getelementptr inbounds [4 x i32], ptr @values, i64 0, i64 3
  %previous = getelementptr inbounds i32, ptr %end, i64 -1
  %negative_gep_result = load i32, ptr %previous
  ret i32 %negative_gep_result
}
