;;;; gzip: greedy LZ77 over a 32 KB window, emitted as one DEFLATE block with the trained Huffman code of
;;;; gzip-tables.lisp, plus CRC-32.

(in-package :mu)

(deftype ub32-vec (n) `(simple-array (unsigned-byte 32) (,n)))

(declaim (type (ub32-vec 256) +crc-table+))
(sb-ext:define-load-time-global +crc-table+
  (let ((tab (make-array 256 :element-type '(unsigned-byte 32))))
    (dotimes (n 256 tab)
      (let ((c n))
        (dotimes (k 8) (setf c (if (logbitp 0 c) (logxor #xedb88320 (ash c -1)) (ash c -1))))
        (setf (aref tab n) c)))))

(defun crc32 (v start end)
  (declare (type octets v) (type fixnum start end) (optimize speed (safety 0)))
  (let ((c #xffffffff))
    (declare (type (unsigned-byte 32) c))
    (loop for i of-type fixnum from start below end
          do (setf c (logxor (aref +crc-table+ (logand (logxor c (aref v i)) #xff)) (ash c -8))))
    (logxor c #xffffffff)))

(defun reverse-bits (x n)
  (let ((r 0)) (dotimes (i n r) (setf r (logior (ash r 1) (ldb (byte 1 i) x))))))

(defparameter +len-base+ #(3 4 5 6 7 8 9 10 11 13 15 17 19 23 27 31 35 43 51 59 67 83 99 115 131 163 195 227 258))
(defparameter +len-extra+ #(0 0 0 0 0 0 0 0 1 1 1 1 2 2 2 2 3 3 3 3 4 4 4 4 5 5 5 5 0))
(defparameter +dist-base+ #(1 2 3 4 5 7 9 13 17 25 33 49 65 97 129 193 257 385 513 769 1025 1537 2049 3073
                            4097 6145 8193 12289 16385 24577))
(defparameter +dist-extra+ #(0 0 0 0 1 1 2 2 3 3 4 4 5 5 6 6 7 7 8 8 9 9 10 10 11 11 12 12 13 13))

(defconstant +hbits+ 13)
;;; 32-bit positions in 8192 slots (32 KB, so the table stays in L1/L2): a slot holds base + offset of the
;;; last position with that hash; anything below the current call's base is stale. The base restarts
;;; (and the table is cleared) before it could pass 2^31.
(declaim (type (simple-array (unsigned-byte 32) (8192)) *gz-head*))
(sb-ext:define-load-time-global *gz-head* (make-array 8192 :element-type '(unsigned-byte 32) :initial-element 0))
(declaim (type fixnum *gz-base*))
(sb-ext:define-load-time-global *gz-base* 1)

;;; slicing-by-8 CRC-32 tables
(declaim (type (ub32-vec 2048) +crc8+))
(sb-ext:define-load-time-global +crc8+
  (let ((tab (make-array 2048 :element-type '(unsigned-byte 32))))
    (dotimes (n 256) (setf (aref tab n) (aref +crc-table+ n)))
    (loop for k from 1 below 8
          do (dotimes (n 256)
               (let ((prev (aref tab (+ (* 256 (1- k)) n))))
                 (setf (aref tab (+ (* 256 k) n)) (logxor (ash prev -8) (aref +crc-table+ (logand prev #xff)))))))
    tab))

(sb-ext:define-load-time-global *crc-clmul* nil)   ; set at startup

(defun crc32-sap (sap start end)
  (declare (type system-area-pointer sap) (type fixnum start end) (optimize speed (safety 0)))
  (let ((c #xffffffff) (i start) (tab +crc8+))
    (declare (type (unsigned-byte 32) c) (type fixnum i))
    ;; whole 16-byte blocks by carry-less multiplication where the CPU has it (crc.lisp); the table does the rest
    (when (and *crc-clmul* (>= (- end start) 64))
      (let ((whole (logand (- end start) -16)))
        (declare (type fixnum whole))
        (setf c (crc-fold (sap+ sap start) whole c))
        (incf i whole)))
    (loop while (<= (+ i 8) end)
          do (let ((lo (logxor c (sap-ref-32 sap i))) (hi (sap-ref-32 sap (+ i 4))))
               (declare (type (unsigned-byte 32) lo hi))
               (setf c (logxor (aref tab (+ 1792 (logand lo #xff)))
                               (aref tab (+ 1536 (logand (ash lo -8) #xff)))
                               (aref tab (+ 1280 (logand (ash lo -16) #xff)))
                               (aref tab (+ 1024 (ash lo -24)))
                               (aref tab (+ 768 (logand hi #xff)))
                               (aref tab (+ 512 (logand (ash hi -8) #xff)))
                               (aref tab (+ 256 (logand (ash hi -16) #xff)))
                               (aref tab (ash hi -24))))
               (incf i 8)))
    (loop while (< i end)
          do (setf c (logxor (aref tab (logand (logxor c (sap-ref-8 sap i)) #xff)) (ash c -8)))
             (incf i))
    (logxor c #xffffffff)))

;;; one table for the compressor's loop, so it holds one pointer: literal codes at 0, match-length codes (with their
;;; extra bits) at 512 + length, distance codes at 800 + symbol (all as code | bits << 24), and at 832 + symbol the
;;; distance symbol's base | extra-bit count << 16
(declaim (type (simple-array (unsigned-byte 32) (864)) +gz-tab+))
(sb-ext:define-load-time-global +gz-tab+
  (let ((v (make-array 864 :element-type '(unsigned-byte 32) :initial-element 0)))
    (dotimes (i 257) (setf (aref v i) (logior (aref +gz-lit-code+ i) (ash (aref +gz-lit-len+ i) 24))))
    (loop for l from 3 to 258 do (setf (aref v (+ 512 l)) (logior (aref +gz-len-code+ l) (ash (aref +gz-len-len+ l) 24))))
    (dotimes (k 30 v)
      (setf (aref v (+ 800 k)) (logior (aref +gz-dist-code+ k) (ash (aref +gz-dist-len+ k) 24))
            (aref v (+ 832 k)) (logior (aref +dist-base+ k) (ash (aref +dist-extra+ k) 16))))))

(defmacro gz-hash (sap i)
  `(ash (ldb (byte 32 0) (* (sap-ref-32 ,sap ,i) 2654435761)) ,(- +hbits+ 32)))

(defun gzip-into (g v start end)
  "Appends the gzip of V[start,end) to obuf G."
  (declare (type obuf g) (type octets v) (type fixnum start end) (optimize speed (safety 0)))
  (let ((n (- end start)))
    (ob-room g (+ 512 (* 2 n)))            ; the longest code is 15 bits a byte, plus the headers
    (let* ((out (obuf-data g)) (o (obuf-len g)) (bh +gz-block-hdr+)
           (base *gz-base*))
      (declare (type octets out bh) (type fixnum o base))
      (when (> (+ base n 1) #x7fffffff)
        (fill *gz-head* 0) (setf base 1))
      (setf *gz-base* (+ base n 1))
      (replace out #(#x1f #x8b 8 0 0 0 0 0 0 3) :start1 o)
      (incf o 10)
      (replace out bh :start1 o)
      (incf o (length bh))
      (with-pinned-objects (v out)
        (let* ((src (vector-sap v)) (dst (vector-sap out))
               (acc +gz-hdr-tail+) (nbits +gz-hdr-tail-bits+)
               (head *gz-head*) (tab +gz-tab+)
               (gb (- base start))         ; a slot holds gb + position; below base it is stale
               (i start))
          (declare (type (unsigned-byte 64) acc) (type (integer 0 63) nbits) (type fixnum gb i))
          (macrolet ((put (bits count)
                       `(progn
                          ;; nbits < 32 here and a code with its extra bits is at most 20, so nothing is lost off the top
                          (setf acc (logior acc (ldb (byte 64 0) (ash (the (unsigned-byte 32) ,bits) nbits))))
                          (setf nbits (the (integer 0 63) (+ nbits (the (integer 0 31) ,count))))
                          (when (>= nbits 32)
                            (setf (sap-ref-32 dst o) (ldb (byte 32 0) acc))
                            (incf o 4)
                            (setf acc (ash acc -32))
                            (decf nbits 32))))
                     (putp (x) `(let ((x ,x)) (declare (type (unsigned-byte 32) x)) (put (logand x #xffffff) (ash x -24)))))
            (loop while (<= i (- end 4))    ; positions with 4 readable bytes can be hashed
                  do (let* ((w (sap-ref-32 src i))
                            (h (ash (ldb (byte 32 0) (* w 2654435761)) (- +hbits+ 32)))
                            (cand (- (aref head h) gb)))
                       (declare (type fixnum cand) (type (unsigned-byte 32) w))
                       (setf (aref head h) (+ gb i))
                       (if (and (>= cand start) (<= (- i cand) 32768) (= (sap-ref-32 src cand) w))
                           (let ((lim (min 258 (- end i))) (k 4) (dist (- i cand)))
                             (declare (type fixnum lim k dist))
                             (loop
                               (if (<= (+ k 8) lim)
                                   (let ((x (logxor (sap-ref-64 src (+ cand k)) (sap-ref-64 src (+ i k)))))
                                     (declare (type (unsigned-byte 64) x))
                                     (if (zerop x)
                                         (incf k 8)
                                         (progn (incf k (ash (1- (integer-length (logxor x (1- x)))) -3))
                                                (return))))
                                   (progn
                                     (loop while (and (< k lim) (= (sap-ref-8 src (+ cand k)) (sap-ref-8 src (+ i k))))
                                           do (incf k))
                                     (return))))
                             (putp (aref tab (+ 512 k)))
                             (let* ((ds (if (< dist 5)
                                            (1- dist)
                                            (let* ((x (1- dist)) (nb (integer-length x)))
                                              (declare (type (integer 4 32767) x) (type (integer 3 15) nb))
                                              (+ (* 2 (1- nb)) (ldb (byte 1 0) (ash x (- 2 nb)))))))
                                    (be (aref tab (+ 832 ds))))
                               (declare (type (integer 0 29) ds) (type (unsigned-byte 32) be))
                               (putp (aref tab (+ 800 ds)))
                               (put (- dist (logand be #xffff)) (ash be -16)))
                             (incf i k))
                           (progn (putp (aref tab (logand w #xff)))
                                  (incf i)))))
            (loop while (< i end)
                  do (putp (aref tab (sap-ref-8 src i))) (incf i))
            (putp (aref tab 256))
            (loop while (plusp nbits)
                  do (setf (sap-ref-8 dst o) (ldb (byte 8 0) acc) acc (ash acc -8)) (incf o) (setf nbits (max 0 (- nbits 8)))))
          (let ((crc (crc32-sap src start end)))
            (setf (sap-ref-32 dst o) crc (sap-ref-32 dst (+ o 4)) (ldb (byte 32 0) n))
            (incf o 8))))
      (setf (obuf-len g) o))))
