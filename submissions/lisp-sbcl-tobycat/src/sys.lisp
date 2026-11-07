;;;; Raw Linux syscalls through libc, octet buffers and small helpers.

(defpackage :mu
  (:use :cl :sb-alien :sb-sys))
(in-package :mu)

(deftype octets () '(simple-array (unsigned-byte 8) (*)))
(deftype u8 () '(unsigned-byte 8))
(deftype u32 () '(unsigned-byte 32))

(declaim (inline make-octets))
(defun make-octets (n) (make-array n :element-type '(unsigned-byte 8)))

(defun ascii (s)
  "A literal string as octets (ASCII only)."
  (map 'octets #'char-code s))

(defun utf8 (s) (sb-ext:string-to-octets s :external-format :utf-8))

;;; ---------------------------------------------------------------- syscalls

(defconstant +eagain+ 11)
(defconstant +eintr+ 4)
(defconstant +einprogress+ 115)
(defconstant +msg-nosignal+ #x4000)
(defconstant +sock-nonblock+ #o4000)
(defconstant +sock-cloexec+ #o2000000)
(defconstant +epollin+ 1)
(defconstant +epollout+ 4)
(defconstant +epoll-ctl-add+ 1)
(defconstant +epoll-ctl-del+ 2)
(defconstant +epoll-ctl-mod+ 3)

(defmacro defc (lisp-name c-name ret &rest args)
  `(progn
     (declaim (inline ,lisp-name))
     (defun ,lisp-name ,(mapcar #'first args)
       (alien-funcall (extern-alien ,c-name (function ,ret ,@(mapcar #'second args)))
                      ,@(mapcar #'first args)))))

(defc c-recv "recv" long (fd int) (buf system-area-pointer) (n unsigned-long) (flags int))
(defc c-send "send" long (fd int) (buf system-area-pointer) (n unsigned-long) (flags int))
(defc c-close "close" int (fd int))
(defc c-socket "socket" int (domain int) (type int) (proto int))
(defc c-setsockopt "setsockopt" int (fd int) (level int) (opt int) (val system-area-pointer) (len unsigned))
(defc c-bind "bind" int (fd int) (addr system-area-pointer) (len unsigned))
(defc c-connect "connect" int (fd int) (addr system-area-pointer) (len unsigned))
(defc c-listen "listen" int (fd int) (backlog int))
(defc c-accept4 "accept4" int (fd int) (addr system-area-pointer) (len system-area-pointer) (flags int))
(defc c-fcntl "fcntl" int (fd int) (cmd int) (arg long))
(defc c-epoll-create1 "epoll_create1" int (flags int))
(defc c-epoll-ctl "epoll_ctl" int (ep int) (op int) (fd int) (ev system-area-pointer))
(defc c-epoll-wait "epoll_wait" int (ep int) (evs system-area-pointer) (max int) (timeout int))
(defc c-prctl "prctl" int (op int) (a unsigned-long) (b unsigned-long) (c unsigned-long) (d unsigned-long))
(defc c-sched-getaffinity "sched_getaffinity" int (pid int) (size unsigned-long) (mask system-area-pointer))
(defc c-kill "kill" int (pid int) (sig int))
(defc c-getrlimit "getrlimit" int (res int) (lim system-area-pointer))
(defc c-setrlimit "setrlimit" int (res int) (lim system-area-pointer))



(declaim (inline errno))
(defun errno () (get-errno))

(defmacro with-foreign-bytes ((sap n) &body body)
  (let ((a (gensym)))
    `(with-alien ((,a (array (unsigned 8) ,n)))
       (let ((,sap (alien-sap ,a))) ,@body))))

(defun raise-nofile-limit ()
  "Raises the soft open-file limit to the hard limit; returns the new soft limit."
  (with-foreign-bytes (lim 16)
    (when (zerop (c-getrlimit 7 lim))      ; RLIMIT_NOFILE
      (setf (sap-ref-64 lim 0) (sap-ref-64 lim 8))
      (c-setrlimit 7 lim)
      (c-getrlimit 7 lim)
      (sap-ref-64 lim 0))))

(defun cpu-count ()
  (with-foreign-bytes (mask 128)
    (dotimes (i 128) (setf (sap-ref-8 mask i) 0))
    (if (zerop (c-sched-getaffinity 0 128 mask))
        (loop for i below 128 sum (logcount (sap-ref-8 mask i)))
        1)))

(defun set-sockopt-int (fd level opt value)
  (with-foreign-bytes (v 4)
    (setf (sap-ref-32 v 0) value)
    (c-setsockopt fd level opt v 4)))

(defun set-nonblocking (fd blocking-p)
  ;; F_SETFL = 4, O_NONBLOCK = #o4000
  (c-fcntl fd 4 (if blocking-p 0 #o4000)))

(defun fill-sockaddr (sap ip4 port)
  (dotimes (i 16) (setf (sap-ref-8 sap i) 0))
  (setf (sap-ref-8 sap 0) 2                ; AF_INET
        (sap-ref-8 sap 2) (ldb (byte 8 8) port)
        (sap-ref-8 sap 3) (ldb (byte 8 0) port))
  (dotimes (i 4) (setf (sap-ref-8 sap (+ 4 i)) (aref ip4 i))))

(defun parse-ip4 (s)
  "#(a b c d) for a dotted IPv4 literal, else NIL."
  (let ((parts (split-string s #\.)))
    (when (= (length parts) 4)
      (let ((v (map 'vector (lambda (p)
                              (and (plusp (length p)) (<= (length p) 3) (every #'digit-char-p p)
                                   (parse-integer p)))
                    parts)))
        (when (every (lambda (x) (and x (<= x 255))) v) v)))))

(defun resolve-host (host)
  (or (parse-ip4 host)
      (if (string= host "localhost")
          #(127 0 0 1)
          (sb-bsd-sockets:host-ent-address (sb-bsd-sockets:get-host-by-name host)))))

(defun make-listener (ip4 port)
  (let ((fd (c-socket 2 (logior 1 +sock-nonblock+ +sock-cloexec+) 0)))
    (when (< fd 0) (error "socket: errno ~D" (errno)))
    (set-sockopt-int fd 1 2 1)            ; SO_REUSEADDR
    (set-sockopt-int fd 1 15 1)           ; SO_REUSEPORT
    (with-foreign-bytes (sa 16)
      (fill-sockaddr sa ip4 port)
      (unless (zerop (c-bind fd sa 16)) (error "bind port ~D: errno ~D" port (errno))))
    (unless (zerop (c-listen fd 65535)) (error "listen: errno ~D" (errno)))
    fd))

(defun connect-tcp (ip4 port timeout-s)
  "A blocking TCP socket with send/receive timeouts, or an error."
  (let ((fd (c-socket 2 (logior 1 +sock-cloexec+) 0)))
    (when (< fd 0) (error "socket: errno ~D" (errno)))
    (with-foreign-bytes (tv 16)
      (setf (sap-ref-64 tv 0) timeout-s (sap-ref-64 tv 8) 0)
      (c-setsockopt fd 1 20 tv 16)        ; SO_RCVTIMEO
      (c-setsockopt fd 1 21 tv 16))       ; SO_SNDTIMEO (also bounds connect)
    (set-sockopt-int fd 6 1 1)            ; TCP_NODELAY
    (with-foreign-bytes (sa 16)
      (fill-sockaddr sa ip4 port)
      (unless (zerop (c-connect fd sa 16))
        (let ((e (errno))) (c-close fd) (error "connect: errno ~D" e))))
    fd))

(defun clear-timeouts (fd)
  (with-foreign-bytes (tv 16)
    (setf (sap-ref-64 tv 0) 0 (sap-ref-64 tv 8) 0)
    (c-setsockopt fd 1 20 tv 16)
    (c-setsockopt fd 1 21 tv 16)))

(defun unix-time ()
  (- (get-universal-time) #.(encode-universal-time 0 0 0 1 1 1970 0)))

(defun monotonic-ms ()
  (floor (* 1000 (get-internal-real-time)) internal-time-units-per-second))

;;; ---------------------------------------------------------------- strings

(defun split-string (s ch)
  (loop with start = 0
        for pos = (position ch s :start start)
        collect (subseq s start pos)
        while pos do (setf start (1+ pos))))

(defun percent-decode (s)
  (let ((out (make-array (length s) :element-type '(unsigned-byte 8) :fill-pointer 0)) (i 0))
    (loop while (< i (length s))
          do (let ((c (char s i)))
               (if (and (char= c #\%) (<= (+ i 3) (length s)))
                   (progn (vector-push (parse-integer s :start (1+ i) :end (+ i 3) :radix 16) out)
                          (incf i 3))
                   (progn (loop for b across (utf8 (string c)) do (vector-push b out))
                          (incf i)))))
    (sb-ext:octets-to-string (coerce out 'octets) :external-format :utf-8)))

;;; ---------------------------------------------------------------- growable output buffer

(defstruct (obuf (:constructor make-obuf (&optional (size 1024) &aux (data (make-octets size)))))
  (data (make-octets 0) :type octets)
  (len 0 :type fixnum))

(defun ob-grow (b need)
  (declare (type obuf b) (type fixnum need))
  (let* ((old (obuf-data b))
         (new (make-octets (max need (* 2 (length old))))))
    (replace new old :end2 (obuf-len b))
    (setf (obuf-data b) new)))

(declaim (inline ob-room ob-byte))
(defun ob-room (b n)
  (declare (type obuf b) (type fixnum n))
  (let ((need (+ (obuf-len b) n)))
    (when (> need (length (obuf-data b))) (ob-grow b need))))

(defun ob-byte (b x)
  (declare (type obuf b) (type u8 x) (optimize speed (safety 0)))
  (ob-room b 1)
  (setf (aref (obuf-data b) (obuf-len b)) x)
  (incf (obuf-len b)))

(defun ob-bytes (b v &optional (start 0) (end (length v)))
  (declare (type obuf b) (type octets v) (type fixnum start end) (optimize speed (safety 0)))
  (let ((n (- end start)))
    (ob-room b n)
    (replace (obuf-data b) v :start1 (obuf-len b) :start2 start :end2 end)
    (incf (obuf-len b) n)))

(defun ob-u16 (b x) (ob-byte b (ldb (byte 8 8) x)) (ob-byte b (ldb (byte 8 0) x)))
(defun ob-u32 (b x)
  (declare (type obuf b) (type (unsigned-byte 32) x))
  (ob-room b 4)
  (let ((d (obuf-data b)) (i (obuf-len b)))
    (setf (aref d i) (ldb (byte 8 24) x) (aref d (+ i 1)) (ldb (byte 8 16) x)
          (aref d (+ i 2)) (ldb (byte 8 8) x) (aref d (+ i 3)) (ldb (byte 8 0) x))
    (setf (obuf-len b) (+ i 4))))
(defun ob-i64 (b x)
  (let ((u (ldb (byte 64 0) x)))
    (ob-u32 b (ldb (byte 32 32) u)) (ob-u32 b (ldb (byte 32 0) u))))
(defun ob-cstr (b s) (ob-bytes b (utf8 s)) (ob-byte b 0))

(defun ob-uint (b n)
  "Decimal digits of a non-negative integer."
  (declare (type obuf b) (optimize speed))
  (if (typep n '(unsigned-byte 62))
      (let ((len 0) (x n))
        (declare (type (unsigned-byte 62) x) (type fixnum len))
        ;; count digits, then write them right to left
        (loop do (setf x (floor x 10)) (incf len) until (zerop x))
        (ob-room b len)
        (let ((d (obuf-data b)) (o (+ (obuf-len b) len)) (x n))
          (declare (type (unsigned-byte 62) x) (type fixnum o))
          (setf (obuf-len b) o)
          (loop do (multiple-value-bind (q r) (floor x 10)
                     (decf o) (setf (aref d o) (+ 48 r) x q))
                until (zerop x))))
      (ob-bytes b (ascii (princ-to-string n)))))

(defun ob-int (b n)
  (declare (type obuf b) (type integer n))
  (when (minusp n) (ob-byte b 45) (setf n (- n)))
  (ob-uint b n))

(declaim (inline be16 be32))
(defun be16 (v i) (declare (type octets v) (type fixnum i)) (logior (ash (aref v i) 8) (aref v (1+ i))))
(defun be32 (v i)
  (declare (type octets v) (type fixnum i))
  (logior (ash (aref v i) 24) (ash (aref v (+ i 1)) 16) (ash (aref v (+ i 2)) 8) (aref v (+ i 3))))
(defun be64s (v i)
  "Signed big-endian int64."
  (let ((u (logior (ash (be32 v i) 32) (be32 v (+ i 4)))))
    (if (logbitp 63 u) (- u (ash 1 64)) u)))
(defun be32s (v i) (let ((u (be32 v i))) (if (logbitp 31 u) (- u (ash 1 32)) u)))
