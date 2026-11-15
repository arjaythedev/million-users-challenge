;;;; The compressor's token loop as one compiler primitive, for CPUs with BMI2. gzip.lisp has the same loop in
;;;; plain Lisp (GZIP-INTO), and this gives the same bytes: SBCL's code for that one keeps tagged integers,
;;;; spills to the stack and shifts through CL, and takes almost twice as long. Like crc.lisp, this is written
;;;; with SBCL's own assembler.
;;;;
;;;; Everything the loop needs is in one block of memory (BLK), so it holds one pointer:
;;;;   0 end of input, 8 end - 4, 16 start of input, 24 bias (a hash slot holds position - bias),
;;;;   32 bits waiting, 40 how many, 64 the code table of gzip.lisp (+gz-tab+), then the hash slots.

(in-package :mu)

(defconstant +blk-tab+ 64)
(defconstant +blk-head+ (+ 64 (* 4 864)))
(defconstant +blk-size+ (+ +blk-head+ (* 4 8192)))

(eval-when (:compile-toplevel :load-toplevel :execute)
  (defmacro i (&rest x) `(sb-assem:inst ,@x))
  (defmacro m (disp base &optional index (scale 1)) `(sb-vm::ea ,disp ,base ,index ,scale))
  ;; SHLX (pp 1) and SHRX (pp 3), dst = src shifted by cnt, without touching the flags (BMI2). SBCL 2.2.9's
  ;; assembler does not know them, so their five bytes are written out: VEX.LZ.66/F2.0F38.W1 F7 /r
  (defmacro shx (pp dst src cnt)
    `(let ((d (sb-c:tn-offset ,dst)) (s (sb-c:tn-offset ,src)) (c (sb-c:tn-offset ,cnt)))
       (i byte #xC4)
       (i byte (logior (ash (logxor 1 (ash d -3)) 7) #x40 (ash (logxor 1 (ash s -3)) 5) 2))
       (i byte (logior #x80 (ash (logxor 15 c) 3) ,pp))
       (i byte #xF7)
       (i byte (logior #xC0 (ash (logand d 7) 3) (logand s 7)))))
  ;; append bits: the value in A (upper bits clear), the bit count in D. At most 7 bits wait in ACC, so 8 bytes
  ;; are stored every time and DST moves past the whole bytes among them: no branch
  (defmacro put ()
    `(progn (shx 1 a a nb) (i or acc a) (i add nb d)
            (i mov (m 0 dst) acc)
            (i mov d nb) (i shr d 3) (i add dst d) (i and nb 7)
            (i shl d 3) (shx 3 acc acc d)))
  ;; a packed table entry in A: code | bits << 24
  (defmacro putp ()
    `(progn (i mov :dword d a) (i shr :dword d 24) (i and :dword a #xffffff) (put))))

(eval-when (:compile-toplevel :load-toplevel :execute)
(sb-c:defknown %gz-tokens (system-area-pointer system-area-pointer system-area-pointer) system-area-pointer
    () :overwrite-fndb-silently t)

(sb-c:define-vop (%gz-tokens)
  (:translate %gz-tokens)
  (:policy :fast-safe)
  (:args (blk-arg :scs (sb-vm::sap-reg) :target blk)
         (src-arg :scs (sb-vm::sap-reg) :target pos)
         (dst-arg :scs (sb-vm::sap-reg) :target dst))
  (:arg-types system-area-pointer system-area-pointer system-area-pointer)
  (:temporary (:sc sb-vm::sap-reg :from (:argument 0)) blk)
  (:temporary (:sc sb-vm::sap-reg :from (:argument 1)) pos)
  (:temporary (:sc sb-vm::sap-reg :from (:argument 2)) dst)
  (:temporary (:sc sb-vm::unsigned-reg :offset sb-vm::rcx-offset) c)
  (:temporary (:sc sb-vm::unsigned-reg) a d s tt u acc nb)
  (:results (r :scs (sb-vm::sap-reg)))
  (:result-types system-area-pointer)
  (:generator 200
      (sb-c:move blk blk-arg)
      (sb-c:move pos src-arg)
      (sb-c:move dst dst-arg)
      (i mov acc (m 32 blk))
      (i mov nb (m 40 blk))
      SCAN
      (i cmp pos (m 8 blk))
      (i jmp :a TAIL)
      (i mov :dword a (m 0 pos))
      (i mov :dword d a)                       ; the 4 bytes here
      (i mov :dword c #x9E3779B1)
      (i imul a c)
      (i shr :dword a 19)
      (i mov :dword s (m +blk-head+ blk a 4))  ; the last position with this hash
      (i mov c pos)
      (i sub c (m 24 blk))
      (i mov :dword (m +blk-head+ blk a 4) c)
      (i add s (m 24 blk))                     ; candidate
      (i cmp s (m 16 blk))
      (i jmp :b LITERAL)                       ; empty, or from an earlier body
      (i mov tt pos)
      (i sub tt s)                             ; distance
      (i cmp tt 32768)
      (i jmp :a LITERAL)
      (i cmp :dword d (m 0 s))
      (i jmp :ne LITERAL)
      ;; how long: D = length so far, C = its limit
      (i mov d 4)
      (i mov c (m 0 blk))
      (i sub c pos)
      (i cmp c 258)
      (i jmp :be WORDS)
      (i mov c 258)
      WORDS
      (i lea a (m 8 d))
      (i cmp a c)
      (i jmp :a BYTES)
      (i mov a (m 0 pos d))
      (i xor a (m 0 s d))
      (i jmp :nz DIFFER)
      (i add d 8)
      (i jmp WORDS)
      DIFFER
      (i bsf a a)
      (i shr a 3)
      (i add d a)
      (i jmp MATCHED)
      BYTES
      (i cmp d c)
      (i jmp :ae MATCHED)
      (i mov :byte a (m 0 pos d))
      (i cmp :byte a (m 0 s d))
      (i jmp :ne MATCHED)
      (i inc d)
      (i jmp BYTES)
      MATCHED
      (i add pos d)
      (i mov :dword u (m (+ +blk-tab+ (* 4 512)) blk d 4))   ; the length's code with its extra bits
      ;; the distance's symbol in S: distance - 1 below 5, else 2b + the bit under the top one (b = top bit's index)
      (i lea s (m -1 tt))
      (i cmp tt 5)
      (i jmp :b SYMBOL)
      (i bsr c s)
      (i mov a s)
      (i dec c)
      (i shr a :cl)
      (i and a 1)
      (i lea s (m 2 a c 2))
      SYMBOL
      (i mov :dword d (m (+ +blk-tab+ (* 4 800)) blk s 4))   ; the distance's code | bits << 24
      (i mov :dword a (m (+ +blk-tab+ (* 4 832)) blk s 4))   ; its base | extra-bit count << 16
      (i mov :dword s a)
      (i shr :dword s 16)
      (i and :dword a #xffff)
      (i sub tt a)                             ; the extra bits' value
      ;; length code, distance code and extra bits go out as one put: 20 + 24 bits at most, on top of 7 waiting
      (i mov :dword c d)
      (i shr :dword c 24)
      (i and :dword d #xffffff)
      (i shl tt :cl)
      (i or tt d)
      (i add :dword s c)
      (i mov :dword c u)
      (i shr :dword c 24)
      (i and :dword u #xffffff)
      (i shl tt :cl)
      (i or tt u)
      (i add :dword s c)
      (i mov a tt)
      (i mov :dword d s)
      (put)
      (i jmp SCAN)
      LITERAL
      (i mov :dword a d)
      (i and :dword a #xff)
      (i inc pos)
      (i mov :dword a (m +blk-tab+ blk a 4))
      (putp)
      (i jmp SCAN)
      TAIL
      (i cmp pos (m 0 blk))
      (i jmp :ae FINISH)
      (i movzx '(:byte :dword) a (m 0 pos))
      (i inc pos)
      (i mov :dword a (m +blk-tab+ blk a 4))
      (putp)
      (i jmp TAIL)
      FINISH
      (i mov :dword a (m (+ +blk-tab+ (* 4 256)) blk))
      (putp)
      (i test nb nb)
      (i jmp :z DONE)
      (i mov :byte (m 0 dst) acc)
      (i inc dst)
      DONE
      (sb-c:move r dst))))

(defun gz-tokens (blk src dst)
  (declare (type system-area-pointer blk src dst))
  (%gz-tokens blk src dst))

;;; the block: parameters, the code table, the hash slots
(declaim (type octets *gz-blk*))
(sb-ext:define-load-time-global *gz-blk*
  (let ((b (make-octets +blk-size+)))
    (dotimes (k 864 b)
      (let ((x (aref +gz-tab+ k)) (o (+ +blk-tab+ (* 4 k))))
        (setf (aref b o) (ldb (byte 8 0) x) (aref b (+ o 1)) (ldb (byte 8 8) x)
              (aref b (+ o 2)) (ldb (byte 8 16) x) (aref b (+ o 3)) (ldb (byte 8 24) x))))))
(declaim (type fixnum *gz-blk-base*))
(sb-ext:define-load-time-global *gz-blk-base* 1)

(defun gzip-into-native (g v start end)
  "GZIP-INTO with the token loop of %GZ-TOKENS. Same output."
  (declare (type obuf g) (type octets v) (type fixnum start end) (optimize speed (safety 0)))
  (let ((n (- end start)))
    (ob-room g (+ 512 (* 2 n)))
    (let* ((out (obuf-data g)) (o (obuf-len g)) (bh +gz-block-hdr+) (blk *gz-blk*)
           (base *gz-blk-base*))
      (declare (type octets out bh blk) (type fixnum o base))
      (when (> (+ base n 1) #x7fffffff)
        (fill blk 0 :start +blk-head+) (setf base 1))
      (setf *gz-blk-base* (+ base n 1))
      (replace out #(#x1f #x8b 8 0 0 0 0 0 0 3) :start1 o)
      (incf o 10)
      (replace out bh :start1 o)
      (incf o (length bh))
      (with-pinned-objects (v out blk)
        (let* ((src (sap+ (vector-sap v) start)) (dst (sap+ (vector-sap out) o)) (b (vector-sap blk))
               (a0 (sap-int src)))
          (declare (type (unsigned-byte 64) a0))
          (setf (sap-ref-64 b 0) (+ a0 n)
                (signed-sap-ref-64 b 8) (- (+ a0 n) 4)
                (sap-ref-64 b 16) a0
                (sap-ref-64 b 24) (- a0 base)
                (sap-ref-64 b 32) +gz-hdr-tail+
                (sap-ref-64 b 40) +gz-hdr-tail-bits+)
          (let* ((e (gz-tokens b src dst))
                 (oe (+ o (the fixnum (sap- e dst))))
                 (crc (crc32-sap src 0 n)))
            (declare (type fixnum oe))
            (setf (sap-ref-32 (vector-sap out) oe) crc
                  (sap-ref-32 (vector-sap out) (+ oe 4)) (ldb (byte 32 0) n))
            (setf o (+ oe 8)))))
      (setf (obuf-len g) o))))

(sb-ext:define-load-time-global *gz-native* nil)   ; set at startup
