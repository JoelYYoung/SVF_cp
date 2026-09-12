; A memory write of numeric Top is still an explicit sparse-flow fact.  The
; merge must not confuse its canonical missing Box slot with an edge that
; carries no fact and retain the other predecessor's singleton.

define i32 @loop_top(i32 %unknown) {
entry:
  %cell = alloca i32
  store i32 1, ptr %cell
  br label %loop

loop:
  %iteration = phi i32 [ 0, %entry ], [ %next, %latch ]
  %write = icmp eq i32 %iteration, 1
  br i1 %write, label %write_unknown, label %latch

write_unknown:
  store i32 %unknown, ptr %cell
  br label %latch

latch:
  %next = add i32 %iteration, 1
  %continue = icmp ult i32 %next, 2
  br i1 %continue, label %loop, label %exit

exit:
  %sparse_top_loop_result = load i32, ptr %cell
  ret i32 %sparse_top_loop_result
}

define i32 @main(i32 %condition_input, i32 %unknown) {
entry:
  %cell = alloca i32
  store i32 1, ptr %cell
  %condition = icmp eq i32 %condition_input, 0
  br i1 %condition, label %write_unknown, label %keep_one

write_unknown:
  store i32 %unknown, ptr %cell
  br label %merge

keep_one:
  br label %merge

merge:
  %sparse_top_merge_result = load i32, ptr %cell
  %top_loop_call = call i32 @loop_top(i32 %unknown)
  %result = add i32 %sparse_top_merge_result, %top_loop_call
  ret i32 %result
}
