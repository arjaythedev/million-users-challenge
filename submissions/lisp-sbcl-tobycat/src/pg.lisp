;;;; Postgres wire protocol v3: startup and authentication (trust, password, MD5, SCRAM-SHA-256),
;;;; prepared statements, and pipelined Bind/Execute with binary results.

(in-package :mu)

(defstruct dbinfo host ip4 (port 5432) user password database)

(defun parse-database-url (url)
  (let* ((p (search "://" url))
         (rest (if p (subseq url (+ p 3)) url))
         (q (position #\? rest))
         (rest (if q (subseq rest 0 q) rest))
         (slash (position #\/ rest))
         (authority (subseq rest 0 slash))
         (db (if slash (subseq rest (1+ slash)) ""))
         (at (position #\@ authority :from-end t))
         (cred (if at (subseq authority 0 at) ""))
         (hostport (if at (subseq authority (1+ at)) authority))
         (colon (position #\: cred))
         (user (percent-decode (if colon (subseq cred 0 colon) cred)))
         (password (and colon (percent-decode (subseq cred (1+ colon)))))
         (hcolon (position #\: hostport :from-end t))
         (host (if hcolon (subseq hostport 0 hcolon) hostport))
         (port (if hcolon (parse-integer hostport :start (1+ hcolon)) 5432)))
    (make-dbinfo :host host :ip4 (resolve-host host) :port port
                 :user (if (string= user "") "postgres" user)
                 :password password
                 :database (if (string= db "") (if (string= user "") "postgres" user) (percent-decode db)))))

;;; ---------------------------------------------------------------- blocking I/O (startup only)

(defun write-all (fd v &optional (start 0) (end (length v)))
  (loop while (< start end)
        do (let ((n (with-pinned-objects (v) (c-send fd (sap+ (vector-sap v) start) (- end start) +msg-nosignal+))))
             (cond ((> n 0) (incf start n))
                   ((and (< n 0) (= (errno) +eintr+)))
                   (t (error "postgres write failed (errno ~D)" (errno)))))))

(defun read-exact (fd n)
  (let ((v (make-octets n)) (got 0))
    (loop while (< got n)
          do (let ((r (with-pinned-objects (v) (c-recv fd (sap+ (vector-sap v) got) (- n got) 0))))
               (cond ((> r 0) (incf got r))
                     ((and (< r 0) (= (errno) +eintr+)))
                     ((= r 0) (error "postgres closed the connection"))
                     (t (error "postgres read failed (errno ~D)" (errno))))))
    v))

(defun read-message (fd)
  "(values type-char payload-octets)"
  (let* ((h (read-exact fd 5)) (len (be32 h 1)))
    (values (code-char (aref h 0)) (if (> len 4) (read-exact fd (- len 4)) (make-octets 0)))))

(defun error-fields (payload)
  (let ((fields '()) (i 0))
    (loop while (and (< i (length payload)) (/= (aref payload i) 0))
          do (let* ((code (code-char (aref payload i)))
                    (e (position 0 payload :start (1+ i))))
               (push (cons code (sb-ext:octets-to-string payload :start (1+ i) :end e :external-format :utf-8)) fields)
               (setf i (1+ e))))
    fields))

(defun pg-error-string (payload)
  (let ((f (error-fields payload)))
    (format nil "~A ~A: ~A" (or (cdr (assoc #\S f)) "ERROR") (or (cdr (assoc #\C f)) "") (or (cdr (assoc #\M f)) ""))))

(defun msg (type &rest parts)
  "One frontend message from parts: octets, strings (NUL-terminated), (:u16 n), (:u32 n)."
  (let ((b (make-obuf 64)))
    (when type (ob-byte b (char-code type)))
    (let ((lenpos (obuf-len b)))
      (ob-u32 b 0)
      (dolist (p parts)
        (etypecase p
          (string (ob-cstr b p))
          (octets (ob-bytes b p))
          (cons (ecase (first p) (:u16 (ob-u16 b (second p))) (:u32 (ob-u32 b (second p)))))))
      (let ((len (- (obuf-len b) lenpos)) (d (obuf-data b)))
        (setf (aref d lenpos) (ldb (byte 8 24) len) (aref d (+ lenpos 1)) (ldb (byte 8 16) len)
              (aref d (+ lenpos 2)) (ldb (byte 8 8) len) (aref d (+ lenpos 3)) (ldb (byte 8 0) len))))
    (subseq (obuf-data b) 0 (obuf-len b))))

(defun random-octets (n)
  (with-open-file (f "/dev/urandom" :element-type '(unsigned-byte 8))
    (let ((v (make-octets n))) (read-sequence v f) v)))

(defun md5-hex (v) (string-downcase (format nil "~{~2,'0x~}" (coerce (sb-md5:md5sum-sequence v) 'list))))

(defun scram-auth (fd info)
  (let* ((password (utf8 (or (dbinfo-password info) "")))
         (cnonce (b64-encode (random-octets 18)))
         (first-bare (format nil "n=,r=~A" cnonce))
         (first (utf8 (concatenate 'string "n,," first-bare))))
    (write-all fd (msg #\p "SCRAM-SHA-256" (list :u32 (length first)) first))
    (multiple-value-bind (ty payload) (read-message fd)
      (when (char= ty #\E) (error "postgres: ~A" (pg-error-string payload)))
      (unless (and (char= ty #\R) (= (be32 payload 0) 11)) (error "postgres: unexpected SASL reply ~A" ty))
      (let* ((server-first (sb-ext:octets-to-string payload :start 4 :external-format :utf-8))
             (attrs (mapcar (lambda (kv) (cons (char kv 0) (subseq kv 2))) (split-string server-first #\,)))
             (nonce (cdr (assoc #\r attrs)))
             (salt (let ((s (utf8 (cdr (assoc #\s attrs))))) (b64-decode s 0 (length s))))
             (iters (parse-integer (cdr (assoc #\i attrs))))
             (salted (pbkdf2-sha256 password salt iters))
             (client-key (hmac (make-hmac-key salted) (ascii "Client Key")))
             (stored-key (sha256 client-key))
             (final-bare (format nil "c=biws,r=~A" nonce))
             (auth-msg (utf8 (format nil "~A,~A,~A" first-bare server-first final-bare)))
             (sig (hmac (make-hmac-key stored-key) auth-msg))
             (proof (map 'octets #'logxor client-key sig)))
        (unless (and nonce (> (length nonce) (length cnonce)) (string= cnonce nonce :end2 (length cnonce)))
          (error "postgres: bad SCRAM nonce"))
        (write-all fd (msg #\p (utf8 (format nil "~A,p=~A" final-bare (b64-encode proof)))))
        (multiple-value-bind (ty2 p2) (read-message fd)
          (when (char= ty2 #\E) (error "postgres: ~A" (pg-error-string p2)))
          (unless (and (char= ty2 #\R) (= (be32 p2 0) 12)) (error "postgres: unexpected SCRAM final ~A" ty2)))))))

;;; The statements from SPEC.md, one per request.
(defparameter +post-select+
  "SELECT p.id, p.body, p.created_at, u.username, (SELECT count(*) FROM likes l WHERE l.post_id = p.id)::int AS like_count FROM posts p JOIN users u ON u.id = p.user_id")

(defparameter +statements+
  `(("f" ,(concatenate 'string +post-select+ " ORDER BY p.created_at DESC, p.id DESC LIMIT 20") ())
    ("p" ,(concatenate 'string +post-select+ " WHERE p.id = $1") (20))
    ("c" "INSERT INTO posts (user_id, body) VALUES ($1, $2) RETURNING id, created_at" (20 25))
    ("l" "INSERT INTO likes (user_id, post_id) VALUES ($1, $2) ON CONFLICT (user_id, post_id) DO NOTHING" (20 20))
    ("h" "SELECT 1" ())))

(defun pg-connect (info)
  "A logged-in, non-blocking Postgres socket with the statements prepared."
  (let ((fd (connect-tcp (dbinfo-ip4 info) (dbinfo-port info) 3)))
    (handler-bind ((error (lambda (e) (declare (ignore e)) (c-close fd))))
      (write-all fd (msg nil '(:u32 196608)
                         "user" (dbinfo-user info) "database" (dbinfo-database info)
                         "client_encoding" "UTF8" "application_name" "lisp-sbcl-tobycat"
                         "jit" "off" "max_parallel_workers_per_gather" "0"
                         (make-octets 1)))
      (loop
        (multiple-value-bind (ty payload) (read-message fd)
          (case ty
            (#\R (let ((code (be32 payload 0)))
                   (case code
                     (0)
                     (3 (write-all fd (msg #\p (or (dbinfo-password info) ""))))
                     (5 (let ((inner (md5-hex (utf8 (concatenate 'string (or (dbinfo-password info) "") (dbinfo-user info))))))
                          (write-all fd (msg #\p (concatenate 'string "md5"
                                                              (md5-hex (concatenate 'octets (ascii inner) (subseq payload 4 8))))))))
                     (10 (scram-auth fd info))
                     (t (error "postgres: unsupported authentication method ~D" code)))))
            (#\E (error "postgres: ~A" (pg-error-string payload)))
            (#\Z (return))
            (t))))
      ;; prepare
      (let ((b (make-obuf 1024)))
        (loop for (name sql types) in +statements+
              do (ob-bytes b (apply #'msg #\P name sql (list :u16 (length types))
                                    (mapcar (lambda (ty) (list :u32 ty)) types))))
        (ob-bytes b (msg #\S))
        (write-all fd (obuf-data b) 0 (obuf-len b)))
      (loop
        (multiple-value-bind (ty payload) (read-message fd)
          (case ty
            (#\E (error "postgres prepare: ~A" (pg-error-string payload)))
            (#\Z (return))
            (t))))
      (clear-timeouts fd)
      (set-nonblocking fd nil)
      fd)))

;;; ---------------------------------------------------------------- pipelined messages

(defun bind-execute (b stmt params)
  "Appends Bind (binary params and results) + Execute. PARAMS: list of (:i8 n) or octets."
  (declare (type obuf b))
  (ob-byte b 66)                         ; 'B'
  (let ((lenpos (obuf-len b)))
    (ob-u32 b 0)
    (ob-byte b 0)                        ; unnamed portal
    (ob-byte b (char-code stmt)) (ob-byte b 0)
    (ob-u16 b 1) (ob-u16 b 1)            ; all params binary
    (ob-u16 b (length params))
    (dolist (p params)
      (if (consp p)
          (progn (ob-u32 b 8) (ob-i64 b (second p)))
          (progn (ob-u32 b (length p)) (ob-bytes b p))))
    (ob-u16 b 1) (ob-u16 b 1)            ; all results binary
    (let ((len (- (obuf-len b) lenpos)) (d (obuf-data b)))
      (setf (aref d lenpos) (ldb (byte 8 24) len) (aref d (+ lenpos 1)) (ldb (byte 8 16) len)
            (aref d (+ lenpos 2)) (ldb (byte 8 8) len) (aref d (+ lenpos 3)) (ldb (byte 8 0) len))))
  ;; Execute unnamed portal, no row limit
  (ob-byte b 69) (ob-u32 b 9) (ob-byte b 0) (ob-u32 b 0))

(defun append-sync (b) (ob-byte b 83) (ob-u32 b 4))
