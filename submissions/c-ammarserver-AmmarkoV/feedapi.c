/*
 * The million users challenge feed API, in C, on AmmarServer ( https://github.com/AmmarkoV/AmmarServer ),
 * against PostgreSQL through libpq.
 *
 * The process model is prefork : main() forks WORKERS processes and then only waits for them. Each worker
 * is AmmarServer in API mode on one io_uring ring - one thread owning every connection it accepts , and
 * that same thread driving the worker's Postgres connections , so a 4-core run is 4 threads. Half of the
 * workers listen on PORT and half on EXTRA_PORT, all with SO_REUSEPORT, so the kernel spreads the incoming
 * connections evenly over the workers of each port and every worker owns its share for the connection's
 * whole life.
 *
 * A request is a network round trip to the database box, and the ring thread never waits on one. Everything
 * that became ready in one pass over the ring is planned together ( route , token , JSON , id ) ; the pass's
 * reads go to one read connection as one libpq pipeline ended by one Sync , its writes to one write
 * connection as one transaction , and the handler returns. The connections are non-blocking and sit in an
 * epoll set whose descriptor the ring polls ( AmmServer_SetAPINotifyFD ) : when results arrive the ring
 * thread builds the responses and sends them with AmmServer_APISendDeferred. While it is busy the ring waits
 * a couple of milliseconds between passes ( URING_BATCH_WAIT_US , set in build.sh ) , which is what makes a
 * pass a batch : one wake-up , one write to Postgres and one round trip serve everything that arrived
 * meanwhile. Where the kernel cannot set up io_uring , AmmarServer serves from its epoll transport instead ;
 * that transport has no deferred responses , so HandleRequest runs the one statement on a blocking
 * connection and answers before returning. The fast , batched path is the ring one.
 *
 * Everything a worker touches is its own - its ring, its connections, prepared statements, buffers and HMAC
 * key schedule - and all of it is used from one thread , so nothing is locked.
 *
 * Challenge rule 7 ( no caching across requests ) : every response below is built from a Postgres query
 * run while the request is being served, and every token's HMAC is recomputed from scratch. The only
 * things reused between requests are prepared statements ( query plans, explicitly allowed ), the
 * database connections, and the buffers the transport reads and writes through.
 */
#define _GNU_SOURCE

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <errno.h>
#include <endian.h>
#include <signal.h>
#include <time.h>
#include <unistd.h>
#include <sched.h>
#include <sys/prctl.h>
#include <sys/resource.h>
#include <sys/wait.h>
#include <sys/eventfd.h>
#include <sys/epoll.h>
#include <sys/ioctl.h>
#include <netinet/in.h>
#include <netinet/tcp.h>

#include <libpq-fe.h>
#include <isa-l/igzip_lib.h>

#include "AmmServerlib.h"
#include "jwt.h"

/* The feed is 20 posts of up to 500 characters. 500 C0 control characters escape to 6 bytes each, which is
 * the worst case a post body can reach, so 20 * ( 3000 + name + overhead ) has to fit with room to spare. */
#define MAX_RESPONSE_SIZE (192*1024)
/* Has to hold any body AmmarServer will accept ( its API mode caps a request at 32 KB ) so that a body over
 * the 500 character limit can be measured and rejected as too long rather than mistaken for invalid. */
#define MAX_DECODED_BODY_SIZE (32*1024)
#define FEED_POST_COUNT 20

/* One worker per CPU in the affinity mask. A worker's ring holds up to URING_MAX_SLOTS ( 1,048,576 )
 * connections , so 4 workers on 4 cores still have ~4.2M slots for a 1.4M-user run. */
#define MIN_DEFAULT_WORKERS 1
/* Rule 5 : at most 512 connections to Postgres in total. The workers share DEFAULT_TOTAL_DB_CONNECTIONS of
 * them , at least 2 and at most MAX_DB_CONNECTIONS each ( 16 a worker on 4 cores , 8 on 8 , 4 on 16 ) , a
 * quarter of a worker's ( at least one ) for writes so a commit never delays a read. A pass's reads all go
 * to the read connection with the least in flight , so at a steady load one or two of them carry everything
 * and the rest are there for when a round trip is slow or a packet is lost. DB_CONNECTIONS overrides the
 * count per worker. One more per worker is opened only if a request ever has to be answered synchronously
 * ( HandleRequest ) , and DesiredDatabaseConnections leaves room for it within the 512. */
#define DEFAULT_TOTAL_DB_CONNECTIONS 64
#define MAX_DB_CONNECTIONS 16
#define MAX_PG_CONNECTIONS 512
#define MAX_WORKERS 128
/* Statements and Syncs in flight on one connection ; a power of two. A pass that finds every connection of
 * its kind this far behind is answered 500 , so a database that stalls shows up as latency long before it
 * shows up as errors : 2,048 is far more than a second's worth for one connection. */
#define FLIGHT_CAPACITY 2048

/* Only the feed ( 5.1 KB, ~2.2 KB gzipped ) is worth compressing. A single post is ~300 bytes, which gzip's
 * 18 bytes of framing and a near-empty dictionary barely shrink, so below this it goes out as it is. */
#define GZIP_MIN_SIZE 1024

/* Postgres type OIDs for the parameters, and the binary epoch of timestamptz. */
#define INT8OID 20
#define TEXTOID 25
#define POSTGRES_EPOCH_IN_UNIX_DAYS 10957LL
#define SQLSTATE_FOREIGN_KEY_VIOLATION "23503"

/* --- the work behind a request ---------------------------------------------------------------------
 * The ring thread plans every request ( route , token , JSON , id ) , puts the database-bound ones on a
 * Postgres connection and goes back to the ring. A plan waits in its connection's flight ring , in the order
 * its statement was sent , until the result comes back. */

enum Statement
{
  NO_STATEMENT = 0,               /* in a flight ring : the Sync that ends a group of statements */
  STATEMENT_PING,
  STATEMENT_FEED,
  STATEMENT_POST,
  STATEMENT_INSERT_POST,
  STATEMENT_INSERT_LIKE
};
static const char * const statementName[] = { 0 , "ping" , "feed" , "post" , "insert_post" , "insert_like" };

struct PlannedRequest
{
  enum Statement statement;
  int parameters;
  uint64_t parameter[2];          /* the int8 parameters, big endian, libpq's binary format for them */
  long long postId;               /* for the like answer */
  char * body;                    /* POST /posts : malloc'd trimmed body with the username right behind it , freed after its response is built */
  unsigned int bodyLength;
  unsigned int usernameLength;
  unsigned long long deferToken;  /* the transport's handle for this request */
  unsigned char wantGzip;
  PGresult * result;              /* 0 until its result is in ; a plan answered with 0 never reached Postgres */
};

/* One non-blocking connection in pipeline mode. flight holds what has been sent and not yet answered. On a
 * read connection the plan at the head is answered as soon as its result is in. On a write connection the
 * results of a group are kept until the group's Sync comes back , which Postgres sends after the commit
 * ( rule 8 ) : groupDone counts the results in hand at the head of the ring. */
struct DbConnection
{
  PGconn * pg;
  struct PlannedRequest * flight;
  unsigned int head;
  unsigned int count;
  unsigned int groupDone;
  unsigned char isWriter;
  unsigned char groupFailed;
  unsigned char awaitingSeparator; /* a statement's result is in hand and the NULL that ends it is next */
  unsigned char wantsWrite;        /* libpq still holds unsent bytes : EPOLLOUT is being waited for */
  unsigned char leftover;          /* results were left unread in libpq ; look again without waiting for the socket */
  unsigned char broken;            /* to be closed , and its flight answered 500 , by DrainDatabase */
};

static struct DbConnection dbConnection[MAX_DB_CONNECTIONS]; /* the read connections first , then the write ones */
static unsigned int dbConnectionCount = 0;
static unsigned int dbReaderCount = 0;
static int dbEpollFd = -1;        /* every connection's socket , and dbWakeFd ; the ring polls this one descriptor */
static int dbWakeFd = -1;         /* written to have DrainDatabase run again without anything arriving */
static time_t dbRetryAt = 0;      /* a failed connect is not tried again before this */

/* The connection HandleRequest answers on , see there. */
static PGconn * syncPg = 0;
static time_t syncRetryAt = 0;

/* A response that found every send ticket of the transport busy waits here , in order , and nothing more is
 * read from Postgres until it has gone out. */
struct StashedAnswer
{
  struct StashedAnswer * next;
  unsigned long long token;
  unsigned int status;
  unsigned int bodyLength;
  unsigned char gzip;
  char body[];
};
static struct StashedAnswer * stashHead = 0;
static struct StashedAnswer * stashTail = 0;

static char * bodyScratch = 0;    /* one deferred response is built here , then copied by the transport */
static char * gzipScratch = 0;

static const char * databaseUrl = 0;

static char jwtSecret[512];
static unsigned int jwtSecretLength = 0;
static time_t serverStartTime = 0;

/* --- writing the response ---------------------------------------------------------------------------- */

/* Everything here answers through AmmarServer's own response writers ( AmmServer_APIResponseWrite* ) :
 * they append into the body buffer the server handed the handler, bounds checked, and once a write has
 * not fitted the response is marked overflowed and every later write does nothing. So a whole response
 * can be written without checking each step and the buffer still cannot be overrun - the one check is at
 * the end, with AmmServer_APIResponseOverflowed. The only thing added here is this macro, so that a
 * literal's length still comes from sizeof rather than from strlen. */
#define WriteLiteral(res,lit) AmmServer_APIResponseWrite((res),(lit),(unsigned int)(sizeof(lit)-1))

static void Fail(struct AmmServer_APIResponse * res,unsigned int status,const char * message)
{
  /* Throws away whatever had been written : this is called from halfway through building an answer. */
  AmmServer_APIResponseReset(res,status,0);

  WriteLiteral(res,"{\"error\":\"");
  AmmServer_APIResponseWriteText(res,message);
  WriteLiteral(res,"\"}");
}

/* {"id":..,"body":..,"created_at":..,"author":..,"like_count":..} - the key order the spec fixes.
 * authorIsJsonEscaped is for the one case where the author came out of a JWT payload, where it is already
 * JSON string content and escaping it again would double its backslashes ( see jwt.h ). */
/* "00" "01" ... "99" : two digits per lookup instead of two divisions. */
static const char digitPairs[201] =
  "0001020304050607080910111213141516171819202122232425262728293031323334353637383940414243444546474849"
  "5051525354555657585960616263646566676869707172737475767778798081828384858687888990919293949596979899";

/* At most 20 digits ; at must have room for them. The digits are counted first and written straight into
 * place from the end , two at a time , so no copy of variable length follows. */
static char * PutUnsignedDecimal(char * at,unsigned long long value)
{
  unsigned int length=1;
  unsigned long long rest=value;
  while (rest>=10) { rest/=10; ++length; }

  char * end=at+length;
  char * write=end;
  while (value>=100) { unsigned int pair=(unsigned int)(value%100); value/=100; write-=2; memcpy(write,digitPairs+2*pair,2); }
  if (value>=10) { write-=2; memcpy(write,digitPairs+2*value,2); }
  else           { *--write=(char)('0'+value); }
  return end;
}

#define PUT_LITERAL(at,lit) do { memcpy((at),(lit),sizeof(lit)-1); (at)+=sizeof(lit)-1; } while (0)

static void PutPostObject(
                           struct AmmServer_APIResponse * res,
                           long long id,
                           const char * body,unsigned int bodyLength,
                           const char * createdAt,unsigned int createdAtLength,
                           const char * author,unsigned int authorLength,int authorIsJsonEscaped,
                           long long likeCount
                         )
{
  /* The usual case , written straight into the buffer : one room check for the worst this object could
   * take ( every byte of body and author escaped to \u00XX , 20 digits per number ) instead of one per
   * piece. Anything else - a buffer near its end , an overflow already , a negative number - takes the
   * piece by piece path below , which is what decides overflows. */
  unsigned long long worst = 64 + 2*20 + 6ULL*bodyLength + createdAtLength + ( authorIsJsonEscaped ? authorLength : 6ULL*authorLength );
  if ( (!res->overflowed) && (id>=0) && (likeCount>=0) && ( (unsigned long long) res->bodyLength + worst <= res->maxBodyLength ) )
   {
     char * at  = res->body + res->bodyLength;
     char * end = res->body + res->maxBodyLength;
     PUT_LITERAL(at,"{\"id\":");
     at = PutUnsignedDecimal(at,(unsigned long long) id);
     PUT_LITERAL(at,",\"body\":\"");
     at += AmmServer_JSONEscape(body,bodyLength,at,(unsigned int)(end-at));
     PUT_LITERAL(at,"\",\"created_at\":\"");
     memcpy(at,createdAt,createdAtLength); at+=createdAtLength;
     PUT_LITERAL(at,"\",\"author\":\"");
     if (authorIsJsonEscaped) { memcpy(at,author,authorLength); at+=authorLength; }
     else                     { at += AmmServer_JSONEscape(author,authorLength,at,(unsigned int)(end-at)); }
     PUT_LITERAL(at,"\",\"like_count\":");
     at = PutUnsignedDecimal(at,(unsigned long long) likeCount);
     PUT_LITERAL(at,"}");
     res->bodyLength = (unsigned int)( at - res->body );
     return;
   }

  WriteLiteral(res,"{\"id\":");
  AmmServer_APIResponseWriteInteger(res,id);
  WriteLiteral(res,",\"body\":\"");
  AmmServer_APIResponseWriteJSONString(res,body,bodyLength);
  WriteLiteral(res,"\",\"created_at\":\"");
  AmmServer_APIResponseWrite(res,createdAt,createdAtLength);
  WriteLiteral(res,"\",\"author\":\"");
  if (authorIsJsonEscaped) { AmmServer_APIResponseWrite(res,author,authorLength); }
  else                     { AmmServer_APIResponseWriteJSONString(res,author,authorLength); }
  WriteLiteral(res,"\",\"like_count\":");
  AmmServer_APIResponseWriteInteger(res,likeCount);
  WriteLiteral(res,"}");
}

/* --- reading binary results -------------------------------------------------------------------------- */

/* Every statement asks for its results in binary : an int8 is 8 big endian bytes rather than digits to
 * parse, and a timestamptz is microseconds since 2000-01-01 UTC, which is the same whatever the session's
 * TimeZone and DateStyle are - so there is no text timestamp to parse back and nothing to SET first. */
static long long Int8At(const PGresult * r,int row,int column)
{
  uint64_t value; memcpy(&value,PQgetvalue(r,row,column),sizeof(value));
  return (long long) be64toh(value);
}

static long long Int4At(const PGresult * r,int row,int column)
{
  uint32_t value; memcpy(&value,PQgetvalue(r,row,column),sizeof(value));
  return (long long)(int32_t) be32toh(value);
}

/* A fixed number of decimal digits, zero padded, written straight in. */
static char * PutDigits(char * at,unsigned int value,unsigned int digits)
{
  unsigned int i=digits;
  while (i>0) { --i; at[i] = (char)( '0' + value%10 ); value/=10; }
  return at+digits;
}

/* 2025-12-31T23:59:19.409Z : UTC, truncated ( not rounded ) to milliseconds, as the spec asks.
 * Written by hand rather than with gmtime_r + snprintf, since a feed formats 20 of them : the date is
 * Howard Hinnant's civil_from_days, exact for the whole proleptic Gregorian calendar, and only a year past
 * 9999 ( which no row can have ) would come out wrong. */
/* The date part is the same for most posts of a feed , so each thread keeps the last day it wrote. */
static __thread long long cachedDay = -1;
static __thread int cachedDayValid = 0;
static __thread char cachedDate[10];

static unsigned int TimestampAt(const PGresult * r,int row,int column,char out[32])
{
  long long microseconds = Int8At(r,row,column);
  long long seconds   = microseconds / 1000000;
  long long remainder = microseconds % 1000000;
  if (remainder<0) { remainder+=1000000; --seconds; } /* floor, for the ( never seen ) dates before 2000 */

  long long days = seconds / 86400;
  long long secondOfDay = seconds % 86400;
  if (secondOfDay<0) { secondOfDay+=86400; --days; }

  if ( (!cachedDayValid) || (days!=cachedDay) )
   {

  /* civil_from_days counts eras of 400 years from 0000-03-01, so that a leap day is the last of its year. */
  long long z   = days + POSTGRES_EPOCH_IN_UNIX_DAYS + 719468;
  long long era = ( (z>=0) ? z : z-146096 ) / 146097;
  unsigned int dayOfEra  = (unsigned int)( z - era*146097 );
  unsigned int yearOfEra = ( dayOfEra - dayOfEra/1460 + dayOfEra/36524 - dayOfEra/146096 ) / 365;
  unsigned int dayOfYear = dayOfEra - ( 365*yearOfEra + yearOfEra/4 - yearOfEra/100 );
  unsigned int monthFromMarch = ( 5*dayOfYear + 2 ) / 153;
  unsigned int day   = dayOfYear - ( 153*monthFromMarch + 2 )/5 + 1;
  unsigned int month = (monthFromMarch<10) ? monthFromMarch+3 : monthFromMarch-9;
  long long year = (long long) yearOfEra + era*400 + ( (month<=2) ? 1 : 0 );

  char * date = cachedDate;
  date = PutDigits(date,(unsigned int) year,4);   *date++ = '-';
  date = PutDigits(date,month,2);                 *date++ = '-';
  PutDigits(date,day,2);
  cachedDay=days;
  cachedDayValid=1;
   }

  unsigned int second = (unsigned int) secondOfDay;
  unsigned int millisecond = (unsigned int)(remainder/1000);
  char * at = out;
  memcpy(at,cachedDate,10); at+=10;               *at++ = 'T';
  memcpy(at,digitPairs+2*(second/3600),2);        at+=2; *at++ = ':';
  memcpy(at,digitPairs+2*((second/60)%60),2);     at+=2; *at++ = ':';
  memcpy(at,digitPairs+2*(second%60),2);          at+=2; *at++ = '.';
  memcpy(at,digitPairs+2*(millisecond/10),2);     at+=2;
  *at++ = (char)('0'+millisecond%10);
  *at++ = 'Z';
  return (unsigned int)( at-out );
}

/* One row of the feed/post query, which both select the same five columns in the same order. */
static void PutPostRow(struct AmmServer_APIResponse * res,const PGresult * r,int row)
{
  char createdAt[32];
  unsigned int createdAtLength = TimestampAt(r,row,2,createdAt);

  PutPostObject(
                 res,
                 Int8At(r,row,0),
                 PQgetvalue(r,row,1),(unsigned int) PQgetlength(r,row,1),
                 createdAt,createdAtLength,
                 PQgetvalue(r,row,3),(unsigned int) PQgetlength(r,row,3),0,
                 Int4At(r,row,4)
               );
}

/* --- the database ------------------------------------------------------------------------------------ */

/* The spec's queries, verbatim ( rule 3 ) : the two reads share their column list and their order, which
 * is what lets PutPostRow serve both. */
#define SELECT_POST_COLUMNS \
  "SELECT p.id, p.body, p.created_at, u.username, " \
  "(SELECT count(*) FROM likes l WHERE l.post_id = p.id)::int AS like_count " \
  "FROM posts p JOIN users u ON u.id = p.user_id "

static int Prepare(PGconn * pg,const char * name,const char * sql,int parameters,const Oid * types)
{
  PGresult * r = PQprepare(pg,name,sql,parameters,types);
  int ok = (PQresultStatus(r)==PGRES_COMMAND_OK);
  if (!ok) { fprintf(stderr,"feedapi: could not prepare [%s] : %s",sql,PQerrorMessage(pg)); }
  PQclear(r);
  return ok;
}

static int PrepareStatements(PGconn * pg)
{
  static const Oid oneInt8[1]      = { INT8OID };
  static const Oid twoInt8[2]      = { INT8OID , INT8OID };
  static const Oid int8AndText[2]  = { INT8OID , TEXTOID };

  /* posts_created_at_id_idx is (created_at DESC, id DESC), so this is 20 rows off the front of that index
   * rather than a sort of half a million posts. */
  if (!Prepare(pg,statementName[STATEMENT_FEED],SELECT_POST_COLUMNS "ORDER BY p.created_at DESC, p.id DESC LIMIT 20",0,0)) { return 0; }
  if (!Prepare(pg,statementName[STATEMENT_POST],SELECT_POST_COLUMNS "WHERE p.id = $1",1,oneInt8)) { return 0; }
  if (!Prepare(pg,statementName[STATEMENT_INSERT_POST],"INSERT INTO posts (user_id, body) VALUES ($1, $2) RETURNING id, created_at",2,int8AndText)) { return 0; }
  if (!Prepare(pg,statementName[STATEMENT_INSERT_LIKE],"INSERT INTO likes (user_id, post_id) VALUES ($1, $2) ON CONFLICT (user_id, post_id) DO NOTHING",2,twoInt8)) { return 0; }
  if (!Prepare(pg,statementName[STATEMENT_PING],"SELECT 1",0,0)) { return 0; }
  return 1;
}

/* A blocking connect with its statements prepared , or 0. The attempt is bounded : a database that is down
 * must fail requests , not leave the thread waiting on the kernel's TCP timeout. */
static PGconn * ConnectDatabase(void)
{
  static const char * const keywords[] = { "dbname" , "connect_timeout" , 0 };
  const char * const values[]         = { databaseUrl , "3" , 0 };

  PGconn * pg = PQconnectdbParams(keywords,values,1);
  if (pg==0) { return 0; }

  if ( (PQstatus(pg)!=CONNECTION_OK) || (PQsetClientEncoding(pg,"UTF8")!=0) || (!PrepareStatements(pg)) )
   {
     PQfinish(pg);
     return 0;
   }

  /* A batch is a packet or two each way , never a stream that fast retransmit could repair , so a lost one
   * waits for the retransmission timer : linear timeouts retry it every RTO instead of doubling each time.
   * A setting of this process's own socket ( rule 13 ). */
  int yes=1;
  setsockopt(PQsocket(pg),IPPROTO_TCP,TCP_THIN_LINEAR_TIMEOUTS,&yes,sizeof(yes));
  return pg;
}

/* --- request helpers -------------------------------------------------------------------------------- */

/**
 * Verify the request's bearer token, answering with the right 401 if it does not hold up. claims->username
 * points into scratch , so both have to outlive whatever the caller does with it.
 * @retval 1=authenticated , 0=res now holds the 401 to send
 */
static int Authenticate(
                         struct AmmServer_APIRequest * rqst,
                         struct AmmServer_APIResponse * res,
                         struct JwtClaims * claims,
                         char * scratch,
                         unsigned int scratchSize
                       )
{
  unsigned int tokenLength=0;
  const char * token = AmmServer_APIRequestBearer(rqst,&tokenLength);

  /* No Authorization header at all, or one whose scheme is not Bearer. A Bearer header with nothing after
   * it comes back non-zero with length 0 instead, and is a broken credential rather than a missing one. */
  if (token==0) { Fail(res,401,"missing bearer token"); return 0; }

  switch ( JwtVerifyToken(token,tokenLength,jwtSecret,jwtSecretLength,(long long) time(0),claims,scratch,scratchSize) )
   {
     case JWT_OK          : return 1;
     case JWT_INVALID     : Fail(res,401,"invalid or expired token"); return 0;
     case JWT_BAD_PAYLOAD : Fail(res,401,"invalid token payload");    return 0;
   };

  Fail(res,401,"invalid or expired token");
  return 0;
}

static void TrimAsciiWhitespace(const char ** s,unsigned int * length)
{
  const char * start = *s;
  unsigned int n = *length;

  while ( (n>0) && ( (*start==' ')||(*start=='\t')||(*start=='\n')||(*start=='\r')||(*start=='\v')||(*start=='\f') ) ) { ++start; --n; }
  while ( (n>0) && ( (start[n-1]==' ')||(start[n-1]=='\t')||(start[n-1]=='\n')||(start[n-1]=='\r')||(start[n-1]=='\v')||(start[n-1]=='\f') ) ) { --n; }

  *s = start;
  *length = n;
}

/* Characters, not bytes : the schema's own CHECK is char_length(body) BETWEEN 1 AND 500 , so counting
 * bytes here would reject a legitimate 500 character body with accents in it. */
static unsigned int Utf8CharacterCount(const char * s,unsigned int n)
{
  unsigned int count=0;
  unsigned int i=0;
  for (i=0; i<n; i++) { if ( ((unsigned char) s[i] & 0xC0) != 0x80 ) { ++count; } }
  return count;
}

/* --- planning a request ------------------------------------------------------------------------------ */

/* Everything that needs no database comes first : the route, the token, the body, the id. A request that
 * fails any of those is answered right there ; the rest become a PlannedRequest whose statement goes to
 * Postgres and whose response is sent when the result is back. The plan is self-contained : the request's
 * bytes are only promised until the handler returns, so the body and the username are copied into it. */
static struct PlannedRequest planScratch[AMMSERVER_API_MAX_BATCH];

static void Plan(struct PlannedRequest * plan,enum Statement statement,int parameters,long long first,long long second)
{
  plan->statement    = statement;
  plan->parameters   = parameters;
  plan->parameter[0] = htobe64((uint64_t) first);
  plan->parameter[1] = htobe64((uint64_t) second);
}

static char decodedBodyScratch[MAX_DECODED_BODY_SIZE+1]; /* ring thread only */

static void PlanCreatePost(struct AmmServer_APIRequest * rqst,struct AmmServer_APIResponse * res,struct PlannedRequest * plan)
{
  char claimsScratch[1024];
  struct JwtClaims claims;
  if (!Authenticate(rqst,res,&claims,claimsScratch,sizeof(claimsScratch))) { return; }

  unsigned int decodedLength=0;
  switch ( AmmServer_JSONGetStringField(rqst->body,rqst->bodyLength,"body",decodedBodyScratch,sizeof(decodedBodyScratch),&decodedLength) )
   {
     case AMMSERVER_JSON_MALFORMED          : Fail(res,400,"malformed JSON body"); return;
     case AMMSERVER_JSON_FIELD_NOT_A_STRING : Fail(res,400,"body is required");    return;
     case AMMSERVER_JSON_FIELD_OK           : break;
   };

  /* Longer than the buffer means longer than any 500 character body could ever be. */
  if (decodedLength>=sizeof(decodedBodyScratch)) { Fail(res,400,"body must be at most 500 characters"); return; }

  const char * body = decodedBodyScratch;
  unsigned int bodyLength = decodedLength;
  TrimAsciiWhitespace(&body,&bodyLength);

  if (bodyLength==0) { Fail(res,400,"body is required"); return; }
  if (Utf8CharacterCount(body,bodyLength)>500) { Fail(res,400,"body must be at most 500 characters"); return; }

  /* The plan outlives the request buffer, so its body and the author's name get their own copy. Only
   * POST /posts pays this. */
  plan->body = (char *) malloc(bodyLength+claims.usernameLength);
  if (plan->body==0) { Fail(res,500,"internal server error"); return; }
  memcpy(plan->body,body,bodyLength);
  memcpy(plan->body+bodyLength,claims.username,claims.usernameLength);
  plan->bodyLength = bodyLength;
  plan->usernameLength = claims.usernameLength;
  Plan(plan,STATEMENT_INSERT_POST,2,claims.userId,0); /* the second parameter is the body, as text */
}

#define POSTS_PREFIX     "/posts/"
#define POSTS_PREFIX_LEN 7
#define LIKE_SUFFIX      "/like"
#define LIKE_SUFFIX_LEN  5

/* AmmServer_APIRequestMatch does the path matching for the two routes that carry an id : a template is the
 * whole path or it is not a match, and %lld only matches a run of digits that is a positive number and
 * cannot overflow, so "abc", "-1", "0" and "1.5" are not matches rather than ids parsed loosely.
 *
 * What it deliberately cannot say is "any path at all under this prefix", since every template is anchored
 * at both ends - and the spec needs exactly that: /posts/<anything unusable> is a post request with a bad
 * id and is answered 400, where a path that is not about posts at all is a 404. So the prefix tests below
 * stay, and what they guard is the classification, not the routing. */
static void PlanRequest(struct AmmServer_APIRequest * rqst,struct AmmServer_APIResponse * res,struct PlannedRequest * plan)
{
  long long postId=0;

  plan->statement = NO_STATEMENT;
  plan->result = 0;
  plan->body = 0;
  plan->bodyLength = 0;
  plan->wantGzip = 0;
  plan->deferToken = 0;
  plan->usernameLength = 0;

  if (rqst->method==GET)
   {
     if (AmmServer_APIRequestMatch(rqst,"/feed"))   { Plan(plan,STATEMENT_FEED,0,0,0); return; }
     if (AmmServer_APIRequestMatch(rqst,"/health")) { Plan(plan,STATEMENT_PING,0,0,0); return; }

     if (AmmServer_APIRequestMatch(rqst,"/posts/%lld",&postId)) { Plan(plan,STATEMENT_POST,1,postId,0); return; }

     if ( (rqst->pathLength>POSTS_PREFIX_LEN) && (memcmp(rqst->path,POSTS_PREFIX,POSTS_PREFIX_LEN)==0) )
      { Fail(res,400,"invalid post id"); return; }
   }
  else
  if (rqst->method==POST)
   {
     if (AmmServer_APIRequestMatch(rqst,"/posts")) { PlanCreatePost(rqst,res,plan); return; }

     if ( (rqst->pathLength>POSTS_PREFIX_LEN+LIKE_SUFFIX_LEN) &&
          (memcmp(rqst->path,POSTS_PREFIX,POSTS_PREFIX_LEN)==0) &&
          (memcmp(rqst->path+rqst->pathLength-LIKE_SUFFIX_LEN,LIKE_SUFFIX,LIKE_SUFFIX_LEN)==0) )
      {
        /* Auth before the id , which the spec is explicit about : POST /posts/abc/like without a token is a
         * 401 and only becomes a 400 once there is a valid one. */
        char claimsScratch[1024];
        struct JwtClaims claims;
        if (!Authenticate(rqst,res,&claims,claimsScratch,sizeof(claimsScratch))) { return; }

        if (!AmmServer_APIRequestMatch(rqst,"/posts/%lld/like",&postId)) { Fail(res,400,"invalid post id"); return; }

        plan->postId = postId;
        Plan(plan,STATEMENT_INSERT_LIKE,2,claims.userId,postId);
        return;
      }
   }

  Fail(res,404,"not found");
}

static void AnswerHealth(struct AmmServer_APIResponse * res,const PGresult * r)
{
  /* The spec wants the ok answer to mean a SELECT 1 actually came back, so it is run per request. */
  if ( (r==0) || (PQresultStatus(r)!=PGRES_TUPLES_OK) )
   {
     const char * message = (r!=0) ? PQresultErrorMessage(r) : "the connection to the database was lost";
     unsigned int messageLength = (unsigned int) strlen(message);
     while ( (messageLength>0) && (message[messageLength-1]=='\n') ) { --messageLength; } /* libpq ends with one */

     AmmServer_APIResponseReset(res,503,0);
     WriteLiteral(res,"{\"status\":\"degraded\",\"db\":\"unreachable\",\"error\":\"");
     AmmServer_APIResponseWriteJSONString(res,message,messageLength);
     WriteLiteral(res,"\"}");
     return;
   }

  res->statusCode = 200;
  WriteLiteral(res,"{\"status\":\"ok\",\"db\":\"ok\",\"uptime_s\":");
  AmmServer_APIResponseWriteInteger(res,(long long)( time(0)-serverStartTime ));
  WriteLiteral(res,"}");
}

static void AnswerFeed(struct AmmServer_APIResponse * res,const PGresult * r)
{
  if (PQresultStatus(r)!=PGRES_TUPLES_OK) { Fail(res,500,"internal server error"); return; }

  WriteLiteral(res,"{\"posts\":[");
  int rows = PQntuples(r);
  int row=0;
  for (row=0; row<rows; row++)
   {
     if (row>0) { WriteLiteral(res,","); }
     PutPostRow(res,r,row);
   }
  WriteLiteral(res,"]}");

  if (AmmServer_APIResponseOverflowed(res)) { Fail(res,500,"internal server error"); return; }

  res->statusCode = 200;
}

static void AnswerPost(struct AmmServer_APIResponse * res,const PGresult * r)
{
  if (PQresultStatus(r)!=PGRES_TUPLES_OK) { Fail(res,500,"internal server error"); return; }
  if (PQntuples(r)==0)                    { Fail(res,404,"post not found"); return; }

  WriteLiteral(res,"{\"post\":");
  PutPostRow(res,r,0);
  WriteLiteral(res,"}");

  if (AmmServer_APIResponseOverflowed(res)) { Fail(res,500,"internal server error"); return; }

  res->statusCode = 200;
}

static void AnswerCreatePost(struct AmmServer_APIResponse * res,const struct PlannedRequest * plan)
{
  /* Its own transaction, ended by its own Sync before its result was read, so the 201 below means the row
   * is committed ( rule 8 ). */
  const PGresult * r = plan->result;
  if ( (PQresultStatus(r)!=PGRES_TUPLES_OK) || (PQntuples(r)!=1) ) { Fail(res,500,"internal server error"); return; }

  char createdAt[32];
  unsigned int createdAtLength = TimestampAt(r,0,1,createdAt);

  WriteLiteral(res,"{\"post\":");
  PutPostObject(res,Int8At(r,0,0),plan->body,plan->bodyLength,createdAt,createdAtLength,plan->body+plan->bodyLength,plan->usernameLength,1,0);
  WriteLiteral(res,"}");

  if (AmmServer_APIResponseOverflowed(res)) { Fail(res,500,"internal server error"); return; }

  res->statusCode = 201;
}

static void AnswerLike(struct AmmServer_APIResponse * res,const struct PlannedRequest * plan)
{
  const PGresult * r = plan->result;

  if (PQresultStatus(r)!=PGRES_COMMAND_OK)
   {
     /* A like on a post that does not exist is the foreign key on likes.post_id failing, as the spec says. */
     const char * sqlState = (r!=0) ? PQresultErrorField(r,PG_DIAG_SQLSTATE) : 0;
     if ( (sqlState!=0) && (strcmp(sqlState,SQLSTATE_FOREIGN_KEY_VIOLATION)==0) ) { Fail(res,404,"post not found"); }
     else { Fail(res,500,"internal server error"); }
     return;
   }

  /* "INSERT 0 1" or, when ON CONFLICT DO NOTHING kept the like that was already there, "INSERT 0 0". */
  int inserted = (strcmp(PQcmdTuples((PGresult *) r),"1")==0);

  WriteLiteral(res,"{\"liked\":true,\"already_liked\":");
  if (inserted) { WriteLiteral(res,"false"); } else { WriteLiteral(res,"true"); }
  WriteLiteral(res,",\"post_id\":");
  AmmServer_APIResponseWriteInteger(res,plan->postId);
  WriteLiteral(res,"}");

  res->statusCode = inserted ? 201 : 200;
}

/* --- gzip ------------------------------------------------------------------------------------------- */

/* Read while the request bytes are still in hand : the plan outlives them. */
static int AcceptsGzip(const struct AmmServer_APIRequest * rqst)
{
  unsigned int valueLength=0;
  const char * value = AmmServer_APIRequestHeader(rqst,"Accept-Encoding",&valueLength);
  return ( (value!=0) && (memmem(value,valueLength,"gzip",4)!=0) );
}

/* ISA-L igzip at level 0 : one hash lookup per position and no lazy matching, which is the fastest setting
 * and enough for a feed that goes out as two TCP segments either way. Stateless, into the worker's scratch
 * buffer, and only kept when it actually made the response smaller. */
static void GzipResponse(struct AmmServer_APIResponse * res)
{
  if (gzipScratch==0) { return; }

  struct isal_zstream stream;
  isal_deflate_stateless_init(&stream);
  stream.next_in   = (uint8_t *) res->body;
  stream.avail_in  = res->bodyLength;
  stream.next_out  = (uint8_t *) gzipScratch;
  stream.avail_out = (uint32_t) MAX_RESPONSE_SIZE;
  stream.gzip_flag = IGZIP_GZIP;
  stream.level     = 0;

  if (isal_deflate_stateless(&stream)!=COMP_OK) { return; }

  size_t compressedLength = (size_t) MAX_RESPONSE_SIZE - stream.avail_out;
  if ( (compressedLength==0) || (compressedLength>=res->bodyLength) ) { return; }

  memcpy(res->body,gzipScratch,compressedLength);
  res->bodyLength   = (unsigned int) compressedLength;
  res->extraHeaders = "Content-Encoding: gzip\r\n";
}

/* --- answering a plan ------------------------------------------------------------------------------ */

/* The response for a plan whose result is in ( or is 0 : it never reached Postgres , or its connection was
 * lost , which /health says with its 503 and everything else with a 500 ). */
static void BuildAnswer(struct AmmServer_APIResponse * res,const struct PlannedRequest * plan)
{
  switch (plan->statement)
   {
     case STATEMENT_PING        : AnswerHealth(res,plan->result); break;
     case STATEMENT_FEED        : if (plan->result==0) { Fail(res,500,"internal server error"); } else { AnswerFeed(res,plan->result); } break;
     case STATEMENT_POST        : if (plan->result==0) { Fail(res,500,"internal server error"); } else { AnswerPost(res,plan->result); } break;
     case STATEMENT_INSERT_POST : if (plan->result==0) { Fail(res,500,"internal server error"); } else { AnswerCreatePost(res,plan); } break;
     case STATEMENT_INSERT_LIKE : if (plan->result==0) { Fail(res,500,"internal server error"); } else { AnswerLike(res,plan); } break;
     case NO_STATEMENT          : Fail(res,500,"internal server error"); break;
   };

  if (AmmServer_APIResponseOverflowed(res)) { Fail(res,500,"internal server error"); }

  if ( (plan->wantGzip) && (res->bodyLength>=GZIP_MIN_SIZE) ) { GzipResponse(res); }
}

static void ReleasePlan(struct PlannedRequest * plan)
{
  if (plan->result!=0) { PQclear(plan->result); plan->result=0; }
  if (plan->body!=0)   { free(plan->body); plan->body=0; }
}

/* Has DrainDatabase run again on the ring's next turn , whether or not anything arrives from Postgres. */
static void WakeDatabase(void)
{
  uint64_t one=1;
  ssize_t written=write(dbWakeFd,&one,sizeof(one));
  (void) written;
}

static int SendStashed(struct AmmServer_Instance * instance)
{
  while (stashHead!=0)
   {
     struct StashedAnswer * answer=stashHead;
     if (!AmmServer_APISendDeferred(instance,answer->token,answer->status,"application/json",
                                    (answer->gzip!=0) ? "Content-Encoding: gzip\r\n" : 0,answer->body,answer->bodyLength))
      { return 0; }
     stashHead=answer->next;
     if (stashHead==0) { stashTail=0; }
     free(answer);
   }
  return 1;
}

/* Builds a plan's response and sends it , from the ring thread. If every send ticket is busy the response is
 * kept , in order , for DrainDatabase's next run. */
static void Answer(struct AmmServer_Instance * instance,struct PlannedRequest * plan)
{
  struct AmmServer_APIResponse res;
  memset(&res,0,sizeof(res));
  res.statusCode    = 200;
  res.contentType   = "application/json";
  res.body          = bodyScratch;
  res.maxBodyLength = MAX_RESPONSE_SIZE;

  BuildAnswer(&res,plan);

  if ( (stashHead!=0) ||
       (!AmmServer_APISendDeferred(instance,plan->deferToken,res.statusCode,"application/json",res.extraHeaders,res.body,res.bodyLength)) )
   {
     struct StashedAnswer * answer=(struct StashedAnswer *) malloc(sizeof(*answer)+res.bodyLength);
     if (answer!=0)
      {
        answer->next       = 0;
        answer->token      = plan->deferToken;
        answer->status     = res.statusCode;
        answer->bodyLength = res.bodyLength;
        answer->gzip       = (res.extraHeaders!=0);
        memcpy(answer->body,res.body,res.bodyLength);
        if (stashTail!=0) { stashTail->next=answer; } else { stashHead=answer; }
        stashTail=answer;
        WakeDatabase();
      }
   }

  ReleasePlan(plan);
}

/* --- the connections ------------------------------------------------------------------------------- */

/* Did this statement produce the result its plan needs? A read is satisfied by a tuple set ( including an
 * empty one , which is the 404 case for GET /posts/:id ) ; an insert post needs its one RETURNING row , and
 * a like only a command tag. PGRES_PIPELINE_ABORTED falls through to 0. */
static int PlanSucceeded(const struct PlannedRequest * plan)
{
  if (plan->result==0) { return 0; }

  switch (plan->statement)
   {
     case STATEMENT_INSERT_POST : return (PQresultStatus(plan->result)==PGRES_TUPLES_OK) && (PQntuples(plan->result)==1);
     case STATEMENT_INSERT_LIKE : return (PQresultStatus(plan->result)==PGRES_COMMAND_OK);
     case STATEMENT_FEED :
     case STATEMENT_POST :
     case STATEMENT_PING        : return (PQresultStatus(plan->result)==PGRES_TUPLES_OK);
     default                    : return 0;
   }
}

static void PlanParameters(const struct PlannedRequest * plan,const char * values[2],int lengths[2])
{
  values[0]  = (const char *) &plan->parameter[0];
  values[1]  = (const char *) &plan->parameter[1];
  lengths[0] = sizeof(uint64_t);
  lengths[1] = sizeof(uint64_t);

  /* The body goes in binary too, which for text is just its bytes and a length, so it never has to be
   * NUL terminated. */
  if (plan->statement==STATEMENT_INSERT_POST) { values[1] = plan->body; lengths[1] = (int) plan->bodyLength; }
}

static const int binaryParameters[2] = { 1 , 1 };

/* Opens one of the worker's connections : non-blocking , in pipeline mode for good , and in the epoll set. */
static int OpenConnection(struct DbConnection * c)
{
  PGconn * pg = ConnectDatabase();
  if (pg==0) { return 0; }

  struct epoll_event ev;
  memset(&ev,0,sizeof(ev));
  ev.events   = EPOLLIN;
  ev.data.ptr = c;

  if ( (PQsetnonblocking(pg,1)!=0) || (!PQenterPipelineMode(pg)) || (epoll_ctl(dbEpollFd,EPOLL_CTL_ADD,PQsocket(pg),&ev)!=0) )
   {
     PQfinish(pg);
     return 0;
   }

  c->pg = pg;
  c->head = 0; c->count = 0; c->groupDone = 0;
  c->groupFailed = 0; c->awaitingSeparator = 0; c->wantsWrite = 0; c->leftover = 0; c->broken = 0;
  return 1;
}

/* A connection that is down is opened again , so that a database that comes back is served again. The
 * connect blocks this worker , so after one fails nothing is tried for a second. */
static void EnsureConnections(void)
{
  unsigned int i=0;
  for (i=0; i<dbConnectionCount; i++)
   {
     if (dbConnection[i].pg!=0) { continue; }

     time_t now=time(0);
     if (now<dbRetryAt) { return; }
     if (!OpenConnection(&dbConnection[i])) { dbRetryAt=now+1; return; }
   }
}

static void MarkBroken(struct DbConnection * c)
{
  c->broken=1;
  WakeDatabase();
}

/* Hands libpq's output buffer to the socket. What does not fit waits for EPOLLOUT. */
static void FlushConnection(struct DbConnection * c)
{
  int pending=PQflush(c->pg);
  if (pending<0) { MarkBroken(c); return; }

  unsigned char wantsWrite=(pending>0);
  if (wantsWrite!=c->wantsWrite)
   {
     struct epoll_event ev;
     memset(&ev,0,sizeof(ev));
     ev.events   = wantsWrite ? (EPOLLIN|EPOLLOUT) : EPOLLIN;
     ev.data.ptr = c;
     epoll_ctl(dbEpollFd,EPOLL_CTL_MOD,PQsocket(c->pg),&ev);
     c->wantsWrite=wantsWrite;
   }
}

static struct PlannedRequest * FlightPush(struct DbConnection * c)
{
  struct PlannedRequest * slot=&c->flight[(c->head+c->count)&(FLIGHT_CAPACITY-1)];
  ++c->count;
  return slot;
}

/* The connection of one kind with the least in flight , that has room for `needed` statements and their
 * Sync. 0 if there is none. */
static struct DbConnection * PickConnection(unsigned int first,unsigned int end,unsigned int needed)
{
  struct DbConnection * best=0;
  unsigned int i=0;
  for (i=first; i<end; i++)
   {
     struct DbConnection * c=&dbConnection[i];
     if ( (c->pg==0) || (c->broken) || (c->count+needed+1>FLIGHT_CAPACITY) ) { continue; }
     if ( (best==0) || (c->count<best->count) ) { best=c; }
   }
  return best;
}

/* Every statement of a group is followed by a single Sync. That Sync is one round trip and one implicit
 * transaction : reads share the transaction ( each statement still takes its own READ COMMITTED snapshot ,
 * so it sees commits made before it ran ), and the writes of a group commit together ( rule 3 allows
 * batching writes into one transaction ; rule 8 is still kept , every response is held until that commit's
 * Sync is back ). A plan that cannot be sent is answered where it stands , in the response the transport
 * prepared for it. */
static void SendGroup(
                       struct DbConnection * c,
                       struct AmmServer_APIResponse * responses,
                       struct PlannedRequest * plans,
                       const unsigned int * which,
                       unsigned int count
                     )
{
  unsigned int sent=0;
  unsigned int i=0;
  for (i=0; i<count; i++)
   {
     struct PlannedRequest * plan=&plans[which[i]];
     const char * values[2];
     int lengths[2];
     PlanParameters(plan,values,lengths);

     if ( (c==0) || (c->broken) ||
          (!PQsendQueryPrepared(c->pg,statementName[plan->statement],plan->parameters,values,lengths,binaryParameters,1)) )
      {
        if ( (c!=0) && (!c->broken) ) { MarkBroken(c); }
        BuildAnswer(&responses[which[i]],plan);
        ReleasePlan(plan);
        continue;
      }

     *FlightPush(c)=*plan;
     responses[which[i]].deferred=1;
     ++sent;
   }

  if (sent==0) { return; }

  /* Without its Sync nothing sent above can be told apart from what follows , so the connection goes , and
   * DrainDatabase answers what is in its flight. */
  if (!PQpipelineSync(c->pg)) { MarkBroken(c); return; }
  FlightPush(c)->statement=NO_STATEMENT;
  FlushConnection(c);
}

/* A write whose group was rolled back , sent again as a transaction of its own. 0 if it could not be sent ,
 * in which case nothing of it is in flight. */
static int SendAlone(struct DbConnection * c,const struct PlannedRequest * plan)
{
  if ( (c->broken) || (c->count+2>FLIGHT_CAPACITY) ) { return 0; }

  const char * values[2];
  int lengths[2];
  PlanParameters(plan,values,lengths);
  if (!PQsendQueryPrepared(c->pg,statementName[plan->statement],plan->parameters,values,lengths,binaryParameters,1)) { MarkBroken(c); return 0; }

  *FlightPush(c)=*plan;
  if (!PQpipelineSync(c->pg)) { MarkBroken(c); return 1; }
  FlightPush(c)->statement=NO_STATEMENT;
  return 1;
}

/* The Sync at the head of a connection's unanswered flight has come back. On a read connection every plan
 * before it was answered as its result arrived. On a write connection the group's transaction has now
 * committed , or rolled back : a committed group is answered ; a group that one statement aborted ( a like
 * on a missing post ) was rolled back whole , so each of its writes is sent again in a transaction of its
 * own - the valid ones commit and the failing one only fails itself. A write that was already alone is
 * answered with whatever it got. */
static void EndGroup(struct AmmServer_Instance * instance,struct DbConnection * c)
{
  struct PlannedRequest group[AMMSERVER_API_MAX_BATCH];
  unsigned int count=c->groupDone;
  int replay=( (c->groupFailed) && (count>1) );

  unsigned int i=0;
  for (i=0; i<count; i++) { group[i]=c->flight[(c->head+i)&(FLIGHT_CAPACITY-1)]; }

  c->head=(c->head+count+1)&(FLIGHT_CAPACITY-1);
  c->count-=count+1;
  c->groupDone=0;
  c->groupFailed=0;

  for (i=0; i<count; i++)
   {
     if (replay)
      {
        PQclear(group[i].result);
        group[i].result=0;
        if (SendAlone(c,&group[i])) { continue; }
      }
     Answer(instance,&group[i]);
   }

  if ( (replay) && (!c->broken) ) { FlushConnection(c); }
}

/* Answers whatever libpq already holds complete results for. 0 if the connection has to go , or if a
 * response could not be handed to the transport and the rest has to wait for the next run. */
static int TakeResults(struct AmmServer_Instance * instance,struct DbConnection * c)
{
  while (c->count>c->groupDone)
   {
     if (stashHead!=0) { c->leftover=1; WakeDatabase(); return 0; }
     if (PQisBusy(c->pg)) { break; }

     struct PlannedRequest * plan=&c->flight[(c->head+c->groupDone)&(FLIGHT_CAPACITY-1)];
     PGresult * r=PQgetResult(c->pg);

     if (plan->statement==NO_STATEMENT)
      {
        int synced=( (r!=0) && (PQresultStatus(r)==PGRES_PIPELINE_SYNC) );
        PQclear(r);
        if (!synced) { c->broken=1; return 0; }
        EndGroup(instance,c);
        continue;
      }

     /* Per statement : its result , then the NULL that ends it. */
     if (!c->awaitingSeparator)
      {
        if (r==0) { c->broken=1; return 0; }
        plan->result=r;
        c->awaitingSeparator=1;
        continue;
      }
     if (r!=0) { PQclear(r); continue; }
     c->awaitingSeparator=0;

     if (c->isWriter)
      {
        if (!PlanSucceeded(plan)) { c->groupFailed=1; }
        ++c->groupDone;
      }
     else
      {
        c->head=(c->head+1)&(FLIGHT_CAPACITY-1);
        --c->count;
        Answer(instance,plan); /* the slot is not reused before this returns : nothing is sent from here */
      }
   }

  return 1;
}

/* Takes what has arrived on a connection and answers what it completes. One PQconsumeInput is one read of
 * about 16 KB , some ten feeds , and this runs once per pass of the ring : so the socket is read until it is
 * empty , or a connection could never deliver more than that per pass however fast Postgres answered , with
 * its backend waiting on a full socket in between. */
static void ReadResults(struct AmmServer_Instance * instance,struct DbConnection * c)
{
  c->leftover=0;

  int waiting=0;
  do
   {
     if (!PQconsumeInput(c->pg)) { c->broken=1; return; }
     if (!TakeResults(instance,c)) { return; }
     if (c->broken) { return; } /* a replayed write could not be sent */
     waiting=0;
     if (ioctl(PQsocket(c->pg),FIONREAD,&waiting)!=0) { waiting=0; }
   }
  while (waiting>0);

  if (PQstatus(c->pg)==CONNECTION_BAD) { c->broken=1; }
}

/* Closes a connection that broke and answers everything in its flight 500 ( 503 for /health ). Nothing is
 * replayed : a write whose Sync never came back may or may not have committed , and sending it again could
 * duplicate it. EnsureConnections opens the connection again. */
static void FailConnection(struct AmmServer_Instance * instance,struct DbConnection * c)
{
  PQfinish(c->pg); /* closing the socket takes it out of the epoll set */
  c->pg=0;
  c->broken=0;

  while (c->count>0)
   {
     struct PlannedRequest * plan=&c->flight[c->head];
     c->head=(c->head+1)&(FLIGHT_CAPACITY-1);
     --c->count;
     if (plan->statement==NO_STATEMENT) { continue; }

     if (plan->result!=0) { PQclear(plan->result); plan->result=0; }
     Answer(instance,plan);
   }
}

/* Runs on the ring thread when the epoll set has something : results from Postgres , room to write to it ,
 * or a wake-up asked for with WakeDatabase. */
static void DrainDatabase(struct AmmServer_Instance * instance)
{
  uint64_t counter=0;
  ssize_t drained=read(dbWakeFd,&counter,sizeof(counter));
  (void) drained;

  /* In order , and before anything newer. If the transport still has no room it will have by the next turn
   * of the ring , which is when this runs again. */
  if (!SendStashed(instance)) { WakeDatabase(); return; }

  struct epoll_event events[MAX_DB_CONNECTIONS+1];
  int ready=epoll_wait(dbEpollFd,events,MAX_DB_CONNECTIONS+1,0);

  int i=0;
  for (i=0; i<ready; i++)
   {
     struct DbConnection * c=(struct DbConnection *) events[i].data.ptr;
     if ( (c==0) || (c->pg==0) || (c->broken) ) { continue; }

     if (events[i].events&EPOLLOUT) { FlushConnection(c); }
     if (!c->broken) { ReadResults(instance,c); }
   }

  unsigned int k=0;
  for (k=0; k<dbConnectionCount; k++)
   {
     struct DbConnection * c=&dbConnection[k];
     if ( (c->pg!=0) && (c->leftover) && (!c->broken) ) { ReadResults(instance,c); }
   }
  for (k=0; k<dbConnectionCount; k++)
   {
     if ( (dbConnection[k].pg!=0) && (dbConnection[k].broken) ) { FailConnection(instance,&dbConnection[k]); }
   }
}

/* Everything that became ready in one pass over the ring. Each request is planned ; the pass's reads then
 * go to one read connection and its writes to one write connection , each as one group , and the answers
 * are sent from DrainDatabase when the results are back. */
static void HandleBatch(struct AmmServer_APIRequest * requests,struct AmmServer_APIResponse * responses,unsigned int count)
{
  unsigned int readWhich[AMMSERVER_API_MAX_BATCH];
  unsigned int writeWhich[AMMSERVER_API_MAX_BATCH];
  unsigned int readCount=0;
  unsigned int writeCount=0;

  unsigned int i=0;
  for (i=0; i<count; i++)
   {
     struct PlannedRequest * plan=&planScratch[i];
     PlanRequest(&requests[i],&responses[i],plan);
     if (plan->statement==NO_STATEMENT) { continue; } /* PlanRequest answered it already */

     plan->deferToken = responses[i].deferToken;
     plan->wantGzip   = (unsigned char) AcceptsGzip(&requests[i]);

     if ( (plan->statement==STATEMENT_INSERT_POST) || (plan->statement==STATEMENT_INSERT_LIKE) ) { writeWhich[writeCount++]=i; }
     else                                                                                        { readWhich[readCount++]=i; }
   }

  EnsureConnections();

  if (readCount>0)  { SendGroup(PickConnection(0,dbReaderCount,readCount),responses,planScratch,readWhich,readCount); }
  if (writeCount>0) { SendGroup(PickConnection(dbReaderCount,dbConnectionCount,writeCount),responses,planScratch,writeWhich,writeCount); }
}

/* What AmmarServer calls for a request it does not batch : every request on the epoll transport , which has
 * no deferred responses , and the few that arrive on a ring before its batch handler is in place. It expects
 * a finished response when this returns , so the statement runs on a blocking connection of its own , opened
 * the first time it is needed : one round trip per request , used only where the transport cannot do better.
 * PQexecPrepared returns after the statement's own transaction has committed ( rule 8 ). */
static void HandleRequest(struct AmmServer_APIRequest * rqst,struct AmmServer_APIResponse * res)
{
  struct PlannedRequest plan;
  PlanRequest(rqst,res,&plan);
  if (plan.statement==NO_STATEMENT) { return; }

  plan.wantGzip = (unsigned char) AcceptsGzip(rqst);

  if (syncPg==0)
   {
     time_t now=time(0);
     if (now>=syncRetryAt) { syncPg=ConnectDatabase(); if (syncPg==0) { syncRetryAt=now+1; } }
   }

  if (syncPg!=0)
   {
     const char * values[2];
     int lengths[2];
     PlanParameters(&plan,values,lengths);
     plan.result=PQexecPrepared(syncPg,statementName[plan.statement],plan.parameters,values,lengths,binaryParameters,1);

     if (PQstatus(syncPg)!=CONNECTION_OK)
      {
        /* Whatever came back cannot be trusted as an answer , and the next request gets a new connection. */
        if (plan.result!=0) { PQclear(plan.result); plan.result=0; }
        PQfinish(syncPg);
        syncPg=0;
      }
   }

  BuildAnswer(res,&plan);
  ReleasePlan(&plan);
}

/* --- startup ---------------------------------------------------------------------------------------- */

/* A default login shell hands out a soft descriptor limit of 1024 while keeping a far higher hard limit, and
 * nothing would fail loudly at 1024 - the ring table would just be sized for it. So the soft limit is raised
 * to whatever the hard limit already allows, once, before forking, so every worker inherits it. Under the
 * benchmark's LimitNOFILE=2097152 both are already the same. This changes a setting inside this process
 * only, which is what rule 13 permits. */
static void RaiseDescriptorLimit(void)
{
  struct rlimit limit;
  if (getrlimit(RLIMIT_NOFILE,&limit)!=0) { fprintf(stderr,"feedapi: could not read RLIMIT_NOFILE\n"); return; }

  if (limit.rlim_cur!=limit.rlim_max)
   {
     struct rlimit raised;
     raised.rlim_cur = limit.rlim_max;
     raised.rlim_max = limit.rlim_max;
     if (setrlimit(RLIMIT_NOFILE,&raised)==0) { limit.rlim_cur = limit.rlim_max; }
   }

  if (limit.rlim_cur==RLIM_INFINITY) { fprintf(stderr,"feedapi: descriptor limit is unlimited\n"); }
  else { fprintf(stderr,"feedapi: descriptor limit %lu\n",(unsigned long) limit.rlim_cur); }
}

/* How many Postgres connections a worker opens : DB_CONNECTIONS , or the worker's share of
 * DEFAULT_TOTAL_DB_CONNECTIONS , at least one for reads and one for writes. Every worker may open one more
 * ( HandleRequest ) , and the total stays within rule 5's 512 whatever was asked for. */
static unsigned int DesiredDatabaseConnections(unsigned int workers)
{
  unsigned int wanted=DEFAULT_TOTAL_DB_CONNECTIONS/workers;
  const char * text=getenv("DB_CONNECTIONS");
  if ( (text!=0) && (text[0]!=0) ) { wanted=(unsigned int) strtoul(text,0,10); }

  unsigned int maximum=MAX_PG_CONNECTIONS/workers-1;
  if (maximum>MAX_DB_CONNECTIONS) { maximum=MAX_DB_CONNECTIONS; }
  if (wanted>maximum) { wanted=maximum; }
  if (wanted<2)       { wanted=2; }
  return wanted;
}

static int StartDatabase(unsigned int connections)
{
  bodyScratch = (char *) malloc(MAX_RESPONSE_SIZE);
  gzipScratch = (char *) malloc(MAX_RESPONSE_SIZE);
  dbEpollFd   = epoll_create1(EPOLL_CLOEXEC);
  dbWakeFd    = eventfd(0,EFD_NONBLOCK|EFD_CLOEXEC);
  if ( (bodyScratch==0) || (gzipScratch==0) || (dbEpollFd<0) || (dbWakeFd<0) ) { return 0; }

  struct epoll_event ev;
  memset(&ev,0,sizeof(ev));
  ev.events   = EPOLLIN;
  ev.data.ptr = 0; /* which is how DrainDatabase tells it from a connection */
  if (epoll_ctl(dbEpollFd,EPOLL_CTL_ADD,dbWakeFd,&ev)!=0) { return 0; }

  unsigned int writers=connections/4;
  if (writers<1) { writers=1; }
  dbConnectionCount = connections;
  dbReaderCount     = connections-writers;

  unsigned int i=0;
  for (i=0; i<connections; i++)
   {
     struct DbConnection * c=&dbConnection[i];
     memset(c,0,sizeof(*c));
     c->isWriter = (i>=dbReaderCount);
     c->flight   = (struct PlannedRequest *) calloc(FLIGHT_CAPACITY,sizeof(*c->flight));
     if (c->flight==0) { return 0; }
   }

  /* Opened here , before the worker serves anything , so the first requests do not pay for it. If the
   * database is not there yet the worker starts all the same and EnsureConnections keeps trying. */
  EnsureConnections();
  return 1;
}

static int RunWorker(const char * host,unsigned int port,unsigned int databaseConnections)
{
  /* The connections and the epoll set they sit in are made here, after the fork , so that nothing a worker
   * uses was ever shared with another process. */
  if (!StartDatabase(databaseConnections))
   {
     fprintf(stderr,"feedapi: worker %d could not set up its database connections ( %s )\n",getpid(),strerror(errno));
     return 1;
   }

  /* An empty config path , not 0 , so that AmmarServer's automatic lookup cannot pick up a stray
   * ammarServer.conf or /etc/ammarServer.conf and quietly change the port or the thread settings underneath
   * the benchmark. Everything this process needs comes from the environment.
   * One ring : with a worker per CPU a 1.4M-user run on 4 cores is ~350,000 connections per worker, under
   * the ring table's 1,048,576 slots. ReusingPort : the listening socket gets SO_REUSEPORT before it is bound, so
   * every worker on this port binds its own socket and the kernel spreads new connections over them. */
  struct AmmServer_Instance * instance =
      AmmServer_StartAPIOnRingsReusingPort("feedapi",host,port,"/dev/null",(void*) &HandleRequest,MAX_RESPONSE_SIZE,1);
  if (instance==0) { fprintf(stderr,"feedapi: worker %d could not start AmmarServer on %s:%u\n",getpid(),host,port); return 1; }

  /* Before the batch handler : a handler that defers a request needs the transport already polling the
   * descriptor its results will be announced on. An epoll descriptor is readable whenever anything in its
   * set is , so this one stands for every Postgres connection of the worker ; the transport's own read of
   * it fails harmlessly , and DrainDatabase does the reading that matters. */
  AmmServer_SetAPINotifyFD(instance,dbEpollFd,DrainDatabase);

  /* From here on the ring answers whatever is ready in one pass as one batch ( see HandleBatch ). The few
   * requests that could have arrived before this line get HandleRequest. */
  AmmServer_SetAPIBatchHandler(instance,(void*) &HandleBatch);

  while (AmmServer_Running(instance)) { sleep(1); }

  AmmServer_Stop(instance);
  return 0;
}

/* Follow the CPUs this process is actually allowed on : the benchmark gives 16, 8 or 4 cores to the whole
 * server , and a worker is one thread. WORKERS still overrides this. */
static unsigned int DefaultWorkerCount(void)
{
  cpu_set_t affinity;
  unsigned int cpus=0;
  if (sched_getaffinity(0,sizeof(affinity),&affinity)==0) { cpus=(unsigned int) CPU_COUNT(&affinity); }
  if (cpus<MIN_DEFAULT_WORKERS) { cpus=MIN_DEFAULT_WORKERS; }
  if (cpus>MAX_WORKERS)        { cpus=MAX_WORKERS; }
  return cpus;
}

static pid_t workerPid[MAX_WORKERS];
static unsigned int workerCount = 0;

/* Stopping the server is stopping every worker : systemd would take the whole cgroup down anyway, but a
 * plain kill of this process ( which is what test/run.sh does ) must not leave workers holding the ports. */
static void StopWorkers(int signalNumber)
{
  unsigned int i=0;
  for (i=0; i<workerCount; i++) { if (workerPid[i]>0) { kill(workerPid[i],SIGTERM); } }
  signal(signalNumber,SIG_DFL);
  raise(signalNumber);
}

int main(int argc,char *argv[])
{
  serverStartTime = time(0);

  databaseUrl = getenv("DATABASE_URL");
  const char * secret       = getenv("JWT_SECRET");
  const char * host         = getenv("HOST");
  const char * portText     = getenv("PORT");
  const char * extraText    = getenv("EXTRA_PORT");
  const char * workersText  = getenv("WORKERS");

  if ( (databaseUrl==0) || (databaseUrl[0]==0) ) { fprintf(stderr,"feedapi: DATABASE_URL is not set\n"); return 1; }
  if (secret==0) { secret=""; }

  jwtSecretLength = (unsigned int) strlen(secret);
  if (jwtSecretLength>=sizeof(jwtSecret)) { fprintf(stderr,"feedapi: JWT_SECRET is too long\n"); return 1; }
  memcpy(jwtSecret,secret,jwtSecretLength);

  if ( (host==0) || (host[0]==0) ) { host="0.0.0.0"; }

  unsigned int ports[2];
  unsigned int portCount = 0;
  ports[portCount] = (portText!=0) ? (unsigned int) atoi(portText) : 80;
  if (ports[portCount]==0) { ports[portCount]=80; }
  ++portCount;
  unsigned int extraPort = (extraText!=0) ? (unsigned int) atoi(extraText) : 0;
  if ( (extraPort!=0) && (extraPort!=ports[0]) ) { ports[portCount++]=extraPort; }

  unsigned int workers = (workersText!=0) ? (unsigned int) strtoul(workersText,0,10) : DefaultWorkerCount();
  if (workers<portCount)   { workers=portCount; }
  if (workers>MAX_WORKERS) { workers=MAX_WORKERS; }
  unsigned int databaseConnections=DesiredDatabaseConnections(workers);

  RaiseDescriptorLimit();

  fprintf(stderr,"feedapi: AmmarServer %s , libpq %d , ISA-L igzip , %u workers on %s port %u",
          AmmServer_Version(),PQlibVersion(),workers,host,ports[0]);
  if (portCount>1) { fprintf(stderr," and %u",ports[1]); }
  fprintf(stderr," , %u database connections per worker\n",databaseConnections);

  pid_t parent = getpid();
  unsigned int i=0;
  for (i=0; i<workers; i++)
   {
     pid_t pid = fork();
     if (pid<0) { fprintf(stderr,"feedapi: fork failed after %u workers ( %s )\n",i,strerror(errno)); break; }
     if (pid==0)
      {
        /* A worker goes when this process goes, however it goes - including a SIGKILL that gives it no chance
         * to pass anything on. getppid is checked after asking, for a parent that was already gone by then. */
        prctl(PR_SET_PDEATHSIG,SIGTERM);
        if (getppid()!=parent) { _exit(1); }
        _exit( RunWorker(host,ports[i%portCount],databaseConnections) );
      }
     workerPid[workerCount++] = pid;
   }

  signal(SIGTERM,StopWorkers);
  signal(SIGINT,StopWorkers);

  /* Not restarted ( rule 10 ), and not taken down with it either : a worker that dies takes its share of the
   * connections with it, and the rest go on serving theirs. */
  unsigned int alive = workerCount;
  while (alive>0)
   {
     int status=0;
     pid_t pid = waitpid(-1,&status,0);
     if (pid<0) { if (errno==EINTR) { continue; } break; }
     --alive;
     if (WIFSIGNALED(status)) { fprintf(stderr,"feedapi: worker %d killed by signal %d , %u left\n",pid,WTERMSIG(status),alive); }
     else                     { fprintf(stderr,"feedapi: worker %d exited with %d , %u left\n",pid,WEXITSTATUS(status),alive); }
   }

  return 1;
}
