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

;;; per distance 1..32768: its distance symbol; per symbol, base and extra bits count
(declaim (type (simple-array (unsigned-byte 8) (32769)) +dcode+))
(sb-ext:define-load-time-global +dcode+ (make-array 32769 :element-type '(unsigned-byte 8)))
(loop for d from 1 to 32768
      do (setf (aref +dcode+ d) (position-if (lambda (b) (<= b d)) +dist-base+ :from-end t)))
(declaim (type (ub32-vec 30) +dbase+ +dextra+))
(sb-ext:define-load-time-global +dbase+ (make-array 30 :element-type '(unsigned-byte 32) :initial-contents (coerce +dist-base+ 'list)))
(sb-ext:define-load-time-global +dextra+ (make-array 30 :element-type '(unsigned-byte 32) :initial-contents (coerce +dist-extra+ 'list)))

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

(defun crc32-sap (sap start end)
  (declare (type system-area-pointer sap) (type fixnum start end) (optimize speed (safety 0)))
  (let ((c #xffffffff) (i start) (tab +crc8+))
    (declare (type (unsigned-byte 32) c) (type fixnum i))
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

(defmacro gz-hash (sap i)
  `(ash (ldb (byte 32 0) (* (logand (sap-ref-32 ,sap ,i) #xffffff) 2654435761)) ,(- +hbits+ 32)))

(defun gzip-into (g v start end)
  "Appends the gzip of V[start,end) to obuf G."
  (declare (type obuf g) (type octets v) (type fixnum start end) (optimize speed (safety 0)))
  (let ((n (- end start)))
    (ob-room g (+ 512 (* 2 n)))            ; the longest code is 15 bits a byte, plus the headers
    (let* ((out (obuf-data g)) (o (obuf-len g))
           (acc 0) (nbits 0)
           (head *gz-head*) (base *gz-base*)
           (hend (- end 4))                ; positions with 4 readable bytes can be hashed
           (litc +gz-lit-code+) (litl +gz-lit-len+) (lenc +gz-len-code+) (lenn +gz-len-len+)
           (dc +gz-dist-code+) (dl +gz-dist-len+)
           (dcode +dcode+) (dbase +dbase+) (dextra +dextra+) (bh +gz-block-hdr+))
      (declare (type octets out bh) (type fixnum o base hend) (type (integer 0 63) nbits) (type (unsigned-byte 64) acc)
               (type (simple-array (unsigned-byte 32) (*)) litc litl lenc lenn dc dl))
      (when (> (+ base n 1) #x7fffffff)
        (fill head 0) (setf base 1))
      (setf *gz-base* (+ base n 1))
      (replace out #(#x1f #x8b 8 0 0 0 0 0 0 3) :start1 o)
      (incf o 10)
      (replace out bh :start1 o)
      (incf o (length bh))
      (setf acc +gz-hdr-tail+ nbits +gz-hdr-tail-bits+)
      (with-pinned-objects (v out)
        (let ((src (vector-sap v)) (dst (vector-sap out)))
          (macrolet ((put (bits count)
                       `(progn
                          ;; nbits < 32 here and a code with its extra bits is at most 20, so nothing is lost off the top
                          (setf acc (logior acc (ldb (byte 64 0) (ash (the (unsigned-byte 32) ,bits) nbits))))
                          (setf nbits (the (integer 0 63) (+ nbits (the (integer 0 31) ,count))))
                          (when (>= nbits 32)
                            (setf (sap-ref-32 dst o) (ldb (byte 32 0) acc))
                            (incf o 4)
                            (setf acc (ash acc -32))
                            (decf nbits 32)))))
            (let ((i start))
              (declare (type fixnum i))
              (loop while (< i end)
                    do (let ((best 0) (dist 0))
                         (declare (type fixnum best dist))
                         (when (<= i hend)
                           (let* ((h (gz-hash src i))
                                  (stored (aref head h))
                                  (cand (+ start (- stored base))))
                             (declare (type fixnum stored cand))
                             (setf (aref head h) (+ base (- i start)))
                             (when (and (>= stored base) (<= (- i cand) 32768)
                                        (= (logand (sap-ref-32 src cand) #xffffff) (logand (sap-ref-32 src i) #xffffff)))
                               (let ((lim (min 258 (- end i))) (k 3))
                                 (declare (type fixnum lim k))
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
                                 (setf best k dist (- i cand))))))
                         (if (>= best 3)
                             (let ((ds (aref dcode dist)))
                               (put (aref lenc best) (aref lenn best))
                               (put (aref dc ds) (aref dl ds))
                               (put (- dist (aref dbase ds)) (aref dextra ds))
                               ;; index the positions inside very short matches only (3-4 bytes): longer ones are skipped, as zlib's fast levels do
                               (when (<= best 4)
                                 (loop for j of-type fixnum from (1+ i) below (min (+ i best) (1+ hend))
                                       do (setf (aref head (gz-hash src j)) (+ base (- j start)))))
                               (incf i best))
                             (let ((b (sap-ref-8 src i)))
                               (put (aref litc b) (aref litl b))
                               (incf i))))))
            (put (aref litc 256) (aref litl 256))
            (loop while (plusp nbits)
                  do (setf (sap-ref-8 dst o) (ldb (byte 8 0) acc) acc (ash acc -8)) (incf o) (setf nbits (max 0 (- nbits 8)))))
          (let ((crc (crc32-sap src start end)))
            (setf (sap-ref-32 dst o) crc (sap-ref-32 dst (+ o 4)) (ldb (byte 32 0) n))
            (incf o 8))))
      (setf (obuf-len g) o))))
