# Java Spring Boot feed API

This submission implements the challenge API with Java 21, Spring Boot 3.2.3, embedded Tomcat, Spring MVC, `JdbcTemplate`, HikariCP, pgjdbc, Jackson, and Auth0 `java-jwt` 4.6.2. Maven and dependencies are version-pinned through the Maven wrapper and `pom.xml`.

## Implementation

- Implements the five endpoints and response/error formats in the repository's `SPEC.md`.
- Uses the specified PostgreSQL statements, with one query per request. Reads are not cached; inserts use PostgreSQL autocommit.
- Verifies the HS256 signature and expiration on each authenticated request. Auth resolution precedes post-ID validation on the like route.
- Does not create or modify database objects. The API uses the supplied schema and seed data.
- Uses one HikariCP pool with a maximum size configured by `DB_POOL`, defaulting to 128 connections.
- Runs requests on virtual threads. Both the main connector and optional `EXTRA_PORT` connector use a 2,000,000 connection limit, an 8,192 accept backlog, and 90-second connection/keep-alive timeouts. Both use gzip for JSON responses.
- Uses G1 with a 70% maximum and 10% initial heap sized relative to the memory available to the process.

## Build and run

`install.sh` installs OpenJDK 21 when it is not already present. `build.sh` invokes the checked-in Maven wrapper and produces `target/feed.jar`. `start.sh` runs the jar in the foreground and reads `DATABASE_URL`, `JWT_SECRET`, `HOST`, `PORT`, and optional `EXTRA_PORT` from the environment.

From the repository root, run the challenge test harness against a fresh seed database with:

```bash
bash test/run.sh submissions/java-springboot-danieloyoo
```

The harness checks `/health` on both configured ports and runs the 42 API assertions on the primary port against a fresh database copy.
