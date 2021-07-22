;; NOTE: Assertions have been generated by update_lit_checks.py and should not be edited.
;; RUN: wasm-opt %s --rse --enable-gc-nn-locals -all -S -o - | filecheck %s

(module
 ;; CHECK:      (func $10
 ;; CHECK-NEXT:  (local $1 (ref func))
 ;; CHECK-NEXT:  (nop)
 ;; CHECK-NEXT: )
 (func $10
  ;; A non-nullable local. The pass should ignore it (as we cannot optimize
  ;; anything here anyhow: the code must assign to the local before reading from
  ;; it, so no sets can be redundant in that sense).
  (local $1 (ref func))
 )
)