; Minimal regression fixture for analysis-time GepObjVar creation.  A global
; pointer initialized from an aggregate field must denote that field object,
; never the reserved null Location.

@bytes = global [4 x i8] c"abc\00"
@field = global ptr getelementptr inbounds ([4 x i8], ptr @bytes, i64 0, i64 1)
@pair = global { ptr, ptr } {
  ptr @bytes,
  ptr getelementptr inbounds ([4 x i8], ptr @bytes, i64 0, i64 2)
}

define i32 @main() {
entry:
  %from_scalar = load ptr, ptr @field
  %scalar_byte = load i8, ptr %from_scalar
  %pair_slot = getelementptr inbounds { ptr, ptr }, ptr @pair, i64 0, i32 1
  %from_aggregate = load ptr, ptr %pair_slot
  %aggregate_byte = load i8, ptr %from_aggregate
  %lhs = zext i8 %scalar_byte to i32
  %rhs = zext i8 %aggregate_byte to i32
  %sum = add i32 %lhs, %rhs
  ret i32 %sum
}
