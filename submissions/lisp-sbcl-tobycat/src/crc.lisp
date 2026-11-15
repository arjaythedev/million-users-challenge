;;;; CRC-32 by carry-less multiplication (PCLMULQDQ), for CPUs that have it. SBCL has no such operation, but
;;;; its assembler knows the instruction, so this adds one compiler primitive: 128-bit folding, 16 bytes a step,
;;;; then the reduction to 32 bits, with the constants of Linux's crc32-pclmul (reflected polynomial #xEDB88320).

(in-package :mu)

;;; known to the compiler before the functions below are compiled
(eval-when (:compile-toplevel :load-toplevel :execute)
(sb-c:defknown %crc-fold (system-area-pointer (unsigned-byte 62) (unsigned-byte 32)) (unsigned-byte 32)
    (sb-c:flushable) :overwrite-fndb-silently t)

(sb-c:define-vop (%crc-fold)
  (:translate %crc-fold)
  (:policy :fast-safe)
  (:args (ptr :scs (sb-vm::sap-reg) :target p)
         (len :scs (sb-vm::unsigned-reg) :target n)
         (crc :scs (sb-vm::unsigned-reg)))
  (:arg-types system-area-pointer sb-vm::unsigned-num sb-vm::unsigned-num)
  (:temporary (:sc sb-vm::sap-reg :from (:argument 0)) p)
  (:temporary (:sc sb-vm::unsigned-reg :from (:argument 1)) n)
  (:temporary (:sc sb-vm::unsigned-reg) tmp)
  (:temporary (:sc sb-vm::int-sse-reg) x0 x1 x2 x3 x5)
  (:results (r :scs (sb-vm::unsigned-reg)))
  (:result-types sb-vm::unsigned-num)
  (:generator 50
    ;; LEN is 16 or more; whole 16-byte blocks are consumed and the running CRC is returned
    (sb-c:move p ptr)
    (sb-c:move n len)
    (sb-assem:inst movdqu x1 (sb-vm::ea 0 p))
    (sb-assem:inst movq x0 crc)
    (sb-assem:inst pxor x1 x0)
    (sb-assem:inst add p 16)
    (sb-assem:inst sub n 16)
    (sb-assem:inst mov tmp #x1751997d0) (sb-assem:inst movq x0 tmp)
    (sb-assem:inst mov tmp #x0ccaa009e) (sb-assem:inst movq x5 tmp)
    (sb-assem:inst punpcklqdq x0 x5)
    FOLD
    (sb-assem:inst cmp n 16)
    (sb-assem:inst jmp :b DONE)
    (sb-assem:inst movdqa x5 x1)
    (sb-assem:inst pclmulqdq x1 x0 #x00)
    (sb-assem:inst pclmulqdq x5 x0 #x11)
    (sb-assem:inst pxor x1 x5)
    (sb-assem:inst movdqu x5 (sb-vm::ea 0 p))
    (sb-assem:inst pxor x1 x5)
    (sb-assem:inst add p 16)
    (sb-assem:inst sub n 16)
    (sb-assem:inst jmp FOLD)
    DONE
    ;; 128 -> 64 bits, also appending 32 zero bits
    (sb-assem:inst pclmulqdq x0 x1 #x01)
    (sb-assem:inst psrldq x1 8)
    (sb-assem:inst pxor x1 x0)
    ;; 64 -> 32 bits
    (sb-assem:inst mov tmp #x163cd6124) (sb-assem:inst movq x0 tmp)
    (sb-assem:inst mov tmp #xffffffff) (sb-assem:inst movq x3 tmp)
    (sb-assem:inst movdqa x2 x1)
    (sb-assem:inst pand x1 x3)
    (sb-assem:inst psrldq x2 4)
    (sb-assem:inst pclmulqdq x1 x0 #x00)
    (sb-assem:inst pxor x1 x2)
    ;; bit-reflected Barrett reduction
    (sb-assem:inst mov tmp #x1db710641) (sb-assem:inst movq x0 tmp)
    (sb-assem:inst mov tmp #x1f7011641) (sb-assem:inst movq x5 tmp)
    (sb-assem:inst punpcklqdq x0 x5)
    (sb-assem:inst movdqa x2 x1)
    (sb-assem:inst pand x1 x3)
    (sb-assem:inst pclmulqdq x1 x0 #x10)
    (sb-assem:inst pand x1 x3)
    (sb-assem:inst pclmulqdq x1 x0 #x00)
    (sb-assem:inst pxor x1 x2)
    (sb-assem:inst psrldq x1 4)
    (sb-assem:inst movd r x1))))

(defun crc-fold (sap len crc)
  (declare (type system-area-pointer sap) (type (unsigned-byte 62) len) (type (unsigned-byte 32) crc))
  (%crc-fold sap len crc))

(defun cpu-has-pclmulqdq ()
  (ignore-errors
   (with-open-file (s "/proc/cpuinfo")
     (loop for line = (read-line s nil) while line
           thereis (and (> (length line) 5) (string= line "flags" :end1 5) (search " pclmulqdq" line))))))
