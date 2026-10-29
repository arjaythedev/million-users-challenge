Extern "C"
Declare Function basic_listener(ByVal host As ZString Ptr, ByVal port As Long) As Long
Declare Function basic_accept(ByVal fd As Long) As Long
Declare Function basic_errno() As Long
Declare Sub basic_release(ByVal j As Any Ptr)
Declare Sub basic_signals()
Declare Function basic_cpus() As Long
Declare Function fork() As Long
Declare Function waitpid(ByVal pid As Long, ByVal status As Long Ptr, ByVal flags As Long) As Long
Declare Function cclose_ Alias "close"(ByVal fd As Long) As Long
Declare Function recv(ByVal fd As Long, ByVal buf As Any Ptr, ByVal n As ULongInt, ByVal flags As Long) As LongInt
Declare Function send(ByVal fd As Long, ByVal buf As Any Ptr, ByVal n As ULongInt, ByVal flags As Long) As LongInt
Declare Function ctime_ Alias "time"(ByVal p As Any Ptr) As LongInt
Declare Function memcpy(ByVal dest As Any Ptr, ByVal src As Any Ptr, ByVal n As ULongInt) As Any Ptr
Declare Sub free(ByVal p As Any Ptr)
Declare Function event_base_new() As Any Ptr
Declare Function event_base_dispatch(ByVal b As Any Ptr) As Long
Declare Function event_new(ByVal b As Any Ptr, ByVal fd As Long, ByVal flags As Short, ByVal cb As Any Ptr, ByVal arg As Any Ptr) As Any Ptr
Declare Function event_assign(ByVal e As Any Ptr, ByVal b As Any Ptr, ByVal fd As Long, ByVal flags As Short, ByVal cb As Any Ptr, ByVal arg As Any Ptr) As Long
Declare Function event_add(ByVal e As Any Ptr, ByVal tv As Any Ptr) As Long
Declare Function event_del(ByVal e As Any Ptr) As Long
Declare Sub event_free(ByVal e As Any Ptr)
Declare Function PQconnectdb(ByVal s As ZString Ptr) As Any Ptr
Declare Function PQstatus(ByVal p As Any Ptr) As Long
Declare Function PQexec(ByVal p As Any Ptr, ByVal s As ZString Ptr) As Any Ptr
Declare Function PQprepare(ByVal p As Any Ptr, ByVal nm As ZString Ptr, ByVal sql As ZString Ptr, ByVal n As Long, ByVal types As Any Ptr) As Any Ptr
Declare Function PQsetnonblocking(ByVal p As Any Ptr, ByVal flag As Long) As Long
Declare Function PQsocket(ByVal p As Any Ptr) As Long
Declare Function PQsendQueryPrepared(ByVal p As Any Ptr, ByVal nm As ZString Ptr, ByVal n As Long, ByVal vals As Any Ptr, ByVal lens As Any Ptr, ByVal fmts As Any Ptr, ByVal resfmt As Long) As Long
Declare Function PQflush(ByVal p As Any Ptr) As Long
Declare Function PQconsumeInput(ByVal p As Any Ptr) As Long
Declare Function PQisBusy(ByVal p As Any Ptr) As Long
Declare Function PQgetResult(ByVal p As Any Ptr) As Any Ptr
Declare Function PQresultStatus(ByVal p As Any Ptr) As Long
Declare Function PQntuples(ByVal p As Any Ptr) As Long
Declare Function PQgetvalue(ByVal p As Any Ptr, ByVal row As Long, ByVal col As Long) As ZString Ptr
Declare Function PQcmdTuples(ByVal p As Any Ptr) As ZString Ptr
Declare Function PQresultErrorField(ByVal p As Any Ptr, ByVal field As Long) As ZString Ptr
Declare Sub PQclear(ByVal p As Any Ptr)
Declare Function json_loadb(ByVal s As Any Ptr, ByVal n As ULongInt, ByVal flags As ULongInt, ByVal errbuf As Any Ptr) As Any Ptr
Declare Function json_object_get(ByVal j As Any Ptr, ByVal nm As ZString Ptr) As Any Ptr
Declare Function json_string_value(ByVal j As Any Ptr) As ZString Ptr
Declare Function json_string_length(ByVal j As Any Ptr) As ULongInt
Declare Function json_integer_value(ByVal j As Any Ptr) As LongInt
Declare Function json_stringn(ByVal s As Any Ptr, ByVal n As ULongInt) As Any Ptr
Declare Function json_dumps(ByVal j As Any Ptr, ByVal flags As ULongInt) As ZString Ptr
Declare Function EVP_sha256() As Any Ptr
Declare Function EVP_DecodeBlock(ByVal dest As Any Ptr, ByVal src As Any Ptr, ByVal n As Long) As Long
Declare Function HMAC(ByVal md As Any Ptr, ByVal key As Any Ptr, ByVal klen As Long, ByVal data_ As Any Ptr, ByVal n As ULongInt, ByVal dest As Any Ptr, ByVal outlen As ULong Ptr) As Any Ptr
Declare Function CRYPTO_memcmp(ByVal a As Any Ptr, ByVal b As Any Ptr, ByVal n As ULongInt) As Long
End Extern
