/*
 * The million users challenge feed API, in C, on AmmarServer ( https://github.com/AmmarkoV/AmmarServer ),
 * against PostgreSQL through libpq.
 *
 * The process model is prefork : main() forks WORKERS processes and then only waits for them. Each worker
 * is the $12 challenge server almost unchanged - AmmarServer in API mode on one io_uring ring, one thread
 * owning every connection it accepts - except that the data is one blocking libpq connection to Postgres
 * instead of an in-process SQLite, and that requests are answered in batches ( HandleBatch ) : whatever
 * became ready in one pass over the ring, with all of its queries sent in one libpq pipeline and their
 * results read back in one round trip. Half of the workers listen
 * on PORT and half on EXTRA_PORT, all with SO_REUSEPORT, so the kernel spreads the incoming connections
 * evenly over the workers of each port and every worker owns its share for the connection's whole life.
 *
 * Why processes, and why blocking : a request is now a network round trip to the database box, and
 * AmmarServer's API callback is synchronous. A worker waiting on Postgres serves nobody else in that time,
 * so the parallelism has to come from there being several workers - but not too many, because what arrives
 * while a worker waits is its next batch, and a busier worker shares each round trip between more requests.
 * At 1,000,000 users ( ~42,000 req/s ) the default 32 workers see ~1,300 req/s each. Measured at that rate on
 * 4 vCPUs, 32 workers cost 54 us of CPU per request where 128 cost 74 ( and the version without batching
 * 79 ), with Postgres needing a third less CPU as well. 16 was too few : a worker's pipeline runs serially in
 * its one Postgres backend, so 16 backends could not keep up with ~55,000 req/s.
 * Everything a worker touches is its own - its ring, its connection, its prepared statements, its buffers,
 * its HMAC key schedule - so there is still not a single lock between a request and its answer.
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
#include <sys/prctl.h>
#include <sys/resource.h>
#include <sys/wait.h>

#include <libpq-fe.h>
#include <libdeflate.h>

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
/* Rule 5 : at most 512 connections to Postgres in total, and every worker holds exactly one. */
#define MAX_WORKERS 512

/* Only the feed ( 5.1 KB, 1.9 KB gzipped ) is worth compressing. A single post is ~300 bytes, which gzip's
 * 18 bytes of framing and a near-empty dictionary barely shrink, so below this it goes out as it is. */
#define GZIP_MIN_SIZE 1024
/* libdeflate's level 1 : on the seed's feed, 1,901 bytes in 15 us against level 6's 1,866 bytes in 29 us. */
#define GZIP_LEVEL 1

/* Postgres type OIDs for the parameters, and the binary epoch of timestamptz. */
#define INT8OID 20
#define TEXTOID 25
#define POSTGRES_EPOCH_IN_UNIX_DAYS 10957LL
#define SQLSTATE_FOREIGN_KEY_VIOLATION "23503"

static const char * databaseUrl = 0;
static PGconn * pg = 0;
static int statementsPrepared = 0;

static struct libdeflate_compressor * gzipCompressor = 0;

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

static int Prepare(const char * name,const char * sql,int parameters,const Oid * types)
{
  PGresult * r = PQprepare(pg,name,sql,parameters,types);
  int ok = (PQresultStatus(r)==PGRES_COMMAND_OK);
  if (!ok) { fprintf(stderr,"feedapi: could not prepare [%s] : %s",sql,PQerrorMessage(pg)); }
  PQclear(r);
  return ok;
}

/* The prepared statements, by the names they are prepared under. */
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

static int PrepareStatements(void)
{
  static const Oid oneInt8[1]      = { INT8OID };
  static const Oid twoInt8[2]      = { INT8OID , INT8OID };
  static const Oid int8AndText[2]  = { INT8OID , TEXTOID };

  /* posts_created_at_id_idx is (created_at DESC, id DESC), so this is 20 rows off the front of that index
   * rather than a sort of half a million posts. */
  if (!Prepare(statementName[STATEMENT_FEED],SELECT_POST_COLUMNS "ORDER BY p.created_at DESC, p.id DESC LIMIT 20",0,0)) { return 0; }
  if (!Prepare(statementName[STATEMENT_POST],SELECT_POST_COLUMNS "WHERE p.id = $1",1,oneInt8)) { return 0; }
  if (!Prepare(statementName[STATEMENT_INSERT_POST],"INSERT INTO posts (user_id, body) VALUES ($1, $2) RETURNING id, created_at",2,int8AndText)) { return 0; }
  if (!Prepare(statementName[STATEMENT_INSERT_LIKE],"INSERT INTO likes (user_id, post_id) VALUES ($1, $2) ON CONFLICT (user_id, post_id) DO NOTHING",2,twoInt8)) { return 0; }
  if (!Prepare(statementName[STATEMENT_PING],"SELECT 1",0,0)) { return 0; }
  return 1;
}

/* The connection is opened before the worker starts serving, so normally this is one PQstatus. If the
 * connection has been lost it is reset ( or, after DropConnection, opened again ) and the statements
 * prepared again, so that a database that comes back is served again - rather than this worker answering
 * 500 to its share of the users for good.
 * @retval 1=ready to query , 0=the database is unreachable ( PQerrorMessage says why ) */
static int DatabaseReady(void)
{
  if (pg==0) { pg = PQconnectdb(databaseUrl); statementsPrepared = 0; }
  if (pg==0) { return 0; }

  if (PQstatus(pg)!=CONNECTION_OK)
   {
     statementsPrepared = 0;
     PQreset(pg);
     if (PQstatus(pg)!=CONNECTION_OK) { return 0; }
   }

  if (!statementsPrepared)
   {
     PQsetClientEncoding(pg,"UTF8");
     statementsPrepared = PrepareStatements();
   }

  return statementsPrepared;
}

/* A pipeline that went wrong part way through leaves libpq waiting for results, and nothing more can be sent
 * on that connection until they arrive - which, on a connection that broke, they never will. So it is closed,
 * and DatabaseReady opens a new one for the next batch. */
static void DropConnection(void)
{
  PQfinish(pg);
  pg = 0;
  statementsPrepared = 0;
}

/* --- request helpers -------------------------------------------------------------------------------- */

/**
 * Verify the request's bearer token, answering with the right 401 if it does not hold up.
 * scratch has to outlive every use of claims, which point into it.
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

/* --- a batch of requests ----------------------------------------------------------------------------- */

/* AmmarServer hands this worker every request that became ready in one pass over its ring as one batch
 * ( AmmServer_SetAPIBatchHandler ) - one request when it is quiet, more the busier it gets, since what
 * arrives while a batch waits on Postgres is the next batch. Each is answered in three steps :
 *   PlanRequest   - everything that needs no database : the route, the token, the body, the id. A request
 *                   that fails any of those is answered right there ; the rest leave a statement to run.
 *   RunPlanned    - every planned statement of the batch goes to Postgres in one libpq pipeline, and all of
 *                   their results come back in one round trip, instead of one round trip each.
 *   AnswerPlanned - each result becomes its response.
 * Still one statement per request, the spec's own ( rule 3 ), each committed on its own ( rule 8 ). */
struct PlannedRequest
{
  enum Statement statement;       /* NO_STATEMENT = already answered by PlanRequest */
  int parameters;
  uint64_t parameter[2];          /* the int8 parameters, big endian, which is libpq's binary format for them */
  long long postId;               /* for the like answer */
  const char * body;              /* POST /posts : the trimmed body, inside decodedBody */
  unsigned int bodyLength;
  struct JwtClaims claims;        /* username points into payloadScratch */
  char payloadScratch[1024];
  char decodedBody[MAX_DECODED_BODY_SIZE+1];
  PGresult * result;              /* 0 if the statement could not be run at all */
};

/* One per request a batch can hold : ~33 KB each, nearly all of it the body buffer only POST /posts writes. */
static struct PlannedRequest planned[AMMSERVER_API_MAX_BATCH];

static void Plan(struct PlannedRequest * plan,enum Statement statement,int parameters,long long first,long long second)
{
  plan->statement    = statement;
  plan->parameters   = parameters;
  plan->parameter[0] = htobe64((uint64_t) first);
  plan->parameter[1] = htobe64((uint64_t) second);
}

static void PlanCreatePost(struct AmmServer_APIRequest * rqst,struct AmmServer_APIResponse * res,struct PlannedRequest * plan)
{
  if (!Authenticate(rqst,res,&plan->claims,plan->payloadScratch,sizeof(plan->payloadScratch))) { return; }

  unsigned int decodedLength=0;
  switch ( AmmServer_JSONGetStringField(rqst->body,rqst->bodyLength,"body",plan->decodedBody,sizeof(plan->decodedBody),&decodedLength) )
   {
     case AMMSERVER_JSON_MALFORMED          : Fail(res,400,"malformed JSON body"); return;
     case AMMSERVER_JSON_FIELD_NOT_A_STRING : Fail(res,400,"body is required");    return;
     case AMMSERVER_JSON_FIELD_OK           : break;
   };

  /* Longer than the buffer means longer than any 500 character body could ever be. */
  if (decodedLength>=sizeof(plan->decodedBody)) { Fail(res,400,"body must be at most 500 characters"); return; }

  const char * body = plan->decodedBody;
  unsigned int bodyLength = decodedLength;
  TrimAsciiWhitespace(&body,&bodyLength);

  if (bodyLength==0) { Fail(res,400,"body is required"); return; }
  if (Utf8CharacterCount(body,bodyLength)>500) { Fail(res,400,"body must be at most 500 characters"); return; }

  plan->body = body;
  plan->bodyLength = bodyLength;
  Plan(plan,STATEMENT_INSERT_POST,2,plan->claims.userId,0); /* the second parameter is the body, as text */
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
        if (!Authenticate(rqst,res,&plan->claims,plan->payloadScratch,sizeof(plan->payloadScratch))) { return; }

        if (!AmmServer_APIRequestMatch(rqst,"/posts/%lld/like",&postId)) { Fail(res,400,"invalid post id"); return; }

        plan->postId = postId;
        Plan(plan,STATEMENT_INSERT_LIKE,2,plan->claims.userId,postId);
        return;
      }
   }

  Fail(res,404,"not found");
}

/* Every planned statement goes out before any result is read, each followed by its own Sync. A Sync ends
 * the implicit transaction, so every statement commits ( or fails ) on its own exactly as it would outside a
 * pipeline : one like on a missing post cannot roll back another user's new post. The statements are a few
 * hundred bytes between them, far less than a socket buffer, so sending them all before reading anything
 * cannot block on Postgres's results filling up the other direction. */
static void RunPlanned(unsigned int count)
{
  static const int binary[2] = { 1 , 1 };

  unsigned int wanted=0;
  unsigned int i=0;
  for (i=0; i<count; i++) { if (planned[i].statement!=NO_STATEMENT) { ++wanted; } }
  if (wanted==0) { return; }

  /* Every result stays 0 , which AnswerPlanned answers as a database that cannot be reached. */
  if (!DatabaseReady()) { return; }
  if (!PQenterPipelineMode(pg)) { return; }

  unsigned int sent=0;
  for (i=0; i<count; i++)
   {
     struct PlannedRequest * plan = &planned[i];
     if (plan->statement==NO_STATEMENT) { continue; }

     const char * values[2]  = { (const char *) &plan->parameter[0] , (const char *) &plan->parameter[1] };
     int          lengths[2] = { sizeof(uint64_t) , sizeof(uint64_t) };
     /* The body goes in binary too, which for text is just its bytes and a length, so it never has to be
      * NUL terminated. */
     if (plan->statement==STATEMENT_INSERT_POST) { values[1] = plan->body; lengths[1] = (int) plan->bodyLength; }

     if (!PQsendQueryPrepared(pg,statementName[plan->statement],plan->parameters,values,lengths,binary,1)) { break; }
     if (!PQpipelineSync(pg)) { break; }
     ++sent;
   }

  /* Back in the same order : for each statement its result, the NULL that ends it, and the Sync's result. */
  unsigned int received=0;
  for (i=0; (i<count) && (received<sent); i++)
   {
     struct PlannedRequest * plan = &planned[i];
     if (plan->statement==NO_STATEMENT) { continue; }

     plan->result = PQgetResult(pg);
     if (plan->result==0) { break; }

     PGresult * extra=0;
     while ( (extra=PQgetResult(pg))!=0 ) { PQclear(extra); }

     PGresult * sync = PQgetResult(pg);
     int synced = (PQresultStatus(sync)==PGRES_PIPELINE_SYNC);
     PQclear(sync);
     if (!synced) { break; }

     ++received;
   }

  if ( (received<wanted) || (!PQexitPipelineMode(pg)) ) { DropConnection(); }
}

static void AnswerHealth(struct AmmServer_APIResponse * res,const PGresult * r)
{
  /* The spec wants the ok answer to mean a SELECT 1 actually came back, so it is run per request. */
  if (PQresultStatus(r)!=PGRES_TUPLES_OK)
   {
     const char * message = (r!=0) ? PQresultErrorMessage(r) : (pg!=0) ? PQerrorMessage(pg) : "the connection to the database was lost";
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
  PutPostObject(res,Int8At(r,0,0),plan->body,plan->bodyLength,createdAt,createdAtLength,plan->claims.username,plan->claims.usernameLength,1,0);
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

static void AnswerPlanned(struct AmmServer_APIResponse * res,struct PlannedRequest * plan)
{
  switch (plan->statement)
   {
     case NO_STATEMENT          : return; /* PlanRequest answered it already */
     case STATEMENT_PING        : AnswerHealth(res,plan->result);  break;
     case STATEMENT_FEED        : AnswerFeed(res,plan->result);    break;
     case STATEMENT_POST        : AnswerPost(res,plan->result);    break;
     case STATEMENT_INSERT_POST : AnswerCreatePost(res,plan);      break;
     case STATEMENT_INSERT_LIKE : AnswerLike(res,plan);            break;
   };

  PQclear(plan->result);
  plan->result = 0;
}

/* --- gzip ------------------------------------------------------------------------------------------- */

static int AcceptsGzip(const struct AmmServer_APIRequest * rqst)
{
  unsigned int valueLength=0;
  const char * value = AmmServer_APIRequestHeader(rqst,"Accept-Encoding",&valueLength);
  return ( (value!=0) && (memmem(value,valueLength,"gzip",4)!=0) );
}

/* Compressed into a buffer of this worker's and copied back over the body, which it is always smaller than
 * ( libdeflate is told so, and returns 0 rather than produce anything that is not ). */
static void GzipResponse(struct AmmServer_APIResponse * res)
{
  static char gzipped[MAX_RESPONSE_SIZE];

  size_t compressedLength = libdeflate_gzip_compress(gzipCompressor,res->body,res->bodyLength,gzipped,res->bodyLength-1);
  if (compressedLength==0) { return; }

  memcpy(res->body,gzipped,compressedLength);
  res->bodyLength   = (unsigned int) compressedLength;
  res->extraHeaders = "Content-Encoding: gzip\r\n";
}

static void HandleBatch(struct AmmServer_APIRequest * requests,struct AmmServer_APIResponse * responses,unsigned int count)
{
  unsigned int i=0;
  for (i=0; i<count; i++) { PlanRequest(&requests[i],&responses[i],&planned[i]); }

  RunPlanned(count);

  for (i=0; i<count; i++)
   {
     AnswerPlanned(&responses[i],&planned[i]);
     if ( (responses[i].bodyLength>=GZIP_MIN_SIZE) && (gzipCompressor!=0) && AcceptsGzip(&requests[i]) ) { GzipResponse(&responses[i]); }
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

static int RunWorker(const char * host,unsigned int port)
{
  /* The libpq connection, the compressor and the ring are all made here, after the fork, so that nothing a
   * worker uses was ever shared with another process. */
  if (!DatabaseReady())
   {
     /* Not fatal : /health then answers 503 with this message, and DatabaseReady keeps trying. */
     fprintf(stderr,"feedapi: worker %d could not connect to the database : %s",getpid(),(pg!=0)?PQerrorMessage(pg):"out of memory\n");
   }

  gzipCompressor = libdeflate_alloc_compressor(GZIP_LEVEL);
  if (gzipCompressor==0) { fprintf(stderr,"feedapi: worker %d could not allocate a gzip compressor , answering uncompressed\n",getpid()); }

  /* An empty config path , not 0 , so that AmmarServer's automatic lookup cannot pick up a stray
   * ammarServer.conf or /etc/ammarServer.conf and quietly change the port or the thread settings underneath
   * the benchmark. Everything this process needs comes from the environment.
   * One ring : a worker holds ~1/16th of a port's connections ( ~31,000 at a million users ), which one
   * ring's table ( 256k slots ) is plenty for. ReusingPort : the listening socket gets SO_REUSEPORT before it is bound, so
   * every worker on this port binds its own socket and the kernel spreads new connections over them. */
  struct AmmServer_Instance * instance =
      AmmServer_StartAPIOnRingsReusingPort("feedapi",host,port,"/dev/null",(void*) &HandleRequest,MAX_RESPONSE_SIZE,1);
  if (instance==0) { fprintf(stderr,"feedapi: worker %d could not start AmmarServer on %s:%u\n",getpid(),host,port); return 1; }

  /* From here on the ring answers whatever is ready in one pass as one batch ( see HandleBatch ). The few
   * requests that could have arrived before this line get HandleRequest, which is a batch of one. */
  AmmServer_SetAPIBatchHandler(instance,(void*) &HandleBatch);

  /* Every request is served on AmmarServer's ring thread ; this one only has to stay out of its way. */
  while (AmmServer_Running(instance)) { sleep(1); }

  AmmServer_Stop(instance);
  libdeflate_free_compressor(gzipCompressor);
  PQfinish(pg);
  return 0;
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

  unsigned int workers = (workersText!=0) ? (unsigned int) strtoul(workersText,0,10) : DEFAULT_WORKERS;
  if (workers<portCount)   { workers=portCount; }
  if (workers>MAX_WORKERS) { workers=MAX_WORKERS; }

  RaiseDescriptorLimit();

  fprintf(stderr,"feedapi: AmmarServer %s , libpq %d , libdeflate %s , %u workers on %s port %u",
          AmmServer_Version(),PQlibVersion(),LIBDEFLATE_VERSION_STRING,workers,host,ports[0]);
  if (portCount>1) { fprintf(stderr," and %u",ports[1]); }
  fprintf(stderr,"\n");

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
        _exit( RunWorker(host,ports[i%portCount]) );
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
