/*
 * The million users challenge feed API, in C, on AmmarServer ( https://github.com/AmmarkoV/AmmarServer ),
 * against PostgreSQL through libpq.
 *
 * The process model is prefork : main() forks WORKERS processes and then only waits for them. Each worker
 * is AmmarServer in API mode on one io_uring ring - one thread owning every connection it accepts , and
 * that same thread driving the worker's Postgres connections , so a 4-core run is 4 threads. libpq logs each
 * connection in and prepares its statements ; from there the worker speaks the wire protocol on the
 * connection's socket itself ( see "the wire" below ). Half of the
 * workers listen on PORT and half on EXTRA_PORT, all with SO_REUSEPORT, so the kernel spreads the incoming
 * connections evenly over the workers of each port and every worker owns its share for the connection's
 * whole life.
 *
 * A request is a network round trip to the database box, and the ring thread never waits on one. Everything
 * that became ready in one pass over the ring is planned together ( route , token , JSON , id ) ; the pass's
 * reads go to one read connection as one run of Bind/Execute ended by one Sync , its writes to one write
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
#include <sys/socket.h>
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
/* The transport hands the handler at most AMMSERVER_API_MAX_BATCH ( 32 ) requests a call , and a busy pass
 * holds more than that. A group therefore stays open across the calls of one pass and gets its Sync when a
 * call comes that is not full , or at this many statements. */
#define GROUP_MAX 128
/* What has been read from a connection and not yet answered. One statement's result has to fit : the largest
 * is a feed of 20 posts of 500 four-byte characters , ~41 KB. */
#define DB_IN_CAPACITY (256*1024)
/* What is waiting to be written to a connection. It grows as needed up to this , past which a statement is
 * answered 500 : that is a database that stopped reading long ago. */
#define DB_OUT_INITIAL (64*1024)
#define DB_OUT_MAXIMUM (16*1024*1024)

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

/* What Postgres said about a plan's statement. */
enum Outcome
{
  OUTCOME_NONE = 0,               /* nothing : it never reached Postgres , its connection was lost , or its group was aborted */
  OUTCOME_OK,
  OUTCOME_MISSING_POST,           /* SQLSTATE 23503 , the foreign key of a like */
  OUTCOME_ERROR
};

struct PlannedRequest
{
  enum Statement statement;
  int parameters;
  uint64_t parameter[2];          /* the int8 parameters, big endian, the binary format for them */
  long long postId;               /* for the like answer */
  char * body;                    /* POST /posts : malloc'd trimmed body with the username right behind it , freed after its response is built */
  unsigned int bodyLength;
  unsigned int usernameLength;
  unsigned long long deferToken;  /* the transport's handle for this request */
  unsigned char wantGzip;
  unsigned char outcome;          /* enum Outcome */
  unsigned char inserted;         /* a like : the row is new */
  unsigned char newIdLength;      /* a new post : RETURNING id , as its digits ... */
  char newId[20];
  long long createdAt;            /* ... and created_at , microseconds since 2000-01-01 UTC */
};

/* One connection , non-blocking. flight holds what has been sent and not yet answered , with a Sync marker
 * ( NO_STATEMENT ) closing each group. On a read connection the plan at the head is answered as soon as its
 * rows are in. On a write connection the outcomes of a group are kept until the group's ReadyForQuery comes
 * back , which Postgres sends after the commit ( rule 8 ) : groupDone counts the outcomes in hand at the head
 * of the ring. out is what still has to be written to the socket ; in is what has been read from it , of
 * which [inKeep,inScan) are the complete messages of the statement being received. */
struct DbConnection
{
  PGconn * pg;                     /* libpq logged in and prepared the statements ; it now only owns the socket */
  int fd;
  struct PlannedRequest * flight;
  unsigned int head;
  unsigned int count;
  unsigned int groupDone;
  char * out;
  unsigned int outUsed;
  unsigned int outSent;
  unsigned int outCapacity;
  char * in;
  unsigned int inUsed;
  unsigned int inScan;
  unsigned int inKeep;
  unsigned char isWriter;
  unsigned char groupFailed;
  unsigned char commitFailed;      /* the error came at the Sync itself : nothing of the group was committed */
  unsigned char wantsWrite;        /* out could not all be written : EPOLLOUT is being waited for */
  unsigned char leftover;          /* complete messages were left unparsed in `in` ; look again without waiting for the socket */
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

/* One post , from wherever it came : a DataRow in a connection's buffer , a libpq result , or a new post's
 * plan. The id and the like count are their decimal digits. */
struct PostRow
{
  const char * id;      unsigned int idLength;
  const char * body;    unsigned int bodyLength;
  long long createdAt;  /* microseconds since 2000-01-01 UTC */
  const char * author;  unsigned int authorLength;
  int authorIsJsonEscaped;
  const char * likes;   unsigned int likesLength;
};

static unsigned int PutTimestamp(long long microseconds,char out[32]);

static void PutPostObject(struct AmmServer_APIResponse * res,const struct PostRow * row)
{
  char createdAt[32];
  unsigned int createdAtLength = PutTimestamp(row->createdAt,createdAt);

  /* The usual case , written straight into the buffer : one room check for the worst this object could
   * take ( every byte of body and author escaped to \u00XX ) instead of one per piece. Anything else - a
   * buffer near its end , an overflow already - takes the piece by piece path below , which is what decides
   * overflows. */
  unsigned long long worst = 64 + row->idLength + row->likesLength + 6ULL*row->bodyLength + createdAtLength
                           + ( row->authorIsJsonEscaped ? row->authorLength : 6ULL*row->authorLength );
  if ( (!res->overflowed) && ( (unsigned long long) res->bodyLength + worst <= res->maxBodyLength ) )
   {
     char * at  = res->body + res->bodyLength;
     char * end = res->body + res->maxBodyLength;
     PUT_LITERAL(at,"{\"id\":");
     memcpy(at,row->id,row->idLength); at+=row->idLength;
     PUT_LITERAL(at,",\"body\":\"");
     at += AmmServer_JSONEscape(row->body,row->bodyLength,at,(unsigned int)(end-at));
     PUT_LITERAL(at,"\",\"created_at\":\"");
     memcpy(at,createdAt,createdAtLength); at+=createdAtLength;
     PUT_LITERAL(at,"\",\"author\":\"");
     if (row->authorIsJsonEscaped) { memcpy(at,row->author,row->authorLength); at+=row->authorLength; }
     else                          { at += AmmServer_JSONEscape(row->author,row->authorLength,at,(unsigned int)(end-at)); }
     PUT_LITERAL(at,"\",\"like_count\":");
     memcpy(at,row->likes,row->likesLength); at+=row->likesLength;
     PUT_LITERAL(at,"}");
     res->bodyLength = (unsigned int)( at - res->body );
     return;
   }

  WriteLiteral(res,"{\"id\":");
  AmmServer_APIResponseWrite(res,row->id,row->idLength);
  WriteLiteral(res,",\"body\":\"");
  AmmServer_APIResponseWriteJSONString(res,row->body,row->bodyLength);
  WriteLiteral(res,"\",\"created_at\":\"");
  AmmServer_APIResponseWrite(res,createdAt,createdAtLength);
  WriteLiteral(res,"\",\"author\":\"");
  if (row->authorIsJsonEscaped) { AmmServer_APIResponseWrite(res,row->author,row->authorLength); }
  else                          { AmmServer_APIResponseWriteJSONString(res,row->author,row->authorLength); }
  WriteLiteral(res,"\",\"like_count\":");
  AmmServer_APIResponseWrite(res,row->likes,row->likesLength);
  WriteLiteral(res,"}");
}

/* --- reading results --------------------------------------------------------------------------------- */

static uint32_t ReadBE32(const char * at) { uint32_t value; memcpy(&value,at,sizeof(value)); return be32toh(value); }
static uint16_t ReadBE16(const char * at) { uint16_t value; memcpy(&value,at,sizeof(value)); return be16toh(value); }
static long long ReadBE64(const char * at) { uint64_t value; memcpy(&value,at,sizeof(value)); return (long long) be64toh(value); }

/* A fixed number of decimal digits, zero padded, written straight in. */
static char * PutDigits(char * at,unsigned int value,unsigned int digits)
{
  unsigned int i=digits;
  while (i>0) { --i; at[i] = (char)( '0' + value%10 ); value/=10; }
  return at+digits;
}

/* 2025-12-31T23:59:19.409Z : UTC, truncated ( not rounded ) to milliseconds, as the spec asks. created_at
 * is asked for in binary : microseconds since 2000-01-01 UTC , the same whatever the session's TimeZone and
 * DateStyle are , so there is no text timestamp to parse back and nothing to SET first.
 * Written by hand rather than with gmtime_r + snprintf, since a feed formats 20 of them : the date is
 * Howard Hinnant's civil_from_days, exact for the whole proleptic Gregorian calendar, and only a year past
 * 9999 ( which no row can have ) would come out wrong. */
/* The date part is the same for most posts of a feed , so each thread keeps the last day it wrote. */
static __thread long long cachedDay = -1;
static __thread int cachedDayValid = 0;
static __thread char cachedDate[10];

static unsigned int PutTimestamp(long long microseconds,char out[32])
{
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

/* The columns of one DataRow message ( 'D' , int32 length , int16 columns , then int32 length + bytes for
 * each ). 0 unless it has exactly `columns` of them , none NULL , all inside the message. */
static int DataRowColumns(const char * message,unsigned int columns,const char * value[],unsigned int size[])
{
  const char * end = message + 1 + ReadBE32(message+1);
  const char * at  = message + 5;
  if ( (end-at<2) || (ReadBE16(at)!=columns) ) { return 0; }
  at+=2;

  unsigned int i=0;
  for (i=0; i<columns; i++)
   {
     if (end-at<4) { return 0; }
     int32_t length = (int32_t) ReadBE32(at);
     at+=4;
     if ( (length<0) || (length>end-at) ) { return 0; }
     value[i]=at; size[i]=(unsigned int) length;
     at+=length;
   }
  return 1;
}

/* One row of the feed/post query, which both select the same five columns in the same order : the id and
 * the like count come as text ( their digits go into the JSON as they are ) , created_at in binary. */
static int PostRowFromDataRow(const char * message,struct PostRow * row)
{
  const char * value[5];
  unsigned int size[5];
  if ( (!DataRowColumns(message,5,value,size)) || (size[2]!=8) ) { return 0; }

  row->id      = value[0]; row->idLength     = size[0];
  row->body    = value[1]; row->bodyLength   = size[1];
  row->createdAt = ReadBE64(value[2]);
  row->author  = value[3]; row->authorLength = size[3];
  row->authorIsJsonEscaped = 0;
  row->likes   = value[4]; row->likesLength  = size[4];
  return 1;
}

/* The same row out of a libpq result , where every column is binary ( HandleRequest ) : digits holds the two
 * numbers written out. */
static void PostRowFromResult(const PGresult * r,int rowNumber,struct PostRow * row,char digits[40])
{
  uint32_t likes; memcpy(&likes,PQgetvalue(r,rowNumber,4),sizeof(likes));
  char * idEnd    = PutUnsignedDecimal(digits,(unsigned long long) ReadBE64(PQgetvalue(r,rowNumber,0)));
  char * likesEnd = PutUnsignedDecimal(idEnd,(unsigned long long) be32toh(likes));

  row->id      = digits;                    row->idLength     = (unsigned int)( idEnd-digits );
  row->body    = PQgetvalue(r,rowNumber,1); row->bodyLength   = (unsigned int) PQgetlength(r,rowNumber,1);
  row->createdAt = ReadBE64(PQgetvalue(r,rowNumber,2));
  row->author  = PQgetvalue(r,rowNumber,3); row->authorLength = (unsigned int) PQgetlength(r,rowNumber,3);
  row->authorIsJsonEscaped = 0;
  row->likes   = idEnd;                     row->likesLength  = (unsigned int)( likesEnd-idEnd );
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
  /* Plain TCP , whatever the URL says : the worker reads and writes this socket itself afterwards. */
  static const char * const keywords[] = { "dbname" , "connect_timeout" , "sslmode" , "gssencmode" , 0 };
  const char * const values[]         = { databaseUrl , "3" , "disable" , "disable" , 0 };

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
  plan->outcome = OUTCOME_NONE;
  plan->inserted = 0;
  plan->newIdLength = 0;
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

static void AnswerHealth(struct AmmServer_APIResponse * res,const struct PlannedRequest * plan)
{
  /* The spec wants the ok answer to mean a SELECT 1 actually came back, so it is run per request. */
  if (plan->outcome!=OUTCOME_OK)
   {
     AmmServer_APIResponseReset(res,503,0);
     WriteLiteral(res,"{\"status\":\"degraded\",\"db\":\"unreachable\",\"error\":\"");
     if (plan->outcome==OUTCOME_NONE) { WriteLiteral(res,"the connection to the database was lost"); }
     else                             { WriteLiteral(res,"the database answered with an error"); }
     WriteLiteral(res,"\"}");
     return;
   }

  res->statusCode = 200;
  WriteLiteral(res,"{\"status\":\"ok\",\"db\":\"ok\",\"uptime_s\":");
  AmmServer_APIResponseWriteInteger(res,(long long)( time(0)-serverStartTime ));
  WriteLiteral(res,"}");
}

/* The rows a read's response is built from : the messages of its statement , [rows,rowsEnd) in a
 * connection's buffer , or a libpq result ( HandleRequest ). Whichever it is , the posts come out one at a
 * time , in order. */
struct RowSource
{
  const char * rows;
  const char * rowsEnd;
  const PGresult * result;
  int resultRow;
};

/* 1=row filled , 0=no more rows , -1=a row that does not look like one. */
static int NextPostRow(struct RowSource * source,struct PostRow * row,char digits[40])
{
  if (source->result!=0)
   {
     if (source->resultRow>=PQntuples(source->result)) { return 0; }
     PostRowFromResult(source->result,source->resultRow++,row,digits);
     return 1;
   }

  /* Every message in the range is complete : the parser only hands over whole statements. */
  while (source->rows<source->rowsEnd)
   {
     const char * message = source->rows;
     source->rows += 1 + ReadBE32(message+1);
     if (message[0]=='D') { return PostRowFromDataRow(message,row) ? 1 : -1; }
   }
  return 0;
}

static void AnswerFeed(struct AmmServer_APIResponse * res,struct RowSource * source)
{
  struct PostRow row;
  char digits[40];
  int more=0;
  int rows=0;

  WriteLiteral(res,"{\"posts\":[");
  while ( (more=NextPostRow(source,&row,digits))>0 )
   {
     if (rows++>0) { WriteLiteral(res,","); }
     PutPostObject(res,&row);
   }
  WriteLiteral(res,"]}");

  if ( (more<0) || (AmmServer_APIResponseOverflowed(res)) ) { Fail(res,500,"internal server error"); return; }

  res->statusCode = 200;
}

static void AnswerPost(struct AmmServer_APIResponse * res,struct RowSource * source)
{
  struct PostRow row;
  char digits[40];
  int found=NextPostRow(source,&row,digits);
  if (found<0)  { Fail(res,500,"internal server error"); return; }
  if (found==0) { Fail(res,404,"post not found"); return; }

  WriteLiteral(res,"{\"post\":");
  PutPostObject(res,&row);
  WriteLiteral(res,"}");

  if (AmmServer_APIResponseOverflowed(res)) { Fail(res,500,"internal server error"); return; }

  res->statusCode = 200;
}

static void AnswerCreatePost(struct AmmServer_APIResponse * res,const struct PlannedRequest * plan)
{
  /* Answered only after the ReadyForQuery of the Sync that committed it , so the 201 below means the row is
   * committed ( rule 8 ). */
  struct PostRow row;
  row.id      = plan->newId;                 row.idLength     = plan->newIdLength;
  row.body    = plan->body;                  row.bodyLength   = plan->bodyLength;
  row.createdAt = plan->createdAt;
  row.author  = plan->body+plan->bodyLength; row.authorLength = plan->usernameLength;
  row.authorIsJsonEscaped = 1;
  row.likes   = "0";                         row.likesLength  = 1;

  WriteLiteral(res,"{\"post\":");
  PutPostObject(res,&row);
  WriteLiteral(res,"}");

  if (AmmServer_APIResponseOverflowed(res)) { Fail(res,500,"internal server error"); return; }

  res->statusCode = 201;
}

static void AnswerLike(struct AmmServer_APIResponse * res,const struct PlannedRequest * plan)
{
  /* "INSERT 0 1" or, when ON CONFLICT DO NOTHING kept the like that was already there, "INSERT 0 0". */
  WriteLiteral(res,"{\"liked\":true,\"already_liked\":");
  if (plan->inserted) { WriteLiteral(res,"false"); } else { WriteLiteral(res,"true"); }
  WriteLiteral(res,",\"post_id\":");
  AmmServer_APIResponseWriteInteger(res,plan->postId);
  WriteLiteral(res,"}");

  res->statusCode = plan->inserted ? 201 : 200;
}

/* --- gzip ------------------------------------------------------------------------------------------- */

/* Read while the request bytes are still in hand : the plan outlives them. */
static int AcceptsGzip(const struct AmmServer_APIRequest * rqst)
{
  unsigned int valueLength=0;
  const char * value = AmmServer_APIRequestHeader(rqst,"Accept-Encoding",&valueLength);
  return ( (value!=0) && (memmem(value,valueLength,"gzip",4)!=0) );
}

/* igzip's level 0 does not build a Huffman code for what it compresses : it uses a table made beforehand ,
 * by default one for general data. About once a second the table is made again from the symbols of the
 * response in hand , so it fits JSON feeds instead ( ~2.2 KB down to ~1.95 KB a feed , for the same CPU ). It
 * is a code table , not content : each response is still compressed from its own bytes , and any table
 * encodes any input correctly. */
static struct isal_hufftables trainedTables;
static int trainedTablesReady = 0;
static time_t trainedAt = 0;

static void TrainGzip(const char * body,unsigned int bodyLength)
{
  struct isal_huff_histogram histogram;
  memset(&histogram,0,sizeof(histogram));
  isal_update_histogram((uint8_t *) body,(int) bodyLength,&histogram);

  static struct isal_hufftables candidate;
  if (isal_create_hufftables(&candidate,&histogram)==0) { trainedTables=candidate; trainedTablesReady=1; }
}

/* ISA-L igzip at level 0 : one hash lookup per position and no lazy matching, which is the fastest setting
 * and enough for a feed that goes out as two TCP segments either way. Stateless, into the worker's scratch
 * buffer, and only kept when it actually made the response smaller. */
static void GzipResponse(struct AmmServer_APIResponse * res)
{
  if (gzipScratch==0) { return; }

  time_t now=time(0);
  if (now!=trainedAt) { trainedAt=now; TrainGzip(res->body,res->bodyLength); }

  struct isal_zstream stream;
  isal_deflate_stateless_init(&stream);
  stream.next_in   = (uint8_t *) res->body;
  stream.avail_in  = res->bodyLength;
  stream.next_out  = (uint8_t *) gzipScratch;
  stream.avail_out = (uint32_t) MAX_RESPONSE_SIZE;
  stream.gzip_flag = IGZIP_GZIP;
  stream.level     = 0;
  if (trainedTablesReady) { stream.hufftables = &trainedTables; }

  if (isal_deflate_stateless(&stream)!=COMP_OK) { return; }

  size_t compressedLength = (size_t) MAX_RESPONSE_SIZE - stream.avail_out;
  if ( (compressedLength==0) || (compressedLength>=res->bodyLength) ) { return; }

  /* A deferred answer was built in the worker's own scratch and is copied by the transport from wherever
   * it is ; one built in the transport's buffer has to end up there. */
  if (res->body==bodyScratch) { res->body=gzipScratch; }
  else                        { memcpy(res->body,gzipScratch,compressedLength); }
  res->bodyLength   = (unsigned int) compressedLength;
  res->extraHeaders = "Content-Encoding: gzip\r\n";
}

/* --- answering a plan ------------------------------------------------------------------------------ */

/* The response for a plan whose outcome is known. A plan without one never reached Postgres , or its
 * connection was lost , or its group was aborted : /health says that with its 503 and everything else with
 * a 500. source is where a read's rows are. */
static void BuildAnswer(struct AmmServer_APIResponse * res,const struct PlannedRequest * plan,struct RowSource * source)
{
  if (plan->statement==STATEMENT_PING) { AnswerHealth(res,plan); } else
  if (plan->outcome==OUTCOME_MISSING_POST)
   {
     /* A like on a post that does not exist is the foreign key on likes.post_id failing, as the spec says. */
     if (plan->statement==STATEMENT_INSERT_LIKE) { Fail(res,404,"post not found"); } else { Fail(res,500,"internal server error"); }
   } else
  if ( (plan->outcome!=OUTCOME_OK) || ( (source==0) && ( (plan->statement==STATEMENT_FEED) || (plan->statement==STATEMENT_POST) ) ) )
   {
     Fail(res,500,"internal server error");
   } else
   {
     switch (plan->statement)
      {
        case STATEMENT_FEED        : AnswerFeed(res,source); break;
        case STATEMENT_POST        : AnswerPost(res,source); break;
        case STATEMENT_INSERT_POST : AnswerCreatePost(res,plan); break;
        case STATEMENT_INSERT_LIKE : AnswerLike(res,plan); break;
        default                    : Fail(res,500,"internal server error"); break;
      };
   }

  if (AmmServer_APIResponseOverflowed(res)) { Fail(res,500,"internal server error"); }

  if ( (plan->wantGzip) && (res->bodyLength>=GZIP_MIN_SIZE) ) { GzipResponse(res); }
}

static void ReleasePlan(struct PlannedRequest * plan)
{
  if (plan->body!=0) { free(plan->body); plan->body=0; }
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

/* Builds a plan's response and sends it , from the ring thread. rows..rowsEnd are a read's messages , still
 * in its connection's buffer. If every send ticket is busy the response is kept , in order , for
 * DrainDatabase's next run. */
static void Answer(struct AmmServer_Instance * instance,struct PlannedRequest * plan,const char * rows,const char * rowsEnd)
{
  struct AmmServer_APIResponse res;
  memset(&res,0,sizeof(res));
  res.statusCode    = 200;
  res.contentType   = "application/json";
  res.body          = bodyScratch;
  res.maxBodyLength = MAX_RESPONSE_SIZE;

  struct RowSource source;
  source.rows = rows; source.rowsEnd = rowsEnd; source.result = 0; source.resultRow = 0;

  BuildAnswer(&res,plan,(rows!=0) ? &source : 0);

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

/* --- the wire ---------------------------------------------------------------------------------------
 * libpq connects , authenticates and prepares ; after that a worker's connections are spoken to here , in
 * the extended query protocol. A statement is one Bind ( of the statement libpq prepared , to the unnamed
 * portal ) and one Execute ; a group of them is ended by one Sync. There is no Describe - libpq's
 * PQsendQueryPrepared always sends one , so every reply carried a RowDescription nobody needed - and a reply
 * is BindComplete , DataRows , CommandComplete , read where it lies in the connection's buffer with no result
 * object in between. After an error Postgres skips to the Sync , so the statements behind it get no reply at
 * all , and the ReadyForQuery that answers the Sync ends the group either way. */

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

/* The result column formats each statement asks for ( 0 text , 1 binary ) : ids and like counts as text ,
 * which is fewer bytes than an int8 and goes into the JSON as it is ; created_at binary ; body and username
 * are the same bytes either way. */
static const unsigned char resultFormatCount[] = { 0 , 0 , 5 , 5 , 2 , 0 };
static const unsigned char resultFormat[][5]   = { {0} , {0} , {0,1,1,1,0} , {0,1,1,1,0} , {0,1} , {0} };

static char * PutBE32(char * at,uint32_t value) { value=htobe32(value); memcpy(at,&value,4); return at+4; }
static char * PutBE16(char * at,uint16_t value) { value=htobe16(value); memcpy(at,&value,2); return at+2; }

/* Room for `needed` more bytes in a connection's output , growing it if that is what it takes. */
static int OutReserve(struct DbConnection * c,unsigned int needed)
{
  if (needed<=c->outCapacity-c->outUsed) { return 1; }
  if ( (needed>DB_OUT_MAXIMUM) || (c->outUsed>DB_OUT_MAXIMUM-needed) ) { return 0; }

  unsigned int capacity=c->outCapacity;
  while (capacity-c->outUsed<needed) { capacity*=2; }
  char * grown=(char *) realloc(c->out,capacity);
  if (grown==0) { return 0; }
  c->out=grown;
  c->outCapacity=capacity;
  return 1;
}

/* Bind + Execute for one plan , appended to the connection's output. 0 if there is no room for it. */
static int AppendStatement(struct DbConnection * c,const struct PlannedRequest * plan)
{
  const char * values[2];
  int lengths[2];
  PlanParameters(plan,values,lengths);

  const char * name=statementName[plan->statement];
  unsigned int nameLength=(unsigned int) strlen(name)+1;
  unsigned int formats=resultFormatCount[plan->statement];

  unsigned int bindLength = 4 + 1 + nameLength + 2 + 2 + 2 + 2 + 2*formats;
  int i=0;
  for (i=0; i<plan->parameters; i++) { bindLength += 4 + (unsigned int) lengths[i]; }

  if (!OutReserve(c,1+bindLength+10)) { return 0; }

  char * at=c->out+c->outUsed;
  *at++='B';
  at=PutBE32(at,bindLength);
  *at++=0;                                   /* the unnamed portal */
  memcpy(at,name,nameLength); at+=nameLength;
  at=PutBE16(at,1); at=PutBE16(at,1);        /* every parameter is binary */
  at=PutBE16(at,(uint16_t) plan->parameters);
  for (i=0; i<plan->parameters; i++)
   {
     at=PutBE32(at,(uint32_t) lengths[i]);
     memcpy(at,values[i],(size_t) lengths[i]); at+=lengths[i];
   }
  at=PutBE16(at,(uint16_t) formats);
  unsigned int f=0;
  for (f=0; f<formats; f++) { at=PutBE16(at,resultFormat[plan->statement][f]); }

  *at++='E';
  at=PutBE32(at,9);
  *at++=0;                                   /* the unnamed portal */
  at=PutBE32(at,0);                          /* every row */

  c->outUsed=(unsigned int)( at-c->out );
  return 1;
}

static int AppendSync(struct DbConnection * c)
{
  if (!OutReserve(c,5)) { return 0; }
  char * at=c->out+c->outUsed;
  *at++='S';
  PutBE32(at,4);
  c->outUsed+=5;
  return 1;
}

/* --- the connections ------------------------------------------------------------------------------- */

/* Opens one of the worker's connections and puts its socket in the epoll set. */
static int OpenConnection(struct DbConnection * c)
{
  PGconn * pg = ConnectDatabase();
  if (pg==0) { return 0; }

  struct epoll_event ev;
  memset(&ev,0,sizeof(ev));
  ev.events   = EPOLLIN;
  ev.data.ptr = c;

  /* libpq is not asked anything about this connection again , so nothing may be left in its own buffers ,
   * and the socket has to be the plain TCP one the wire code expects. */
  if ( (PQsslInUse(pg)) || (PQtransactionStatus(pg)!=PQTRANS_IDLE) || (epoll_ctl(dbEpollFd,EPOLL_CTL_ADD,PQsocket(pg),&ev)!=0) )
   {
     PQfinish(pg);
     return 0;
   }

  c->pg = pg;
  c->fd = PQsocket(pg);
  c->head = 0; c->count = 0; c->groupDone = 0;
  c->outUsed = 0; c->outSent = 0;
  c->inUsed = 0; c->inScan = 0; c->inKeep = 0;
  c->groupFailed = 0; c->commitFailed = 0; c->wantsWrite = 0; c->leftover = 0; c->broken = 0;
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

/* Writes the connection's output to its socket. What does not fit waits for EPOLLOUT. */
static void FlushConnection(struct DbConnection * c)
{
  while (c->outSent<c->outUsed)
   {
     ssize_t written=send(c->fd,c->out+c->outSent,c->outUsed-c->outSent,MSG_DONTWAIT|MSG_NOSIGNAL);
     if (written>0) { c->outSent+=(unsigned int) written; continue; }
     if ( (written<0) && (errno==EINTR) ) { continue; }
     if ( (written<0) && ( (errno==EAGAIN) || (errno==EWOULDBLOCK) ) ) { break; }
     MarkBroken(c);
     return;
   }
  if (c->outSent==c->outUsed) { c->outSent=0; c->outUsed=0; }

  unsigned char wantsWrite=(c->outUsed>0);
  if (wantsWrite!=c->wantsWrite)
   {
     struct epoll_event ev;
     memset(&ev,0,sizeof(ev));
     ev.events   = wantsWrite ? (EPOLLIN|EPOLLOUT) : EPOLLIN;
     ev.data.ptr = c;
     epoll_ctl(dbEpollFd,EPOLL_CTL_MOD,c->fd,&ev);
     c->wantsWrite=wantsWrite;
   }
}

static struct PlannedRequest * FlightAt(struct DbConnection * c,unsigned int index)
{
  return &c->flight[(c->head+index)&(FLIGHT_CAPACITY-1)];
}

static struct PlannedRequest * FlightPush(struct DbConnection * c)
{
  struct PlannedRequest * slot=FlightAt(c,c->count);
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

/* The group being put together in this pass , of reads [0] and of writes [1] : its statements are in a
 * connection's output and flight , its Sync is not yet. */
static struct DbConnection * openGroup[2] = { 0 , 0 };
static unsigned int openGroupSize[2] = { 0 , 0 };

/* Ends a group with its Sync and writes it out : one write , one round trip. */
static void CloseGroup(int kind)
{
  struct DbConnection * c=openGroup[kind];
  openGroup[kind]=0;
  openGroupSize[kind]=0;
  if ( (c==0) || (c->broken) ) { return; }

  /* Without its Sync nothing sent before can be told apart from what follows , so the connection goes , and
   * DrainDatabase answers what is in its flight. */
  if (!AppendSync(c)) { MarkBroken(c); return; }
  FlightPush(c)->statement=NO_STATEMENT;
  FlushConnection(c);
}

/* Adds plans to the open group of their kind , opening one on the least busy connection if there is none.
 * The group's single Sync is one round trip and one implicit transaction : reads share the transaction
 * ( each statement still takes its own READ COMMITTED snapshot , so it sees commits made before it ran ),
 * and the writes of a group commit together ( rule 3 allows batching writes into one transaction ; rule 8
 * is still kept , every response is held until that commit's ReadyForQuery is back ). A plan that cannot be
 * sent is answered where it stands , in the response the transport prepared for it. */
static void SendGroup(
                       int kind,
                       struct AmmServer_APIResponse * responses,
                       struct PlannedRequest * plans,
                       const unsigned int * which,
                       unsigned int count
                     )
{
  if (count==0) { return; }

  struct DbConnection * c=openGroup[kind];
  if ( (c!=0) && ( (c->broken) || (c->count+count+1>FLIGHT_CAPACITY) ) ) { CloseGroup(kind); c=0; }
  if (c==0)
   {
     if (kind==0) { c=PickConnection(0,dbReaderCount,count); }
     else         { c=PickConnection(dbReaderCount,dbConnectionCount,count); }
   }

  unsigned int sent=0;
  unsigned int i=0;
  for (i=0; i<count; i++)
   {
     struct PlannedRequest * plan=&plans[which[i]];

     if ( (c==0) || (!AppendStatement(c,plan)) )
      {
        BuildAnswer(&responses[which[i]],plan,0);
        ReleasePlan(plan);
        continue;
      }

     *FlightPush(c)=*plan;
     responses[which[i]].deferred=1;
     ++sent;
   }

  if (sent==0) { return; }

  openGroup[kind]=c;
  openGroupSize[kind]+=sent;
  if (openGroupSize[kind]>=GROUP_MAX) { CloseGroup(kind); }
}

/* A write whose group was rolled back , sent again as a transaction of its own. 0 if it could not be sent ,
 * in which case nothing of it is in flight. */
static int SendAlone(struct DbConnection * c,const struct PlannedRequest * plan)
{
  if ( (c->broken) || (c->count+2>FLIGHT_CAPACITY) ) { return 0; }
  if (!AppendStatement(c,plan)) { return 0; }

  *FlightPush(c)=*plan;
  if (!AppendSync(c)) { MarkBroken(c); return 1; }
  FlightPush(c)->statement=NO_STATEMENT;
  return 1;
}

/* The ReadyForQuery of a write group has come back : its transaction has committed , or rolled back. A
 * committed group is answered. A group that one statement aborted ( a like on a missing post ) was rolled
 * back whole : the like that failed has its answer , and every other write is sent again in a transaction
 * of its own , so the valid ones commit. A write that was already alone is answered with whatever it got. */
static void EndGroup(struct AmmServer_Instance * instance,struct DbConnection * c,unsigned int count)
{
  struct PlannedRequest group[GROUP_MAX+AMMSERVER_API_MAX_BATCH];
  int failed=c->groupFailed;
  int commitFailed=c->commitFailed;

  /* Only a connection that broke its own rules could have more than this between two Syncs. */
  if (count>GROUP_MAX+AMMSERVER_API_MAX_BATCH) { c->broken=1; return; }

  unsigned int i=0;
  for (i=0; i<count; i++) { group[i]=*FlightAt(c,i); }

  c->head=(c->head+count+1)&(FLIGHT_CAPACITY-1);
  c->count-=count+1;
  c->groupDone=0;
  c->groupFailed=0;
  c->commitFailed=0;

  int replay=( (failed) && (count>1) );

  /* A replayed write must not join the group this pass is still putting together on this connection. */
  if ( (replay) && (openGroup[1]==c) ) { CloseGroup(1); }

  for (i=0; i<count; i++)
   {
     if (commitFailed) { group[i].outcome=OUTCOME_ERROR; }

     if ( (replay) && (group[i].outcome!=OUTCOME_MISSING_POST) )
      {
        group[i].outcome=OUTCOME_NONE;
        if (SendAlone(c,&group[i])) { continue; }
      }
     Answer(instance,&group[i],0,0);
   }

  if ( (replay) && (!c->broken) ) { FlushConnection(c); }
}

/* What a write's CommandComplete says , kept in its plan until the group's commit. messages..complete are
 * the statement's messages before it. */
static void TakeWriteOutcome(struct PlannedRequest * plan,const char * messages,const char * complete)
{
  plan->outcome=OUTCOME_OK;

  if (plan->statement==STATEMENT_INSERT_LIKE)
   {
     /* "INSERT 0 1" or, when ON CONFLICT DO NOTHING kept the like that was already there, "INSERT 0 0". */
     static const char insertedOne[]="INSERT 0 1";
     plan->inserted=( (ReadBE32(complete+1)==4+sizeof(insertedOne)) && (memcmp(complete+5,insertedOne,sizeof(insertedOne))==0) );
     return;
   }

  if (plan->statement==STATEMENT_INSERT_POST)
   {
     /* RETURNING id , created_at : one row , the id as text. */
     plan->outcome=OUTCOME_ERROR;
     while (messages<complete)
      {
        const char * message=messages;
        messages += 1 + ReadBE32(message+1);
        if (message[0]!='D') { continue; }

        const char * value[2];
        unsigned int size[2];
        if ( (!DataRowColumns(message,2,value,size)) || (size[0]==0) || (size[0]>sizeof(plan->newId)) || (size[1]!=8) ) { return; }
        memcpy(plan->newId,value[0],size[0]);
        plan->newIdLength=(unsigned char) size[0];
        plan->createdAt=ReadBE64(value[1]);
        plan->outcome=OUTCOME_OK;
        return;
      }
   }
}

/* An ErrorResponse is fields of one type byte and a NUL terminated string ; 'C' is the SQLSTATE. */
static enum Outcome ErrorOutcome(const char * message)
{
  const char * end = message + 1 + ReadBE32(message+1);
  const char * at  = message + 5;
  while ( (at<end) && (*at!=0) )
   {
     char field=*at++;
     size_t length=strnlen(at,(size_t)(end-at));
     if ( (field=='C') && (length==5) && (memcmp(at,SQLSTATE_FOREIGN_KEY_VIOLATION,5)==0) ) { return OUTCOME_MISSING_POST; }
     at+=length+1;
   }
  return OUTCOME_ERROR;
}

/* Answers what the complete messages in a connection's buffer settle. 0 if the connection has to go , or if
 * a response could not be handed to the transport and the rest has to wait for the next run. */
static int ParseResults(struct AmmServer_Instance * instance,struct DbConnection * c)
{
  for (;;)
   {
     if (c->inUsed-c->inScan<5) { break; }

     const char * message=c->in+c->inScan;
     uint32_t length=ReadBE32(message+1);
     if ( (length<4) || (length>=DB_IN_CAPACITY) ) { c->broken=1; return 0; }
     if (c->inUsed-c->inScan<1+length) { break; }

     unsigned int next=c->inScan+1+length;
     char type=message[0];

     /* Rows wait where they are for their CommandComplete. BindComplete says nothing ; a notice , a
      * parameter status or a notification can turn up anywhere and is not ours to act on. */
     if ( (type=='D') || (type=='2') || (type=='N') || (type=='S') || (type=='A') ) { c->inScan=next; continue; }
     if ( (type!='C') && (type!='E') && (type!='Z') ) { c->broken=1; return 0; }

     if (stashHead!=0) { c->leftover=1; WakeDatabase(); return 0; }

     if (type=='Z')
      {
        /* The Sync that ends the group at the head of the flight. Whatever is still in front of its marker
         * got no reply : an error earlier in the group had Postgres skip it. */
        unsigned int unanswered=0;
        while ( (unanswered<c->count) && (FlightAt(c,unanswered)->statement!=NO_STATEMENT) ) { ++unanswered; }
        if (unanswered==c->count) { c->broken=1; return 0; }

        c->inScan=next;
        c->inKeep=next;

        if (c->isWriter) { EndGroup(instance,c,unanswered); }
        else
         {
           while (unanswered>0)
            {
              struct PlannedRequest plan=*FlightAt(c,0);
              c->head=(c->head+1)&(FLIGHT_CAPACITY-1);
              --c->count;
              --unanswered;
              plan.outcome=OUTCOME_NONE;
              Answer(instance,&plan,0,0);
            }
           c->head=(c->head+1)&(FLIGHT_CAPACITY-1);
           --c->count;
         }

        if (c->broken) { return 0; }
        continue;
      }

     /* CommandComplete or ErrorResponse : the outcome of the first statement that has none yet. */
     unsigned int index = c->isWriter ? c->groupDone : 0;
     if (index>=c->count) { c->broken=1; return 0; } /* an error nobody asked for : the server is going away */
     struct PlannedRequest * plan=FlightAt(c,index);

     if (plan->statement==NO_STATEMENT)
      {
        /* Only an error can come where a ReadyForQuery was due : the commit itself failed. */
        if (type!='E') { c->broken=1; return 0; }
        c->groupFailed=1;
        c->commitFailed=1;
        c->inScan=next;
        c->inKeep=next;
        continue;
      }

     if (c->isWriter)
      {
        if (type=='C') { TakeWriteOutcome(plan,c->in+c->inKeep,message); } else { plan->outcome=(unsigned char) ErrorOutcome(message); }
        if (plan->outcome!=OUTCOME_OK) { c->groupFailed=1; }
        ++c->groupDone;
      }
     else
      {
        struct PlannedRequest answered=*plan;
        c->head=(c->head+1)&(FLIGHT_CAPACITY-1);
        --c->count;
        answered.outcome=(unsigned char)( (type=='C') ? OUTCOME_OK : OUTCOME_ERROR );
        Answer(instance,&answered,c->in+c->inKeep,message);
      }

     c->inScan=next;
     c->inKeep=next;
   }

  return 1;
}

/* Takes what has arrived on a connection and answers what it completes. The socket is read until it is
 * empty : this runs once per pass of the ring , and a connection must be able to deliver a whole pass's
 * results however many they are , or its backend waits on a full socket in between. */
static void ReadResults(struct AmmServer_Instance * instance,struct DbConnection * c)
{
  if (c->leftover)
   {
     c->leftover=0;
     if (!ParseResults(instance,c)) { return; }
   }

  for (;;)
   {
     /* Everything before inKeep has been answered. */
     if (c->inKeep==c->inUsed) { c->inKeep=0; c->inScan=0; c->inUsed=0; } else
     if ( (c->inKeep>0) && (DB_IN_CAPACITY-c->inUsed<DB_IN_CAPACITY/2) )
      {
        memmove(c->in,c->in+c->inKeep,c->inUsed-c->inKeep);
        c->inUsed-=c->inKeep;
        c->inScan-=c->inKeep;
        c->inKeep=0;
      }

     unsigned int space=DB_IN_CAPACITY-c->inUsed;
     if (space==0) { c->broken=1; return; } /* one statement's result that does not fit : not one of ours */

     ssize_t got=recv(c->fd,c->in+c->inUsed,space,MSG_DONTWAIT);
     if (got==0) { c->broken=1; return; }
     if (got<0)
      {
        if (errno==EINTR) { continue; }
        if ( (errno!=EAGAIN) && (errno!=EWOULDBLOCK) ) { c->broken=1; }
        return;
      }

     c->inUsed+=(unsigned int) got;
     if (!ParseResults(instance,c)) { return; }
     if ((unsigned int) got<space) { return; } /* the socket had less than was asked for : it is empty */
   }
}

/* Closes a connection that broke and answers everything in its flight 500 ( 503 for /health ). Nothing is
 * replayed : a write whose Sync never came back may or may not have committed , and sending it again could
 * duplicate it. EnsureConnections opens the connection again. */
static void FailConnection(struct AmmServer_Instance * instance,struct DbConnection * c)
{
  /* Closing the socket takes it out of the epoll set. libpq writes its Terminate first , which the server
   * reads as the end whatever came before it. */
  PQfinish(c->pg);
  c->pg=0;
  c->fd=-1;
  c->broken=0;
  c->leftover=0;
  c->wantsWrite=0;
  c->groupDone=0;
  c->groupFailed=0;
  c->commitFailed=0;
  c->outUsed=0; c->outSent=0;
  c->inUsed=0; c->inScan=0; c->inKeep=0;

  int kind=0;
  for (kind=0; kind<2; kind++) { if (openGroup[kind]==c) { openGroup[kind]=0; openGroupSize[kind]=0; } }

  while (c->count>0)
   {
     struct PlannedRequest plan=*FlightAt(c,0);
     c->head=(c->head+1)&(FLIGHT_CAPACITY-1);
     --c->count;
     if (plan.statement==NO_STATEMENT) { continue; }

     plan.outcome=OUTCOME_NONE;
     Answer(instance,&plan,0,0);
   }
}

/* Runs on the ring thread when the epoll set has something : results from Postgres , room to write to it ,
 * or a wake-up asked for with WakeDatabase. */
static void DrainDatabase(struct AmmServer_Instance * instance)
{
  uint64_t counter=0;
  ssize_t drained=read(dbWakeFd,&counter,sizeof(counter));
  (void) drained;

  /* A group whose last handler call was a full one is still waiting for its Sync. */
  CloseGroup(0);
  CloseGroup(1);

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

/* Everything that became ready in one pass over the ring , at most AMMSERVER_API_MAX_BATCH requests a call.
 * Each request is planned ; the reads then join the pass's read group and the writes its write group , and
 * the answers are sent from DrainDatabase when the results are back. A call that is not full is the pass's
 * last , so the groups get their Sync and go out ; after a full one more may follow , so they stay open ,
 * and DrainDatabase is woken to close them in case nothing does. */
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

  SendGroup(0,responses,planScratch,readWhich,readCount);
  SendGroup(1,responses,planScratch,writeWhich,writeCount);

  if (count<AMMSERVER_API_MAX_BATCH) { CloseGroup(0); CloseGroup(1); } else
  if ( (openGroup[0]!=0) || (openGroup[1]!=0) ) { WakeDatabase(); }
}

/* What AmmarServer calls for a request it does not batch : every request on the epoll transport , which has
 * no deferred responses , and the few that arrive on a ring before its batch handler is in place. It expects
 * a finished response when this returns , so the statement runs through libpq on a blocking connection of
 * its own , opened the first time it is needed : one round trip per request , used only where the transport
 * cannot do better. PQexecPrepared returns after the statement's own transaction has committed ( rule 8 ). */
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

  PGresult * r=0;
  if (syncPg!=0)
   {
     const char * values[2];
     int lengths[2];
     PlanParameters(&plan,values,lengths);
     r=PQexecPrepared(syncPg,statementName[plan.statement],plan.parameters,values,lengths,binaryParameters,1);

     if (PQstatus(syncPg)!=CONNECTION_OK)
      {
        /* Whatever came back cannot be trusted as an answer , and the next request gets a new connection. */
        if (r!=0) { PQclear(r); r=0; }
        PQfinish(syncPg);
        syncPg=0;
      }
   }

  /* The same outcome the wire code reads off a connection , here from libpq's result ( all binary ). */
  if (r!=0)
   {
     ExecStatusType status=PQresultStatus(r);
     const char * sqlState=PQresultErrorField(r,PG_DIAG_SQLSTATE);

     plan.outcome=OUTCOME_ERROR;
     if ( (sqlState!=0) && (strcmp(sqlState,SQLSTATE_FOREIGN_KEY_VIOLATION)==0) ) { plan.outcome=OUTCOME_MISSING_POST; } else
     if ( (plan.statement==STATEMENT_INSERT_LIKE) && (status==PGRES_COMMAND_OK) )
      {
        plan.outcome=OUTCOME_OK;
        plan.inserted=(strcmp(PQcmdTuples(r),"1")==0);
      } else
     if ( (plan.statement==STATEMENT_INSERT_POST) && (status==PGRES_TUPLES_OK) && (PQntuples(r)==1) )
      {
        plan.outcome=OUTCOME_OK;
        plan.newIdLength=(unsigned char)( PutUnsignedDecimal(plan.newId,(unsigned long long) ReadBE64(PQgetvalue(r,0,0))) - plan.newId );
        plan.createdAt=ReadBE64(PQgetvalue(r,0,1));
      } else
     if ( (plan.statement!=STATEMENT_INSERT_LIKE) && (plan.statement!=STATEMENT_INSERT_POST) && (status==PGRES_TUPLES_OK) ) { plan.outcome=OUTCOME_OK; }
   }

  struct RowSource source;
  source.rows = 0; source.rowsEnd = 0; source.result = r; source.resultRow = 0;

  BuildAnswer(res,&plan,(r!=0) ? &source : 0);
  ReleasePlan(&plan);
  if (r!=0) { PQclear(r); }
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
     c->fd       = -1;
     c->flight   = (struct PlannedRequest *) calloc(FLIGHT_CAPACITY,sizeof(*c->flight));
     c->in       = (char *) malloc(DB_IN_CAPACITY);
     c->out      = (char *) malloc(DB_OUT_INITIAL);
     c->outCapacity = DB_OUT_INITIAL;
     if ( (c->flight==0) || (c->in==0) || (c->out==0) ) { return 0; }
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
