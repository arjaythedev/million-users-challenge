#include once "api.bi"
Const EV_READ=2, EV_WRITE=4, EV_PERSIST=16
Type Client
 fd As Long
 ev As Any Ptr
 evflags As Short
 incoming As String
 outgoing As String
 sent As Integer
 busy As Long
 route As Long
 ident As String
 userid As String
 username As String
 body As String
 next_ As Client Ptr
End Type
Type DBSlot
 conn As Any Ptr
 ev As Any Ptr
 current As Client Ptr
 pending_result As Any Ptr
End Type
Dim Shared eventbase As Any Ptr, secret As String, started As LongInt
Dim Shared slots(0 To 127) As DBSlot, pool As Long
Dim Shared qhead As Client Ptr, qtail As Client Ptr, qcount As Long
Dim Shared sqls(0 To 4) As String
Declare Sub client_event Cdecl(ByVal fd As Long, ByVal flags As Short, ByVal arg As Any Ptr)
Declare Sub db_event Cdecl(ByVal fd As Long, ByVal flags As Short, ByVal arg As Any Ptr)
Declare Sub process_request(ByVal c As Client Ptr)
Declare Sub dispatch(ByVal s As DBSlot Ptr, ByVal c As Client Ptr)

Function num(ByVal n As LongInt) As String
 Return LTrim(Str(n))
End Function
Function quote_json(ByRef s As String) As String
 ' Emit JSON in BASIC, allocating once and copying unescaped UTF-8 bytes.
 Dim out_ As String=String(2+Len(s)*6,0), n As Integer=1
 out_[0]=34
 Const digits="0123456789abcdef"
 For i As Integer=0 To Len(s)-1
  Dim ch As UByte=s[i]
  Select Case ch
  Case 34,92
   out_[n]=92:out_[n+1]=ch:n+=2
  Case 0 To 31
   out_[n]=92:out_[n+1]=117:out_[n+2]=48:out_[n+3]=48
   out_[n+4]=Asc(Mid(digits,(ch Shr 4)+1,1))
   out_[n+5]=Asc(Mid(digits,(ch And 15)+1,1)):n+=6
  Case Else
   out_[n]=ch:n+=1
  End Select
 Next
 out_[n]=34
 Return Left(out_,n+1)
End Function

Function getstr(ByVal j As Any Ptr, ByVal key As ZString Ptr) As String
 Dim v As Any Ptr=json_object_get(j,key)
 Dim p As ZString Ptr=json_string_value(v)
 If p=0 Then Return ""
 Dim result As String=String(json_string_length(v),0)
 If Len(result)>0 Then memcpy(StrPtr(result),p,Len(result))
 Return result
End Function
Function positive(ByRef s As String) As Long
 If Len(s)=0 Or Len(s)>16 Then Return 0
 For i As Integer=0 To Len(s)-1
  If s[i]<48 Or s[i]>57 Then Return 0
 Next
 Return IIf(ValLng(s)>0,-1,0)
End Function
Function whitespace_bytes(ByVal p As UByte Ptr, ByVal n As Integer) As Integer
 If n=0 Then Return 0
 If p[0]=32 Or (p[0]>=9 And p[0]<=13) Then Return 1
 If n>=2 Then
  If p[0]=&hC2 And (p[1]=&h85 Or p[1]=&hA0) Then Return 2
 End If
 If n>=3 Then
  If p[0]=&hE1 And p[1]=&h9A And p[2]=&h80 Then Return 3
  If p[0]=&hE2 Then
   If p[1]=&h80 And ((p[2]>=&h80 And p[2]<=&h8A) Or p[2]=&hA8 Or p[2]=&hA9 Or p[2]=&hAF) Then Return 3
   If p[1]=&h81 And p[2]=&h9F Then Return 3
  End If
  If p[0]=&hE3 And p[1]=&h80 And p[2]=&h80 Then Return 3
 End If
 Return 0
End Function
Function trim_body(ByRef s As String) As String
 Dim first_ As Integer=0, last_ As Integer=0, i As Integer=0
 While i<Len(s)
  Dim w As Integer=whitespace_bytes(CPtr(UByte Ptr,StrPtr(s))+i,Len(s)-i)
  If w>0 Then
   i+=w
   If last_=0 Then first_=i
  Else
   i+=1:last_=i
  End If
 Wend
 If last_=0 Then Return ""
 Return Mid(s,first_+1,last_-first_)
End Function
Function b64decode(ByRef s As String) As String
 Dim a As String=s
 If Len(a)=0 Or Len(a) Mod 4=1 Then Return ""
 For i As Integer=0 To Len(a)-1
  Select Case a[i]
  Case 45:a[i]=43
  Case 95:a[i]=47
  Case 48 To 57,65 To 90,97 To 122
  Case Else:Return ""
  End Select
 Next
 Dim padding As Long=(4-Len(a) Mod 4) Mod 4
 a+=String(padding,"=")
 Dim dest As String=String(Len(a),0)
 Dim n As Long=EVP_DecodeBlock(StrPtr(dest),StrPtr(a),Len(a))
 If n<0 Then Return ""
 Return Left(dest,n-padding)
End Function
Function auth(ByVal c As Client Ptr, ByRef header As String) As String
 If Left(header,7)<>"Bearer " Then Return "missing bearer token"
 Dim tok As String=Mid(header,8), a As Integer=InStr(tok,"."), b As Integer=InStr(a+1,tok,".")
 If a<2 Or b<=a+1 Or InStr(b+1,tok,".")>0 Then Return "invalid or expired token"
 Dim hh As String=Left(tok,a-1), pp As String=Mid(tok,a+1,b-a-1), ss As String=Mid(tok,b+1)
 hh=b64decode(hh):pp=b64decode(pp):ss=b64decode(ss)
 If Len(ss)<>32 Then Return "invalid or expired token"
 Dim digest(0 To 31) As UByte, dlen As ULong
 HMAC(EVP_sha256(),StrPtr(secret),Len(secret),StrPtr(tok),b-1,@digest(0),@dlen)
 If CRYPTO_memcmp(@digest(0),StrPtr(ss),32)<>0 Then Return "invalid or expired token"
 Dim h As Any Ptr=json_loadb(StrPtr(hh),Len(hh),1,0), p As Any Ptr=json_loadb(StrPtr(pp),Len(pp),1,0)
 Dim result As String="invalid or expired token"
 If h<>0 And p<>0 Then
  If getstr(h,"alg")="HS256" And json_integer_value(json_object_get(p,"exp"))>ctime_(0) Then
   c->userid=getstr(p,"sub"):c->username=getstr(p,"username")
   Dim u As Any Ptr=json_object_get(p,"username")
   If positive(c->userid) And json_string_value(u)<>0 Then
    result=""
   Else
    result="invalid token payload"
   End If
  End If
 End If
 If h<>0 Then basic_release(h)
 If p<>0 Then basic_release(p)
 Return result
End Function
Sub watch_client(ByVal c As Client Ptr, ByVal flags As Short)
 If c->ev=0 Then
  c->ev=event_new(eventbase,c->fd,flags Or EV_PERSIST,@client_event,c)
 ElseIf c->evflags<>flags Then
  event_del(c->ev)
  event_assign(c->ev,eventbase,c->fd,flags Or EV_PERSIST,@client_event,c)
 End If
 c->evflags=flags
 event_add(c->ev,0)
End Sub

Sub destroy_client(ByVal c As Client Ptr)
 If c->ev<>0 Then event_free(c->ev)
 cclose_(c->fd):Delete c
End Sub
Sub respond(ByVal c As Client Ptr, ByVal status As Long, ByRef body As String)
 c->outgoing="HTTP/1.1 "+num(status)+" Response"+Chr(13,10)+"Content-Type: application/json"+Chr(13,10)+"Content-Length: "+num(Len(body))+Chr(13,10)+"Connection: keep-alive"+Chr(13,10,13,10)+body
 c->sent=0:c->busy=0
 client_event(c->fd,EV_WRITE,c)
End Sub
Sub fail(ByVal c As Client Ptr, ByVal status As Long, ByVal msg As String)
 Dim body As String="{""error"":"+quote_json(msg)+"}"
 respond(c,status,body)
End Sub
Function timestamp_json(ByVal p As ZString Ptr) As String
 Dim s As String=*p
 Dim t As String=Left(s,10)+"T"+Mid(s,12,8), fraction_ As String="000"
 If Mid(s,20,1)="." Then
  fraction_=""
  For i As Integer=21 To Len(s)
   Dim v As String=Mid(s,i,1)
   If v<"0" Or v>"9" Then Exit For
   fraction_+=v
  Next
  fraction_=Left(fraction_+"000",3)
 End If
 Return """"+t+"."+fraction_+"Z"""
End Function
Function post_json(ByVal r As Any Ptr, ByVal row As Long) As String
 Dim body As String=*PQgetvalue(r,row,1), author As String=*PQgetvalue(r,row,3)
 Return "{""id"":"+*PQgetvalue(r,row,0)+",""body"":"+quote_json(body)+",""created_at"":"+timestamp_json(PQgetvalue(r,row,2))+",""author"":"+quote_json(author)+",""like_count"":"+*PQgetvalue(r,row,4)+"}"
End Function
Sub finish_result(ByVal c As Client Ptr, ByVal r As Any Ptr)
 Dim status As Long=PQresultStatus(r)
 If status<>1 And status<>2 Then
  Dim state_ As ZString Ptr=PQresultErrorField(r,67)
  If state_<>0 Then
   If *state_="23503" Then fail(c,404,"post not found"):Exit Sub
  End If
  If c->route=0 Then
   Dim body As String="{""status"":""degraded"",""db"":""unreachable"",""error"":""database query failed""}"
   respond(c,503,body)
  Else
   fail(c,500,"internal server error")
  End If
  Exit Sub
 End If
 Dim body As String, code As Long=200
 Select Case c->route
 Case 0:body="{""status"":""ok"",""db"":""ok"",""uptime_s"":"+num(ctime_(0)-started)+"}"
 Case 1
  body="{""posts"":["
  For i As Long=0 To PQntuples(r)-1
   If i>0 Then body+=","
   body+=post_json(r,i)
  Next
  body+="]}"
 Case 2
  If PQntuples(r)=0 Then fail(c,404,"post not found"):Exit Sub
  body="{""post"":"+post_json(r,0)+"}"
 Case 3
  code=201
  body="{""post"":{""id"":"+*PQgetvalue(r,0,0)+",""body"":"+quote_json(c->body)+",""created_at"":"+timestamp_json(PQgetvalue(r,0,1))+",""author"":"+quote_json(c->username)+",""like_count"":0}}"
 Case 4
  Dim already As String="true"
  If *PQcmdTuples(r)="1" Then code=201:already="false"
  body="{""liked"":true,""already_liked"":"+already+",""post_id"":"+num(ValLng(c->ident))+"}"
 End Select
 respond(c,code,body)
End Sub
Sub watch_db(ByVal s As DBSlot Ptr, ByVal flags As Short)
 If s->ev=0 Then
  s->ev=event_new(eventbase,PQsocket(s->conn),flags Or EV_PERSIST,@db_event,s)
 Else
  event_del(s->ev)
  event_assign(s->ev,eventbase,PQsocket(s->conn),flags Or EV_PERSIST,@db_event,s)
 End If
 event_add(s->ev,0)
End Sub

Sub dispatch(ByVal s As DBSlot Ptr, ByVal c As Client Ptr)
 s->current=c
 Dim vals(0 To 1) As ZString Ptr, n As Long=0
 Select Case c->route
 Case 2:n=1:vals(0)=StrPtr(c->ident)
 Case 3:n=2:vals(0)=StrPtr(c->userid):vals(1)=StrPtr(c->body)
 Case 4:n=2:vals(0)=StrPtr(c->userid):vals(1)=StrPtr(c->ident)
 End Select
 Dim nm As String=num(c->route)
 If PQsendQueryPrepared(s->conn,StrPtr(nm),n,@vals(0),0,0,0)=0 Then
  Print "database send failure":End 1
 End If
 Dim pending As Long=PQflush(s->conn)
 If pending<0 Then End 1
 watch_db(s,EV_READ Or IIf(pending=1,EV_WRITE,0))
End Sub
Sub db_event Cdecl(ByVal fd As Long, ByVal flags As Short, ByVal arg As Any Ptr)
 Dim s As DBSlot Ptr=arg
 If (flags And EV_READ)<>0 Then
  If PQconsumeInput(s->conn)=0 Then Print "database connection lost":End 1
 End If
 If (flags And EV_WRITE)<>0 Then
  Dim pending As Long=PQflush(s->conn)
  If pending<0 Then End 1
  If pending=0 Then watch_db(s,EV_READ)
 End If
 ' PQgetResult can yield CommandComplete before Sync commits the transaction.
 ' Call it only when PQisBusy is false, retaining the result until ReadyForQuery.
 While PQisBusy(s->conn)=0
  Dim r As Any Ptr=PQgetResult(s->conn)
  If r<>0 Then
   ' A deferred commit error can follow a successful statement result.
   If s->pending_result<>0 Then PQclear(s->pending_result)
   s->pending_result=r
  Else
   If s->pending_result=0 Then Exit Sub
   finish_result(s->current,s->pending_result):PQclear(s->pending_result)
   s->pending_result=0
   event_del(s->ev):s->current=0
   If qhead<>0 Then
    Dim c As Client Ptr=qhead:qhead=c->next_:qcount-=1
    If qhead=0 Then qtail=0
    c->next_=0:dispatch(s,c)
   End If
   Exit Sub
  End If
 Wend
End Sub
Sub enqueue(ByVal c As Client Ptr)
 c->busy=1:event_del(c->ev)
 For i As Long=0 To pool-1
  If slots(i).current=0 Then dispatch(@slots(i),c):Exit Sub
 Next
 If qcount>=4096 Then fail(c,503,"server busy"):Exit Sub
 If qtail<>0 Then qtail->next_=c Else qhead=c
 qtail=c:qcount+=1
End Sub
Sub process_request(ByVal c As Client Ptr)
 Dim eoh As Integer=InStr(c->incoming,Chr(13,10,13,10))
 If eoh=0 Then
  If Len(c->incoming)>32768 Then destroy_client(c)
  Exit Sub
 End If
 Dim firstEnd As Integer=InStr(c->incoming,Chr(13,10)), first As String=Left(c->incoming,firstEnd-1)
 Dim a As Integer=InStr(first," "), b As Integer=InStr(a+1,first," ")
 If a=0 Or b=0 Then destroy_client(c):Exit Sub
 Dim meth As String=Left(first,a-1), path As String=Mid(first,a+1,b-a-1)
 Dim authorization As String, contentLength As Long=0, cursor_ As Integer=firstEnd+2
 While cursor_<eoh
  Dim lineEnd As Integer=InStr(cursor_,c->incoming,Chr(13,10)), line_ As String=Mid(c->incoming,cursor_,lineEnd-cursor_), sep As Integer=InStr(line_,":")
  Dim key As String=LCase(Left(line_,sep-1)), value As String=Trim(Mid(line_,sep+1))
  If key="authorization" Then authorization=value
  If key="content-length" Then contentLength=ValInt(value)
  If key="transfer-encoding" Then destroy_client(c):Exit Sub
  cursor_=lineEnd+2
 Wend
 If contentLength<0 Or contentLength>16384 Then destroy_client(c):Exit Sub
 If Len(c->incoming)<eoh+3+contentLength Then Exit Sub
 Dim rawbody As String=Mid(c->incoming,eoh+4,contentLength)
 c->incoming=Mid(c->incoming,eoh+4+contentLength)
 c->ident="":c->userid="":c->username="":c->body=""
 c->route=-1
 If meth="GET" And path="/health" Then c->route=0
 If meth="GET" And path="/feed" Then c->route=1
 If meth="GET" And Left(path,7)="/posts/" Then c->route=2:c->ident=Mid(path,8)
 If meth="POST" And path="/posts" Then c->route=3
 If meth="POST" And Left(path,7)="/posts/" And Right(path,5)="/like" Then c->route=4:c->ident=Mid(path,8,Len(path)-12)
 If c->route=-1 Then fail(c,404,"not found"):Exit Sub
 If c->route>=3 Then
  Dim err_ As String=auth(c,authorization)
  If Len(err_)>0 Then fail(c,401,err_):Exit Sub
 End If
 If c->route=2 Or c->route=4 Then
  If positive(c->ident)=0 Then fail(c,400,"invalid post id"):Exit Sub
 End If
 If c->route=3 Then
  Dim j As Any Ptr=json_loadb(StrPtr(rawbody),Len(rawbody),1,0)
  If j=0 Then fail(c,400,"malformed JSON body"):Exit Sub
  c->body=getstr(j,"body"):basic_release(j)
  If InStr(c->body,Chr(0))>0 Then fail(c,400,"malformed JSON body"):Exit Sub
  c->body=trim_body(c->body)
  If Len(c->body)=0 Then fail(c,400,"body is required"):Exit Sub
  Dim chars As Long=0
  For i As Integer=0 To Len(c->body)-1
   If (c->body[i] And &hC0)<>&h80 Then chars+=1
  Next
  If chars>500 Then fail(c,400,"body must be at most 500 characters"):Exit Sub
 End If
 enqueue(c)
End Sub
Sub client_event Cdecl(ByVal fd As Long, ByVal flags As Short, ByVal arg As Any Ptr)
 Dim c As Client Ptr=arg
 If (flags And EV_WRITE)<>0 Then
  While c->sent<Len(c->outgoing)
   Dim n As LongInt=send(fd,StrPtr(c->outgoing)+c->sent,Len(c->outgoing)-c->sent,16384)
   If n<0 Then
    If basic_errno()=11 Then watch_client(c,EV_WRITE):Exit Sub
    destroy_client(c):Exit Sub
   End If
   If n=0 Then Exit Sub
   c->sent+=n
  Wend
  c->outgoing="":c->body="":c->username="":c->userid=""
  watch_client(c,EV_READ)
  If Len(c->incoming)>0 Then process_request(c)
  Exit Sub
 End If
 Dim buf As ZString * 8193
 Dim n As LongInt=recv(fd,@buf,8192,0)
 If n<0 Then
  If basic_errno()=11 Then Exit Sub
  destroy_client(c):Exit Sub
 End If
 If n=0 Then destroy_client(c):Exit Sub
 Dim part As String=String(n,0)
 memcpy(StrPtr(part),@buf,n)
 c->incoming+=part
 process_request(c)
End Sub
Sub accept_event Cdecl(ByVal fd As Long, ByVal flags As Short, ByVal arg As Any Ptr)
 Do
  Dim sock As Long=basic_accept(fd)
  If sock<0 Then Exit Do
  Dim c As Client Ptr=New Client
  c->fd=sock:watch_client(c,EV_READ)
 Loop
End Sub

secret=Environ("JWT_SECRET"):started=ctime_(0)
If Len(secret)=0 Or Len(Environ("DATABASE_URL"))=0 Then Print "DATABASE_URL and JWT_SECRET required":End 1
Dim workers As Long=basic_cpus()
If Len(Environ("BASIC_WORKERS"))>0 Then workers=ValInt(Environ("BASIC_WORKERS"))
If workers<1 Or workers>64 Then End 1
Dim isChild As Long=0
For i As Long=1 To workers
 Dim pid As Long=fork()
 If pid<0 Then End 1
 If pid=0 Then isChild=1:Exit For
Next
If isChild=0 Then
 Dim st As Long
 waitpid(-1,@st,0)
 End 1
End If
basic_signals()
pool=32
If Len(Environ("BASIC_POOL"))>0 Then pool=ValInt(Environ("BASIC_POOL"))
If pool<1 Or pool>128 Or pool*workers>512 Then End 1
eventbase=event_base_new()
sqls(0)="SELECT 1"
Dim select_ As String="SELECT p.id, p.body, p.created_at, u.username, (SELECT count(*) FROM likes l WHERE l.post_id = p.id)::int AS like_count FROM posts p JOIN users u ON u.id = p.user_id"
sqls(1)=select_+" ORDER BY p.created_at DESC, p.id DESC LIMIT 20"
sqls(2)=select_+" WHERE p.id = $1"
sqls(3)="INSERT INTO posts (user_id, body) VALUES ($1, $2) RETURNING id, created_at"
sqls(4)="INSERT INTO likes (user_id, post_id) VALUES ($1, $2) ON CONFLICT (user_id, post_id) DO NOTHING"
Dim url As String=Environ("DATABASE_URL")
For i As Long=0 To pool-1
 slots(i).conn=PQconnectdb(StrPtr(url))
 If PQstatus(slots(i).conn)<>0 Then Print "database unavailable":End 1
 Dim r As Any Ptr=PQexec(slots(i).conn,"SET TIME ZONE 'UTC'"):PQclear(r)
 For j As Long=0 To 4
  Dim nm As String=num(j)
  r=PQprepare(slots(i).conn,StrPtr(nm),StrPtr(sqls(j)),0,0)
  If PQresultStatus(r)<>1 Then Print "prepare failed ";j:End 1
  PQclear(r)
 Next
 PQsetnonblocking(slots(i).conn,1)
Next
Dim host As String=Environ("HOST")
If host="" Then host="0.0.0.0"
Dim primaryPort As String=Environ("PORT"), secondaryPort As String=Environ("EXTRA_PORT")
If primaryPort="" Then primaryPort="80"
If secondaryPort="" Then secondaryPort="81"
Dim portList As String=primaryPort+","+secondaryPort
If Environ("DEV_EXTRA_PORTS")<>"" Then portList+=","+Environ("DEV_EXTRA_PORTS")
Dim portIndex As Long=0
Do
 Dim comma_ As Integer=InStr(portList,",")
 Dim item_ As String
 If comma_>0 Then item_=Left(portList,comma_-1):portList=Mid(portList,comma_+1) Else item_=portList:portList=""
 Dim port_ As Long=ValInt(item_)
 If port_=0 Then port_=80+portIndex
 Dim fd As Long=basic_listener(StrPtr(host),port_)
 If fd<0 Then Print "listen failed ";port_:End 1
 Dim ev As Any Ptr=event_new(eventbase,fd,EV_READ Or EV_PERSIST,@accept_event,0)
 event_add(ev,0)
 portIndex+=1
Loop While Len(portList)>0
Print "GOTO MILLION: FreeBASIC worker ready; pool=";pool
 event_base_dispatch(eventbase)
