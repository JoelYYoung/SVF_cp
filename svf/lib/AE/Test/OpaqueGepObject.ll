; A GEP object over an opaque external allocation has no recoverable flattened
; field type. Registering its memory-content variable must remain conservative
; instead of asking SVF for an out-of-layout field type.

%Opaque = type opaque

@opaque_object = external global %Opaque

define i32 @main() {
entry:
  %opaque_byte = getelementptr i8, ptr @opaque_object, i64 1
  store i8 7, ptr %opaque_byte
  ret i32 0
}
