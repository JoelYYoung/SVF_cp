; Original-compatible AE does not introduce integer-derived addresses into
; modeled pointer value-flow. The domain can represent raw addresses, but the
; default interpreter leaves these three results empty.

declare ptr @opaque_pointer()

define i32 @main(i64 %unknown_integer) {
entry:
  %unknown_pointer = inttoptr i64 %unknown_integer to ptr
  %invalid_pointer = inttoptr i64 1 to ptr
  %null_pointer = inttoptr i64 0 to ptr
  %external_pointer = call ptr @opaque_pointer()
  ret i32 0
}
