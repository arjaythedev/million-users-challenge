/*
 * The million users challenge feed API, in C, on AmmarServer ( https://github.com/AmmarkoV/AmmarServer ),
 * against PostgreSQL through libpq.
 *
 * The process model is prefork : main() forks WORKERS processes and then only waits for them. Each worker
 * is AmmarServer in API mode on one io_uring ring - one thread owning every connection it accepts - with
 * DB_THREADS database threads behind it. Half of the workers listen on PORT and half on EXTRA_PORT, all
 * with SO_REUSEPORT, so the kernel spreads the incoming connections evenly over the workers of each port
 * and every worker owns its share for the connection's whole life.
 *
 * Why processes and threads : a request is a network round trip to the database box, and the ring thread
 * must never wait on one. It plans the request ( route , token , JSON , id ) and hands the statement to a
 * database thread through a lane queue ; those threads run the spec's queries with blocking libpq pipelines
 * exactly as the old single connection per worker did. The ring keeps accepting and reading while they wait.
 * Finished responses come back through a completion queue and the worker's eventfd , and are sent from the
 * ring thread by AmmServer_SetAPINotifyFD / AmmServer_APISendDeferred. The read lanes are independent , and
 * the last lane is reserved for writes , so a commit never delays a read. At a million users ( ~42,000
 * req/s ) each of 32 workers sees ~1,300 req/s over three read lanes , and no reader waits for another's
 * round trip.
 *
 * Everything a worker touches is its own - its ring, its database threads, their connections, prepared
 * statements, buffers and HMAC key schedule. The only state shared inside a worker is the plan and
 * completion queues between its ring thread and its database threads , one mutex each.
 *
 * Challenge rule 7 ( no caching across requests ) : every response below is built from a Postgres query
 * run while the request is being served, and every token's HMAC is recomputed from scratch. The only
 * things reused between requests are prepared statements ( query plans, explicitly allowed ), the
 * database connection, and the buffers the transport reads and writes through.
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
#include <pthread.h>
#include <sys/prctl.h>
#include <sys/resource.h>
#include <sys/wait.h>
#include <sys/eventfd.h>

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

#define DEFAULT_WORKERS 32
/* Rule 5 : at most 512 connections to Postgres in total. Each worker process runs this many database
 * threads, one connection each, so workers * DB_THREADS must stay at or below 512. The default splits into
 * read lanes and one write lane ( see StartDatabaseThreads ) ; the ring thread never blocks on a round trip. */
#define DEFAULT_DB_THREADS 4
#define MAX_DB_THREADS 8
#define MAX_PG_CONNECTIONS 512
#define PLAN_QUEUE_CAPACITY 2048
#define COMPLETION_QUEUE_CAPACITY 4096
#define DB_BATCH_MAX 32
#define DB_BATCH_MIN 8
#define DB_BATCH_WINDOW_US 1000
#define DB_BATCH_WINDOW_NS ((long long)DB_BATCH_WINDOW_US*1000LL)
#define MAX_WORKERS MAX_PG_CONNECTIONS

/* Only the feed ( 5.1 KB, ~2.2 KB gzipped ) is worth compressing. A single post is ~300 bytes, which gzip's
 * 18 bytes of framing and a near-empty dictionary barely shrink, so below this it goes out as it is. */
#define GZIP_MIN_SIZE 1024

/* Postgres type OIDs for the parameters, and the binary epoch of timestamptz. */
#define INT8OID 20
#define TEXTOID 25
#define POSTGRES_EPOCH_IN_UNIX_DAYS 10957LL
#define SQLSTATE_FOREIGN_KEY_VIOLATION "23503"

/* --- the work behind a request ---------------------------------------------------------------------
 * The ring thread plans every request ( route , token , JSON , id ) and hands the database-bound ones to a
 * lane of database threads. Each lane owns blocking libpq connections and prepared statements exactly as the
 * old single connection per worker did , but it no longer owns the ring : the ring keeps reading and
 * accepting while those threads wait on Postgres. Finished responses come back through a completion queue
 * and the worker's eventfd , and are sent from the ring thread by the AmmServer notify callback. */

enum Statement
{
  NO_STATEMENT = 0,
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
  char * body;                    /* POST /posts : malloc'd trimmed body, freed after its response is built */
  unsigned int bodyLength;
  long long userId;
  char username[1024];            /* copied out of the JWT scratch : the plan outlives the request buffer */
  unsigned int usernameLength;
  unsigned long long deferToken;  /* the transport's handle for this request */
  unsigned char wantGzip;
  PGresult * result;              /* set and cleared by the database thread that runs this plan */
};

/* One lane of plans. Producers are the ring thread ; consumers are one or more database threads. */
struct PlanQueue
{
  pthread_mutex_t lock;
  pthread_cond_t notEmpty;
  pthread_cond_t notFull;
  struct PlannedRequest * items;
  unsigned int capacity;
  unsigned int head;
  unsigned int tail;
  unsigned int count;
  int stopping;
};

/* One finished response on its way back to the ring thread. body is malloc'd here and freed after the
 * transport has copied it into a send ticket. */
struct Completion
{
  unsigned long long token;
  unsigned int status;
  char * body;
  unsigned int bodyLength;
  unsigned char gzip;
};

struct CompletionQueue
{
  pthread_mutex_t lock;
  pthread_cond_t notFull;
  struct Completion * items;
  unsigned int capacity;
  unsigned int head;
  unsigned int tail;
  unsigned int count;
  int stopping;
};

struct DbThread
{
  pthread_t thread;
  struct PlanQueue * queue;
  int isWriter;
  PGconn * pg;
  int statementsPrepared;
  char * bodyScratch;             /* one response body built here, then copied to the exact-size completion */
  char * gzipScratch;
};

static struct PlanQueue readQueue;
static struct PlanQueue writeQueue;
static struct CompletionQueue completionQueue;
static int completionEventFd = -1;
static struct DbThread dbThreads[MAX_DB_THREADS];
static unsigned int dbThreadCount = 0;
static unsigned int readThreadCount = 0;
static unsigned int writeThreadCount = 0;
static volatile sig_atomic_t dbStopping = 0;

/* Lane batching, tunable from the environment so the benchmark box can sweep without a rebuild. */
static unsigned int dbBatchMin = DB_BATCH_MIN;
static long long dbBatchWindowNs = DB_BATCH_WINDOW_NS;

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
static void PutPostObject(
                           struct AmmServer_APIResponse * res,
                           long long id,
                           const char * body,unsigned int bodyLength,
                           const char * createdAt,unsigned int createdAtLength,
                           const char * author,unsigned int authorLength,int authorIsJsonEscaped,
                           long long likeCount
                         )
{
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
static unsigned int TimestampAt(const PGresult * r,int row,int column,char out[32])
{
  long long microseconds = Int8At(r,row,column);
  long long seconds   = microseconds / 1000000;
  long long remainder = microseconds % 1000000;
  if (remainder<0) { remainder+=1000000; --seconds; } /* floor, for the ( never seen ) dates before 2000 */

  long long days = seconds / 86400;
  long long secondOfDay = seconds % 86400;
  if (secondOfDay<0) { secondOfDay+=86400; --days; }

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

  unsigned int second = (unsigned int) secondOfDay;
  char * at = out;
  at = PutDigits(at,(unsigned int) year,4);       *at++ = '-';
  at = PutDigits(at,month,2);                     *at++ = '-';
  at = PutDigits(at,day,2);                       *at++ = 'T';
  at = PutDigits(at,second/3600,2);               *at++ = ':';
  at = PutDigits(at,(second/60)%60,2);            *at++ = ':';
  at = PutDigits(at,second%60,2);                 *at++ = '.';
  at = PutDigits(at,(unsigned int)(remainder/1000),3);
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

static int Prepare(struct DbThread * thread,const char * name,const char * sql,int parameters,const Oid * types)
{
  PGresult * r = PQprepare(thread->pg,name,sql,parameters,types);
  int ok = (PQresultStatus(r)==PGRES_COMMAND_OK);
  if (!ok) { fprintf(stderr,"feedapi: could not prepare [%s] : %s",sql,PQerrorMessage(thread->pg)); }
  PQclear(r);
  return ok;
}

static int PrepareStatements(struct DbThread * thread)
{
  static const Oid oneInt8[1]      = { INT8OID };
  static const Oid twoInt8[2]      = { INT8OID , INT8OID };
  static const Oid int8AndText[2]  = { INT8OID , TEXTOID };

  /* posts_created_at_id_idx is (created_at DESC, id DESC), so this is 20 rows off the front of that index
   * rather than a sort of half a million posts. */
  if (!Prepare(thread,statementName[STATEMENT_FEED],SELECT_POST_COLUMNS "ORDER BY p.created_at DESC, p.id DESC LIMIT 20",0,0)) { return 0; }
  if (!Prepare(thread,statementName[STATEMENT_POST],SELECT_POST_COLUMNS "WHERE p.id = $1",1,oneInt8)) { return 0; }
  if (!Prepare(thread,statementName[STATEMENT_INSERT_POST],"INSERT INTO posts (user_id, body) VALUES ($1, $2) RETURNING id, created_at",2,int8AndText)) { return 0; }
  if (!Prepare(thread,statementName[STATEMENT_INSERT_LIKE],"INSERT INTO likes (user_id, post_id) VALUES ($1, $2) ON CONFLICT (user_id, post_id) DO NOTHING",2,twoInt8)) { return 0; }
  if (!Prepare(thread,statementName[STATEMENT_PING],"SELECT 1",0,0)) { return 0; }
  return 1;
}

/* The connection is opened before the worker starts serving, so normally this is one PQstatus. If the
 * connection has been lost it is reset ( or, after DropConnection, opened again ) and the statements
 * prepared again, so that a database that comes back is served again - rather than this worker answering
 * 500 to its share of the users for good.
 * @retval 1=ready to query , 0=the database is unreachable ( PQerrorMessage says why ) */
static PGconn * ConnectDatabase(void)
{
  /* A bounded connect attempt : a database that is down must fail this lane's requests , not leave the
   * thread and the queue behind it waiting on the kernel's TCP timeout. */
  static const char * const keywords[] = { "dbname" , "connect_timeout" , 0 };
  const char * const values[]         = { databaseUrl , "3" , 0 };
  return PQconnectdbParams(keywords,values,1);
}

static int DatabaseReady(struct DbThread * thread)
{
  if (thread->pg==0) { thread->pg = ConnectDatabase(); thread->statementsPrepared = 0; }
  if (thread->pg==0) { return 0; }

  if (PQstatus(thread->pg)!=CONNECTION_OK)
   {
     thread->statementsPrepared = 0;
     PQreset(thread->pg);
     if (PQstatus(thread->pg)!=CONNECTION_OK) { return 0; }
   }

  if (!thread->statementsPrepared)
   {
     PQsetClientEncoding(thread->pg,"UTF8");
     thread->statementsPrepared = PrepareStatements(thread);
   }

  return thread->statementsPrepared;
}

/* A pipeline that went wrong part way through leaves libpq waiting for results, and nothing more can be sent
 * on that connection until they arrive - which, on a connection that broke, they never will. So it is closed,
 * and DatabaseReady opens a new one for the next batch. */
static void DropConnection(struct DbThread * thread)
{
  PQfinish(thread->pg);
  thread->pg = 0;
  thread->statementsPrepared = 0;
}

/* --- request helpers -------------------------------------------------------------------------------- */

/**
 * Verify the request's bearer token, answering with the right 401 if it does not hold up. The claims are
 * copied into the plan, because the scratch the JWT decoder writes them into ends with this call while the
 * plan lives on in a database thread.
 * @retval 1=authenticated , 0=res now holds the 401 to send
 */
static int Authenticate(
                         struct AmmServer_APIRequest * rqst,
                         struct AmmServer_APIResponse * res,
                         struct PlannedRequest * plan
                       )
{
  unsigned int tokenLength=0;
  const char * token = AmmServer_APIRequestBearer(rqst,&tokenLength);

  /* No Authorization header at all, or one whose scheme is not Bearer. A Bearer header with nothing after
   * it comes back non-zero with length 0 instead, and is a broken credential rather than a missing one. */
  if (token==0) { Fail(res,401,"missing bearer token"); return 0; }

  char scratch[1024];
  struct JwtClaims claims;
  switch ( JwtVerifyToken(token,tokenLength,jwtSecret,jwtSecretLength,(long long) time(0),&claims,scratch,sizeof(scratch)) )
   {
     case JWT_OK          :
       plan->userId = claims.userId;
       plan->usernameLength = claims.usernameLength;
       if (plan->usernameLength>sizeof(plan->username)) { plan->usernameLength=(unsigned int) sizeof(plan->username); }
       memcpy(plan->username,claims.username,plan->usernameLength);
       return 1;
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

/* The ring thread does everything that needs no database : the route, the token, the body, the id. A request
 * that fails any of those is answered right there ; the rest become a PlannedRequest that a database thread
 * runs, and whose response is sent later from the ring thread. The plan is self-contained : the request's
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
  if (!Authenticate(rqst,res,plan)) { return; }

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

  /* The plan outlives the request buffer, so its body gets its own copy. Only POST /posts pays this. */
  plan->body = (char *) malloc(bodyLength?bodyLength:1);
  if (plan->body==0) { Fail(res,500,"internal server error"); return; }
  memcpy(plan->body,body,bodyLength);
  plan->bodyLength = bodyLength;
  Plan(plan,STATEMENT_INSERT_POST,2,plan->userId,0); /* the second parameter is the body, as text */
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
        if (!Authenticate(rqst,res,plan)) { return; }

        if (!AmmServer_APIRequestMatch(rqst,"/posts/%lld/like",&postId)) { Fail(res,400,"invalid post id"); return; }

        plan->postId = postId;
        Plan(plan,STATEMENT_INSERT_LIKE,2,plan->userId,postId);
        return;
      }
   }

  Fail(res,404,"not found");
}

/* --- the lanes -------------------------------------------------------------------------------------- */

static int PlanQueueInit(struct PlanQueue * queue,unsigned int capacity)
{
  memset(queue,0,sizeof(*queue));
  queue->items=(struct PlannedRequest *) calloc(capacity,sizeof(*queue->items));
  if (queue->items==0) { return 0; }
  queue->capacity=capacity;
  if (pthread_mutex_init(&queue->lock,0)!=0) { free(queue->items); queue->items=0; return 0; }
  if (pthread_cond_init(&queue->notEmpty,0)!=0) { free(queue->items); queue->items=0; return 0; }
  if (pthread_cond_init(&queue->notFull,0)!=0) { free(queue->items); queue->items=0; return 0; }
  return 1;
}

static void PlanQueueStop(struct PlanQueue * queue)
{
  pthread_mutex_lock(&queue->lock);
  queue->stopping=1;
  pthread_cond_broadcast(&queue->notEmpty);
  pthread_cond_broadcast(&queue->notFull);
  pthread_mutex_unlock(&queue->lock);
}

/* Takes ownership of plan->body : on a queue that is stopping or full it is freed here. A full queue is
 * answered 500 rather than waited on, because blocking the ring thread here would also stop it draining the
 * completion queue, and a database stall must not be able to deadlock both ends. The queues hold thousands
 * of plans, so only a real stall reaches this. */
static int PlanQueuePush(struct PlanQueue * queue,const struct PlannedRequest * plan)
{
  pthread_mutex_lock(&queue->lock);

  if ( (queue->stopping) || (queue->count==queue->capacity) )
   {
     pthread_mutex_unlock(&queue->lock);
     if (plan->body!=0) { free(plan->body); }
     return 0;
   }

  queue->items[queue->tail]=*plan;
  queue->tail=(queue->tail+1)%queue->capacity;
  ++queue->count;
  pthread_cond_signal(&queue->notEmpty);
  pthread_mutex_unlock(&queue->lock);
  return 1;
}

/* Pops what is ready, then waits up to DB_BATCH_WINDOW_NS for company if the batch is short : that is what
 * makes one round trip and one pipeline serve several requests instead of one. */
static unsigned int PlanQueuePopBatch(struct PlanQueue * queue,struct PlannedRequest * out,unsigned int maximum)
{
  pthread_mutex_lock(&queue->lock);
  while ( (queue->count==0) && (!queue->stopping) ) { pthread_cond_wait(&queue->notEmpty,&queue->lock); }

  unsigned int count=0;
  while ( (count<maximum) && (queue->count>0) )
   {
     out[count++]=queue->items[queue->head];
     queue->head=(queue->head+1)%queue->capacity;
     --queue->count;
   }
  pthread_cond_broadcast(&queue->notFull);

  if ( (count>0) && (count<dbBatchMin) && (dbBatchWindowNs>0) && (!queue->stopping) )
   {
     struct timespec deadline;
     clock_gettime(CLOCK_REALTIME,&deadline);
     deadline.tv_nsec+=dbBatchWindowNs;
     while (deadline.tv_nsec>=1000000000L) { deadline.tv_nsec-=1000000000L; ++deadline.tv_sec; }

     while ( (queue->count==0) && (!queue->stopping) )
      {
        if (pthread_cond_timedwait(&queue->notEmpty,&queue->lock,&deadline)==ETIMEDOUT) { break; }
      }

     while ( (count<maximum) && (queue->count>0) )
      {
        out[count++]=queue->items[queue->head];
        queue->head=(queue->head+1)%queue->capacity;
        --queue->count;
      }
     pthread_cond_broadcast(&queue->notFull);
   }

  pthread_mutex_unlock(&queue->lock);
  return count;
}

static int CompletionQueueInit(struct CompletionQueue * queue,unsigned int capacity)
{
  memset(queue,0,sizeof(*queue));
  queue->items=(struct Completion *) calloc(capacity,sizeof(*queue->items));
  if (queue->items==0) { return 0; }
  queue->capacity=capacity;
  if (pthread_mutex_init(&queue->lock,0)!=0) { free(queue->items); queue->items=0; return 0; }
  if (pthread_cond_init(&queue->notFull,0)!=0) { free(queue->items); queue->items=0; return 0; }
  return 1;
}

static void CompletionQueueStop(struct CompletionQueue * queue)
{
  pthread_mutex_lock(&queue->lock);
  queue->stopping=1;
  pthread_cond_broadcast(&queue->notFull);
  pthread_mutex_unlock(&queue->lock);
}

/* Takes ownership of completion->body. Drops it if the worker is stopping and nobody is draining. */
static void CompletionPush(struct CompletionQueue * queue,const struct Completion * completion)
{
  pthread_mutex_lock(&queue->lock);
  while ( (queue->count==queue->capacity) && (!queue->stopping) ) { pthread_cond_wait(&queue->notFull,&queue->lock); }

  if (queue->stopping)
   {
     pthread_mutex_unlock(&queue->lock);
     if (completion->body!=0) { free(completion->body); }
     return;
   }

  queue->items[queue->tail]=*completion;
  queue->tail=(queue->tail+1)%queue->capacity;
  ++queue->count;
  pthread_mutex_unlock(&queue->lock);

  if (completionEventFd>=0)
   {
     uint64_t one=1;
     ssize_t written=write(completionEventFd,&one,sizeof(one));
     (void) written;
   }
}

static void AnswerHealth(struct AmmServer_APIResponse * res,const PGresult * r,struct DbThread * thread)
{
  /* The spec wants the ok answer to mean a SELECT 1 actually came back, so it is run per request. */
  if ( (r==0) || (PQresultStatus(r)!=PGRES_TUPLES_OK) )
   {
     const char * message = (r!=0) ? PQresultErrorMessage(r) : (thread->pg!=0) ? PQerrorMessage(thread->pg) : "the connection to the database was lost";
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
  PutPostObject(res,Int8At(r,0,0),plan->body,plan->bodyLength,createdAt,createdAtLength,plan->username,plan->usernameLength,1,0);
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

/* Read while the request bytes are still in hand, on the ring thread : the plan outlives them. */
static int AcceptsGzip(const struct AmmServer_APIRequest * rqst)
{
  unsigned int valueLength=0;
  const char * value = AmmServer_APIRequestHeader(rqst,"Accept-Encoding",&valueLength);
  return ( (value!=0) && (memmem(value,valueLength,"gzip",4)!=0) );
}

/* ISA-L igzip at level 0 : one hash lookup per position and no lazy matching, which is the fastest setting
 * and enough for a feed that goes out as two TCP segments either way. Stateless, into this database
 * thread's own scratch buffer, and only kept when it actually made the response smaller. */
static void GzipResponse(struct AmmServer_APIResponse * res,struct DbThread * thread)
{
  if (thread->gzipScratch==0) { return; }

  struct isal_zstream stream;
  isal_deflate_stateless_init(&stream);
  stream.next_in   = (uint8_t *) res->body;
  stream.avail_in  = res->bodyLength;
  stream.next_out  = (uint8_t *) thread->gzipScratch;
  stream.avail_out = (uint32_t) MAX_RESPONSE_SIZE;
  stream.gzip_flag = IGZIP_GZIP;
  stream.level     = 0;

  if (isal_deflate_stateless(&stream)!=COMP_OK) { return; }

  size_t compressedLength = (size_t) MAX_RESPONSE_SIZE - stream.avail_out;
  if ( (compressedLength==0) || (compressedLength>=res->bodyLength) ) { return; }

  memcpy(res->body,thread->gzipScratch,compressedLength);
  res->bodyLength   = (unsigned int) compressedLength;
  res->extraHeaders = "Content-Encoding: gzip\r\n";
}

/* --- a plan answered on a database thread ----------------------------------------------------------- */

/* One response, built here and handed to the ring thread as an exact-size body. A plan whose result is 0
 * never reached Postgres ; /health says so with its 503 , everything else is a 500. */
static void CompletePlan(struct DbThread * thread,struct PlannedRequest * plan)
{
  struct AmmServer_APIResponse res;
  memset(&res,0,sizeof(res));
  res.statusCode    = 200;
  res.contentType   = "application/json";
  res.body          = thread->bodyScratch;
  res.maxBodyLength = MAX_RESPONSE_SIZE;

  switch (plan->statement)
   {
     case STATEMENT_PING        : AnswerHealth(&res,plan->result,thread); break;
     case STATEMENT_FEED        : if (plan->result==0) { Fail(&res,500,"internal server error"); } else { AnswerFeed(&res,plan->result); } break;
     case STATEMENT_POST        : if (plan->result==0) { Fail(&res,500,"internal server error"); } else { AnswerPost(&res,plan->result); } break;
     case STATEMENT_INSERT_POST : if (plan->result==0) { Fail(&res,500,"internal server error"); } else { AnswerCreatePost(&res,plan); } break;
     case STATEMENT_INSERT_LIKE : if (plan->result==0) { Fail(&res,500,"internal server error"); } else { AnswerLike(&res,plan); } break;
     case NO_STATEMENT          : Fail(&res,500,"internal server error"); break;
   };

  if (AmmServer_APIResponseOverflowed(&res)) { Fail(&res,500,"internal server error"); }

  unsigned char gzip=0;
  if ( (plan->wantGzip) && (res.bodyLength>=GZIP_MIN_SIZE) )
   {
     GzipResponse(&res,thread);
     if (res.extraHeaders!=0) { gzip=1; }
   }

  char * body=(char *) malloc(res.bodyLength?res.bodyLength:1);
  if (body!=0) { memcpy(body,res.body,res.bodyLength); }

  struct Completion completion;
  completion.token      = plan->deferToken;
  completion.status     = (body!=0) ? res.statusCode : 500;
  completion.body       = body;
  completion.bodyLength = (body!=0) ? res.bodyLength : 0;
  completion.gzip       = gzip;
  CompletionPush(&completionQueue,&completion);

  if (plan->result!=0) { PQclear(plan->result); plan->result=0; }
  if (plan->body!=0)   { free(plan->body); plan->body=0; }
}

/* Every planned statement goes out before any result is read, each followed by its own Sync. A Sync ends
 * the implicit transaction, so every statement commits ( or fails ) on its own exactly as it would outside a
 * pipeline : one like on a missing post cannot roll back another user's new post. */
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

static int SendPlan(PGconn * pg,const struct PlannedRequest * plan,const int binary[2])
{
  const char * values[2]  = { (const char *) &plan->parameter[0] , (const char *) &plan->parameter[1] };
  int          lengths[2] = { sizeof(uint64_t) , sizeof(uint64_t) };

  /* The body goes in binary too, which for text is just its bytes and a length, so it never has to be
   * NUL terminated. */
  if (plan->statement==STATEMENT_INSERT_POST) { values[1] = plan->body; lengths[1] = (int) plan->bodyLength; }

  return PQsendQueryPrepared(pg,statementName[plan->statement],plan->parameters,values,lengths,binary,1);
}

/* One write on its own, outside pipeline mode, used only after a grouped write transaction rolled back.
 * PQexecPrepared commits it before returning, so its response can be sent immediately. */
static PGresult * RunOneWrite(struct DbThread * thread,const struct PlannedRequest * plan)
{
  static const int binary[2] = { 1 , 1 };
  const char * values[2]  = { (const char *) &plan->parameter[0] , (const char *) &plan->parameter[1] };
  int          lengths[2] = { sizeof(uint64_t) , sizeof(uint64_t) };

  if (plan->statement==STATEMENT_INSERT_POST) { values[1] = plan->body; lengths[1] = (int) plan->bodyLength; }

  return PQexecPrepared(thread->pg,statementName[plan->statement],plan->parameters,values,lengths,binary,1);
}

/* Results from a batch whose fate is not known must not be reported as successes. */
static void ClearBatchResults(struct PlannedRequest * batch,unsigned int count)
{
  unsigned int i=0;
  for (i=0; i<count; i++)
   {
     if (batch[i].result!=0) { PQclear(batch[i].result); batch[i].result=0; }
   }
}

/* A grouped write transaction aborted, so every write in the batch was rolled back. Run them one at a time
 * instead: the valid posts and likes commit, and a like on a missing post only fails itself. A connection
 * lost during a retry is not retried again - the client gets 500 rather than a possible duplicate. */
static void RetryWrites(struct DbThread * thread,struct PlannedRequest * batch,unsigned int count)
{
  unsigned int i=0;
  for (i=0; i<count; i++)
   {
     struct PlannedRequest * plan=&batch[i];
     if (plan->statement==NO_STATEMENT) { CompletePlan(thread,plan); continue; }
     if (thread->pg==0)                 { CompletePlan(thread,plan); continue; }

     /* The aborted batch's result for this plan is replaced by the individual retry's. */
     if (plan->result!=0) { PQclear(plan->result); plan->result=0; }

     plan->result = RunOneWrite(thread,plan);
     if (plan->result==0)
      {
        CompletePlan(thread,plan);
        if ( (thread->pg==0) || (PQstatus(thread->pg)!=CONNECTION_OK) ) { DropConnection(thread); }
        continue;
     }

     CompletePlan(thread,plan);
   }
}

/* Every statement of one lane batch is followed by a single Sync. That Sync is one protocol flush and one
 * implicit transaction : the reads in a read lane share a snapshot and one round trip, and the writes in the
 * write lane commit together ( rule 3 allows batching writes into one transaction ; rule 8 is still kept ,
 * every response is held until that commit's result is in ). A write transaction that aborts is then
 * replayed one write at a time, so one bad like does not take the rest of the batch with it. */
static void RunBatch(struct DbThread * thread,struct PlannedRequest * batch,unsigned int count)
{
  static const int binary[2] = { 1 , 1 };

  unsigned int wanted=0;
  unsigned int i=0;
  for (i=0; i<count; i++) { if (batch[i].statement!=NO_STATEMENT) { ++wanted; } }
  if (wanted==0) { return; }

  if (!DatabaseReady(thread))
   {
     /* Every result stays 0 , which CompletePlan answers as a database that cannot be reached. */
     for (i=0; i<count; i++) { CompletePlan(thread,&batch[i]); }
     return;
   }
  if (!PQenterPipelineMode(thread->pg))
   {
     for (i=0; i<count; i++) { CompletePlan(thread,&batch[i]); }
     return;
   }

  int sendFailed=0;
  unsigned int sent=0;
  for (i=0; i<count; i++)
   {
     struct PlannedRequest * plan = &batch[i];
     if (plan->statement==NO_STATEMENT) { continue; }
     if (!SendPlan(thread->pg,plan,binary)) { sendFailed=1; break; }
     ++sent;
   }

  if ( (!sendFailed) && (sent>0) && (!PQpipelineSync(thread->pg)) ) { sendFailed=1; }

  if (sendFailed)
   {
     /* Nothing here reached a Sync , so nothing can have committed. */
     DropConnection(thread);
     for (i=0; i<count; i++) { CompletePlan(thread,&batch[i]); }
     return;
   }

  /* Per statement : its result , then the NULL that ends it. After the last one , the Sync's result. */
  unsigned int received=0;
  int batchOk=1;
  for (i=0; (i<count) && (received<sent); i++)
   {
     struct PlannedRequest * plan = &batch[i];
     if (plan->statement==NO_STATEMENT) { continue; }

     plan->result = PQgetResult(thread->pg);
     if (plan->result==0) { break; }

     PGresult * extra=0;
     while ( (extra=PQgetResult(thread->pg))!=0 ) { PQclear(extra); }

     if (!PlanSucceeded(plan)) { batchOk=0; }
     ++received;
   }

  int synced=0;
  if (received==sent)
   {
     PGresult * sync=PQgetResult(thread->pg);
     synced = ( (sync!=0) && (PQresultStatus(sync)==PGRES_PIPELINE_SYNC) );
     PQclear(sync);
   }

  int exited=0;
  if (synced) { exited=PQexitPipelineMode(thread->pg); }

  if ( (received<sent) || (!synced) )
   {
     /* No Sync result. A write may or may not have committed , and replaying it could duplicate it : clear
      * whatever partial results there are , answer 500 and leave it. */
     DropConnection(thread);
     ClearBatchResults(batch,count);
     for (i=0; i<count; i++) { CompletePlan(thread,&batch[i]); }
     return;
   }

  if (!exited)
   {
     /* The Sync came back , so the transaction definitely ended ; the results are valid. The connection
      * cannot take another pipeline , so drop it. An aborted write batch cannot be replayed here ; its
      * writes were rolled back , so they are answered 500 rather than with stale successes. */
     DropConnection(thread);
     if ( (thread->isWriter) && (!batchOk) ) { ClearBatchResults(batch,count); }
     for (i=0; i<count; i++) { CompletePlan(thread,&batch[i]); }
     return;
   }

  if ( (thread->isWriter) && (!batchOk) )
   {
     RetryWrites(thread,batch,count);
     return;
   }

  for (i=0; i<count; i++) { CompletePlan(thread,&batch[i]); }
}

static void * DbThreadMain(void * argument)
{
  struct DbThread * thread = (struct DbThread *) argument;

  thread->bodyScratch = (char *) malloc(MAX_RESPONSE_SIZE);
  thread->gzipScratch = (char *) malloc(MAX_RESPONSE_SIZE);

  if ( (thread->bodyScratch==0) || (thread->gzipScratch==0) )
   {
     /* Without its response buffer this thread cannot answer anything ; losing the worker is better than
      * leaving its share of the users waiting forever. */
     fprintf(stderr,"feedapi: database thread could not allocate its response buffers\n");
     _exit(1);
   }

  struct PlannedRequest batch[DB_BATCH_MAX];
  while (!dbStopping)
   {
     unsigned int count = PlanQueuePopBatch(thread->queue,batch,DB_BATCH_MAX);
     if (count==0) { continue; }
     RunBatch(thread,batch,count);
   }

  if (thread->pg!=0) { PQfinish(thread->pg); thread->pg=0; }
  free(thread->bodyScratch); thread->bodyScratch=0;
  free(thread->gzipScratch); thread->gzipScratch=0;
  return 0;
}

/* How many database threads a worker runs : at least one read lane and, when there is room for two, one
 * write lane so a commit never delays a read. The total across all workers stays within rule 5's 512. */
/* DB_BATCH_US is the coalescing window a short lane batch waits for company ; DB_BATCH_MIN is the batch
 * size at which it stops waiting. 0 for either disables that part. Read once, before the workers fork. */
static void ReadBatchingTuning(void)
{
  const char * text=getenv("DB_BATCH_US");
  if ( (text!=0) && (text[0]!=0) )
   {
     long value=strtol(text,0,10);
     if ( (value>=0) && (value<=100000) ) { dbBatchWindowNs=(long long) value*1000LL; }
   }

  text=getenv("DB_BATCH_MIN");
  if ( (text!=0) && (text[0]!=0) )
   {
     long value=strtol(text,0,10);
     if ( (value>=0) && (value<=DB_BATCH_MAX) ) { dbBatchMin=(unsigned int) value; }
   }
}

static unsigned int DesiredDatabaseThreads(unsigned int workers)
{
  unsigned int wanted=DEFAULT_DB_THREADS;
  const char * text=getenv("DB_THREADS");
  if ( (text!=0) && (text[0]!=0) ) { wanted=(unsigned int) strtoul(text,0,10); }

  if (wanted<1) { wanted=1; }
  if (wanted>MAX_DB_THREADS) { wanted=MAX_DB_THREADS; }

  unsigned int maximum=(workers>0) ? (MAX_PG_CONNECTIONS/workers) : MAX_PG_CONNECTIONS;
  if (maximum<1) { maximum=1; }
  if (wanted>maximum) { wanted=maximum; }
  return wanted;
}

static int StartDatabaseThreads(unsigned int wanted)
{
  if (wanted<1) { return 0; }

  dbStopping=0;
  if (!PlanQueueInit(&readQueue,PLAN_QUEUE_CAPACITY)) { return 0; }
  if (!PlanQueueInit(&writeQueue,PLAN_QUEUE_CAPACITY)) { return 0; }
  if (!CompletionQueueInit(&completionQueue,COMPLETION_QUEUE_CAPACITY)) { return 0; }

  completionEventFd=eventfd(0,EFD_NONBLOCK|EFD_CLOEXEC);
  if (completionEventFd<0) { fprintf(stderr,"feedapi: could not create the completion eventfd (%s)\n",strerror(errno)); return 0; }

  dbThreadCount=wanted;
  if (wanted>1) { writeThreadCount=1; readThreadCount=wanted-1; }
  else          { writeThreadCount=0; readThreadCount=1; }

  unsigned int i=0;
  for (i=0; i<wanted; i++)
   {
     struct DbThread * thread=&dbThreads[i];
     memset(thread,0,sizeof(*thread));
     thread->isWriter = ( (writeThreadCount>0) && (i==wanted-1) );
     thread->queue    = thread->isWriter ? &writeQueue : &readQueue;

     if (pthread_create(&thread->thread,0,DbThreadMain,thread)!=0)
      {
        fprintf(stderr,"feedapi: worker %d could not start database thread %u (%s)\n",getpid(),i,strerror(errno));
        dbStopping=1;
        PlanQueueStop(&readQueue);
        PlanQueueStop(&writeQueue);
        CompletionQueueStop(&completionQueue);
        unsigned int j=0;
        for (j=0; j<i; j++) { pthread_join(dbThreads[j].thread,0); }
        dbThreadCount=0;
        close(completionEventFd); completionEventFd=-1;
        return 0;
      }
   }
  return 1;
}

static void DrainPlanQueue(struct PlanQueue * queue)
{
  pthread_mutex_lock(&queue->lock);
  while (queue->count>0)
   {
     struct PlannedRequest * plan=&queue->items[queue->head];
     if (plan->body!=0)   { free(plan->body); }
     if (plan->result!=0) { PQclear(plan->result); }
     queue->head=(queue->head+1)%queue->capacity;
     --queue->count;
   }
  pthread_mutex_unlock(&queue->lock);
}

static void StopDatabaseThreads(void)
{
  if (dbThreadCount==0) { return; }

  dbStopping=1;
  PlanQueueStop(&readQueue);
  PlanQueueStop(&writeQueue);
  CompletionQueueStop(&completionQueue);

  unsigned int i=0;
  for (i=0; i<dbThreadCount; i++) { pthread_join(dbThreads[i].thread,0); }

  DrainPlanQueue(&readQueue);
  DrainPlanQueue(&writeQueue);

  pthread_mutex_lock(&completionQueue.lock);
  while (completionQueue.count>0)
   {
     struct Completion * completion=&completionQueue.items[completionQueue.head];
     if (completion->body!=0) { free(completion->body); }
     completionQueue.head=(completionQueue.head+1)%completionQueue.capacity;
     --completionQueue.count;
   }
  pthread_mutex_unlock(&completionQueue.lock);

  dbThreadCount=0;
  if (completionEventFd>=0) { close(completionEventFd); completionEventFd=-1; }
}

/* Runs on the ring thread when the completion eventfd is readable : sends every finished response through
 * the transport. A response that finds every send ticket busy stays at the head of the queue and the fd is
 * written again, so the ring comes back for it. */
static void DrainCompletions(struct AmmServer_Instance * instance)
{
  pthread_mutex_lock(&completionQueue.lock);
  while (completionQueue.count>0)
   {
     struct Completion * completion=&completionQueue.items[completionQueue.head];

     if (!AmmServer_APISendDeferred(instance,
                                    completion->token,
                                    completion->status,
                                    "application/json",
                                    (completion->gzip!=0) ? "Content-Encoding: gzip\r\n" : 0,
                                    completion->body,
                                    completion->bodyLength))
      {
        if (completionEventFd>=0)
         {
           uint64_t one=1;
           ssize_t written=write(completionEventFd,&one,sizeof(one));
           (void) written;
         }
        break;
      }

     if (completion->body!=0) { free(completion->body); completion->body=0; }
     completionQueue.head=(completionQueue.head+1)%completionQueue.capacity;
     --completionQueue.count;
     pthread_cond_signal(&completionQueue.notFull);
   }
  pthread_mutex_unlock(&completionQueue.lock);
}

/* The ring thread only plans and hands over. Everything below the plan - the query , the response , gzip -
 * happens on a database thread while the ring keeps reading and accepting. */
static void HandleBatch(struct AmmServer_APIRequest * requests,struct AmmServer_APIResponse * responses,unsigned int count)
{
  unsigned int i=0;
  for (i=0; i<count; i++) { PlanRequest(&requests[i],&responses[i],&planScratch[i]); }

  for (i=0; i<count; i++)
   {
     struct PlannedRequest * plan=&planScratch[i];
     if (plan->statement==NO_STATEMENT) { continue; } /* PlanRequest answered it already */

     if (dbThreadCount==0)
      {
        if (plan->body!=0) { free(plan->body); plan->body=0; }
        Fail(&responses[i],500,"internal server error");
        continue;
      }

     plan->deferToken = responses[i].deferToken;
     plan->wantGzip   = (unsigned char) AcceptsGzip(&requests[i]);

     int write = ( (plan->statement==STATEMENT_INSERT_POST) || (plan->statement==STATEMENT_INSERT_LIKE) );
     struct PlanQueue * queue = ( (write) && (writeThreadCount>0) ) ? &writeQueue : &readQueue;

     if (!PlanQueuePush(queue,plan))
      {
        Fail(&responses[i],500,"internal server error");
        continue;
      }

     responses[i].deferred = 1;
   }
}

/* What AmmarServer calls for a request it does not batch ( the few answered before the batch handler is in
 * place ) : a batch of one, so the answers are the same either way. */
static void HandleRequest(struct AmmServer_APIRequest * rqst,struct AmmServer_APIResponse * res)
{
  HandleBatch(rqst,res,1);
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

static int RunWorker(const char * host,unsigned int port,unsigned int databaseThreads)
{
  /* The database threads, their connections and the completion eventfd are all made here, after the fork ,
   * so that nothing a worker uses was ever shared with another process. */
  if (!StartDatabaseThreads(databaseThreads))
   {
     fprintf(stderr,"feedapi: worker %d could not start its database threads , answering 500\n",getpid());
   }

  /* An empty config path , not 0 , so that AmmarServer's automatic lookup cannot pick up a stray
   * ammarServer.conf or /etc/ammarServer.conf and quietly change the port or the thread settings underneath
   * the benchmark. Everything this process needs comes from the environment.
   * One ring : a worker holds ~1/16th of a port's connections ( ~31,000 at a million users ), which one
   * ring's table ( 256k slots ) is plenty for. ReusingPort : the listening socket gets SO_REUSEPORT before it is bound, so
   * every worker on this port binds its own socket and the kernel spreads new connections over them. */
  struct AmmServer_Instance * instance =
      AmmServer_StartAPIOnRingsReusingPort("feedapi",host,port,"/dev/null",(void*) &HandleRequest,MAX_RESPONSE_SIZE,1);
  if (instance==0) { fprintf(stderr,"feedapi: worker %d could not start AmmarServer on %s:%u\n",getpid(),host,port); return 1; }

  /* Before the batch handler : a handler that defers a request needs the transport already polling the
   * eventfd its completion will be announced on. The fd may be -1 if the database threads did not start. */
  AmmServer_SetAPINotifyFD(instance,completionEventFd,DrainCompletions);

  /* From here on the ring answers whatever is ready in one pass as one batch ( see HandleBatch ). The few
   * requests that could have arrived before this line get HandleRequest, which is a batch of one. */
  AmmServer_SetAPIBatchHandler(instance,(void*) &HandleBatch);

  /* Every request is planned on AmmarServer's ring thread ; the database work happens on its own threads. */
  while (AmmServer_Running(instance)) { sleep(1); }

  AmmServer_Stop(instance);
  StopDatabaseThreads();
  return 0;
}

/* The worker count is about concurrent round trips, not CPU. Each worker now has three read lanes and one
 * write lane, so 32 workers already keep ~100 database round trips in flight; the local sweep in README.md
 * found 32 workers consistently ahead of 64 at the same load. Keep it a knob, since the rig is the judge. */
static unsigned int DefaultWorkerCount(void)
{
  return DEFAULT_WORKERS;
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
  unsigned int databaseThreads=DesiredDatabaseThreads(workers);
  ReadBatchingTuning();

  RaiseDescriptorLimit();

  fprintf(stderr,"feedapi: AmmarServer %s , libpq %d , ISA-L igzip , %u workers on %s port %u",
          AmmServer_Version(),PQlibVersion(),workers,host,ports[0]);
  if (portCount>1) { fprintf(stderr," and %u",ports[1]); }
  fprintf(stderr," , %u database threads per worker , batch %u/%lldus\n",databaseThreads,dbBatchMin,dbBatchWindowNs/1000);

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
        _exit( RunWorker(host,ports[i%portCount],databaseThreads) );
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
