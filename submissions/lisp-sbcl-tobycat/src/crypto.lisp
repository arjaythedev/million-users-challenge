;;;; SHA-256, HMAC-SHA256, PBKDF2 and base64 / base64url.

(in-package :mu)

(declaim (type (simple-array u32 (64)) +k256+))
(sb-ext:define-load-time-global +k256+
  (make-array 64 :element-type 'u32 :initial-contents
   '(#x428a2f98 #x71374491 #xb5c0fbcf #xe9b5dba5 #x3956c25b #x59f111f1 #x923f82a4 #xab1c5ed5
     #xd807aa98 #x12835b01 #x243185be #x550c7dc3 #x72be5d74 #x80deb1fe #x9bdc06a7 #xc19bf174
     #xe49b69c1 #xefbe4786 #x0fc19dc6 #x240ca1cc #x2de92c6f #x4a7484aa #x5cb0a9dc #x76f988da
     #x983e5152 #xa831c66d #xb00327c8 #xbf597fc7 #xc6e00bf3 #xd5a79147 #x06ca6351 #x14292967
     #x27b70a85 #x2e1b2138 #x4d2c6dfc #x53380d13 #x650a7354 #x766a0abb #x81c2c92e #x92722c85
     #xa2bfe8a1 #xa81a664b #xc24b8b70 #xc76c51a3 #xd192e819 #xd6990624 #xf40e3585 #x106aa070
     #x19a4c116 #x1e376c08 #x2748774c #x34b0bcb5 #x391c0cb3 #x4ed8aa4a #x5b9cca4f #x682e6ff3
     #x748f82ee #x78a5636f #x84c87814 #x8cc70208 #x90befffa #xa4506ceb #xbef9a3f7 #xc67178f2)))

(defstruct (sha (:constructor make-sha ()))
  (h (make-array 8 :element-type 'u32) :type (simple-array u32 (8)))
  (w (make-array 64 :element-type 'u32) :type (simple-array u32 (64)))
  (buf (make-octets 64) :type octets)
  (n 0 :type fixnum)
  (total 0 :type fixnum))

(defmacro u32+ (&rest xs) `(ldb (byte 32 0) (+ ,@xs)))
(defmacro rotr (x n) `(logior (ash ,x ,(- n)) (ldb (byte 32 0) (ash ,x ,(- 32 n)))))

(defun sha-block (h w v off)
  (declare (type (simple-array u32 (8)) h) (type (simple-array u32 (64)) w)
           (type octets v) (type fixnum off) (optimize speed (safety 0)))
  (dotimes (i 16)
    (let ((j (+ off (* 4 i))))
      (setf (aref w i) (logior (ash (aref v j) 24) (ash (aref v (+ j 1)) 16)
                               (ash (aref v (+ j 2)) 8) (aref v (+ j 3))))))
  (loop for i of-type fixnum from 16 below 64
        do (let* ((w15 (aref w (- i 15))) (w2 (aref w (- i 2)))
                  (s0 (logxor (rotr w15 7) (rotr w15 18) (ash w15 -3)))
                  (s1 (logxor (rotr w2 17) (rotr w2 19) (ash w2 -10))))
             (declare (type u32 w15 w2 s0 s1))
             (setf (aref w i) (u32+ (aref w (- i 16)) s0 (aref w (- i 7)) s1))))
  (let ((a (aref h 0)) (b (aref h 1)) (c (aref h 2)) (d (aref h 3))
        (e (aref h 4)) (f (aref h 5)) (g (aref h 6)) (hh (aref h 7)))
    (declare (type u32 a b c d e f g hh))
    (dotimes (i 64)
      (let* ((s1 (logxor (rotr e 6) (rotr e 11) (rotr e 25)))
             (ch (logxor (logand e f) (logand (logxor e #xffffffff) g)))
             (t1 (u32+ hh s1 ch (aref +k256+ i) (aref w i)))
             (s0 (logxor (rotr a 2) (rotr a 13) (rotr a 22)))
             (maj (logxor (logand a b) (logand a c) (logand b c)))
             (t2 (u32+ s0 maj)))
        (declare (type u32 s1 ch t1 s0 maj t2))
        (setf hh g g f f e e (u32+ d t1) d c c b b a a (u32+ t1 t2))))
    (setf (aref h 0) (u32+ (aref h 0) a) (aref h 1) (u32+ (aref h 1) b)
          (aref h 2) (u32+ (aref h 2) c) (aref h 3) (u32+ (aref h 3) d)
          (aref h 4) (u32+ (aref h 4) e) (aref h 5) (u32+ (aref h 5) f)
          (aref h 6) (u32+ (aref h 6) g) (aref h 7) (u32+ (aref h 7) hh)))
  nil)

(defun sha-reset (s)
  (replace (sha-h s) '(#x6a09e667 #xbb67ae85 #x3c6ef372 #xa54ff53a #x510e527f #x9b05688c #x1f83d9ab #x5be0cd19))
  (setf (sha-n s) 0 (sha-total s) 0)
  s)

(defun sha-update (s v &optional (start 0) (end (length v)))
  (declare (type sha s) (type octets v) (type fixnum start end) (optimize speed))
  (let ((buf (sha-buf s)) (i start))
    (declare (type fixnum i))
    (incf (sha-total s) (- end start))
    (when (plusp (sha-n s))
      (let ((take (min (- 64 (sha-n s)) (- end i))))
        (replace buf v :start1 (sha-n s) :start2 i :end2 (+ i take))
        (incf (sha-n s) take) (incf i take)
        (when (= (sha-n s) 64) (sha-block (sha-h s) (sha-w s) buf 0) (setf (sha-n s) 0))))
    (loop while (<= (+ i 64) end)
          do (sha-block (sha-h s) (sha-w s) v i) (incf i 64))
    (when (< i end)
      (replace buf v :start2 i :end2 end)
      (setf (sha-n s) (- end i)))
    s))

(defun sha-final (s &optional (out (make-octets 32)))
  (let* ((bits (* 8 (sha-total s)))
         (pad (make-octets (+ 1 (mod (- 55 (sha-n s)) 64) 8))))
    (setf (aref pad 0) #x80)
    (dotimes (i 8) (setf (aref pad (- (length pad) 1 i)) (ldb (byte 8 (* 8 i)) bits)))
    (sha-update s pad)
    (dotimes (i 8)
      (let ((x (aref (sha-h s) i)))
        (dotimes (j 4) (setf (aref out (+ (* 4 i) j)) (ldb (byte 8 (- 24 (* 8 j))) x)))))
    out))

(defun sha256 (v) (sha-final (sha-update (sha-reset (make-sha)) v)))

;;; HMAC with the key's padded blocks prepared once per key (a key schedule, not a cache of results).

(defstruct (hmac-key (:constructor %make-hmac-key (ipad opad)))
  (ipad nil :type octets) (opad nil :type octets))

(defun make-hmac-key (key)
  (let ((k (make-octets 64)))
    (replace k (if (> (length key) 64) (sha256 key) key))
    (%make-hmac-key (map 'octets (lambda (x) (logxor x #x36)) k)
                    (map 'octets (lambda (x) (logxor x #x5c)) k))))

(sb-ext:define-load-time-global *hsha* (make-sha))
(sb-ext:define-load-time-global *hinner* (make-octets 32))

(defun hmac (hk v &optional (start 0) (end (length v)) (out (make-octets 32)))
  (let ((s *hsha*))
    (sha-reset s) (sha-update s (hmac-key-ipad hk)) (sha-update s v start end)
    (sha-final s *hinner*)
    (sha-reset s) (sha-update s (hmac-key-opad hk)) (sha-update s *hinner*)
    (sha-final s out)))

(defun pbkdf2-sha256 (password salt iterations)
  (let* ((hk (make-hmac-key password))
         (u (hmac hk (concatenate 'octets salt #(0 0 0 1))))
         (acc (copy-seq u)))
    (loop repeat (1- iterations)
          do (setf u (hmac hk u))
             (dotimes (i 32) (setf (aref acc i) (logxor (aref acc i) (aref u i)))))
    acc))

;;; ---------------------------------------------------------------- base64

(defparameter +b64+ "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/")
(defparameter +b64url+ "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789-_")

(defun b64-encode (v &key (alphabet +b64+) (pad t))
  (with-output-to-string (o)
    (loop for i from 0 below (length v) by 3
          do (let* ((n (- (length v) i))
                    (x (logior (ash (aref v i) 16)
                               (if (> n 1) (ash (aref v (+ i 1)) 8) 0)
                               (if (> n 2) (aref v (+ i 2)) 0))))
               (write-char (char alphabet (ldb (byte 6 18) x)) o)
               (write-char (char alphabet (ldb (byte 6 12) x)) o)
               (if (> n 1) (write-char (char alphabet (ldb (byte 6 6) x)) o) (when pad (write-char #\= o)))
               (if (> n 2) (write-char (char alphabet (ldb (byte 6 0) x)) o) (when pad (write-char #\= o)))))))

(declaim (type (simple-array (signed-byte 8) (256)) +b64-dec+ +b64url-dec+))
(sb-ext:define-load-time-global +b64-dec+ (make-array 256 :element-type '(signed-byte 8) :initial-element -1))
(sb-ext:define-load-time-global +b64url-dec+ (make-array 256 :element-type '(signed-byte 8) :initial-element -1))
(dotimes (i 64)
  (setf (aref +b64-dec+ (char-code (char +b64+ i))) i
        (aref +b64url-dec+ (char-code (char +b64url+ i))) i))

(defun b64-decode (v start end &optional (table +b64-dec+))
  "Decodes octets V[start,end) (padding optional). NIL if invalid."
  (declare (type octets v) (type fixnum start end) (type (simple-array (signed-byte 8) (256)) table))
  (loop while (and (> end start) (= (aref v (1- end)) 61)) do (decf end))
  (let* ((n (- end start)))
    (when (= (mod n 4) 1) (return-from b64-decode nil))
    (let ((out (make-octets (floor (* n 3) 4))) (acc 0) (bits 0) (o 0))
      (declare (type fixnum acc bits o))
      (loop for i from start below end
            do (let ((d (aref table (aref v i))))
                 (when (< d 0) (return-from b64-decode nil))
                 (setf acc (logior (ldb (byte 18 0) (ash acc 6)) d))
                 (incf bits 6)
                 (when (>= bits 8)
                   (decf bits 8)
                   (setf (aref out o) (ldb (byte 8 bits) acc))
                   (incf o))))
      out)))
