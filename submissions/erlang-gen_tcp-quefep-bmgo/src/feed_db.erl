-module(feed_db).
-include_lib("epgsql/include/epgsql.hrl").

-export([connect/1, query/2, health/0]).

connect(Config) ->
    #{host := Host, port := Port, user := User, password := Password, database := Database} = Config,
    Opts = [
        {host, Host},
        {port, Port},
        {username, User},
        {password, Password},
        {database, Database},
        {ssl, false},
        {timeout, 5000},
        {tcp_opts, [{nodelay, true}]},
        {codecs, [{feed_pg_codec, []}]}
    ],
    case epgsql:connect(Opts) of
        {ok, Conn} ->
            case prepare(Conn) of
                {ok, Statements} ->
                    case epgsql:sync(Conn) of
                        ok ->
                            {ok, Conn, Statements};
                        Error ->
                            catch epgsql:close(Conn),
                            {error, Error}
                    end;
                {error, Reason} ->
                    catch epgsql:close(Conn),
                    {error, Reason}
            end;
        Error ->
            Error
    end.

query(Kind, Params) ->
    Tries = case is_read(Kind) of
        true -> 2;
        false -> 1
    end,
    do_query(Kind, Params, Tries).

health() ->
    do_health(2).

do_query(_Kind, _Params, 0) ->
    {error, no_connection};
do_query(Kind, Params, Tries) ->
    case {pick(), persistent_term:get({feed, statements}, undefined)} of
        {none, _} ->
            {error, no_connection};
        {_, undefined} ->
            {error, no_connection};
        {Conn, Statements} ->
            Statement = maps:get(Kind, Statements),
            try epgsql:prepared_query(Conn, Statement, Params) of
                {error, #error{}} = Error ->
                    log_error({sql, Error}),
                    Error;
                {error, Reason} = Error ->
                    log_error({connection, Reason}),
                    retry_or_error(Kind, Params, Tries, Error);
                Result ->
                    Result
            catch
                Class:Reason ->
                    log_error({crash, Class, Reason}),
                    retry_or_error(Kind, Params, Tries, {error, connection})
            end
    end.

retry_or_error(Kind, Params, Tries, Error) ->
    case is_read(Kind) andalso Tries > 1 of
        true -> do_query(Kind, Params, Tries - 1);
        false -> Error
    end.

do_health(0) ->
    {error, no_connection};
do_health(Tries) ->
    case pick() of
        none ->
            {error, no_connection};
        Conn ->
            try epgsql:squery(Conn, "SELECT 1") of
                {ok, _, _} -> ok;
                {ok, _} -> ok;
                {error, _} -> do_health(Tries - 1)
            catch
                _:_ -> do_health(Tries - 1)
            end
    end.

pick() ->
    case persistent_term:get({feed, db_conns}, undefined) of
        undefined ->
            none;
        {Conns, N} ->
            RR = persistent_term:get({feed, db_rr}),
            element((atomics:add_get(RR, 1, 1) rem N) + 1, Conns)
    end.

is_read(feed) -> true;
is_read(get_post) -> true;
is_read(_) -> false.

log_error(Error) ->
    case persistent_term:get({feed, error_count_ref}, undefined) of
        undefined ->
            ok;
        Ref ->
            case atomics:add_get(Ref, 1, 1) =< 30 of
                true -> io:format("DB ERROR ~p~n", [Error]);
                false -> ok
            end
    end.

prepare(Conn) ->
    try
        {ok, Feed} = epgsql:parse(Conn, "feed", feed_sql(), []),
        {ok, GetPost} = epgsql:parse(Conn, "get_post", get_post_sql(), []),
        {ok, InsertPost} = epgsql:parse(Conn, "insert_post", insert_post_sql(), []),
        {ok, InsertLike} = epgsql:parse(Conn, "insert_like", insert_like_sql(), []),
        {ok, #{
            feed => Feed,
            get_post => GetPost,
            insert_post => InsertPost,
            insert_like => InsertLike
        }}
    catch
        Class:Reason ->
            {error, {Class, Reason}}
    end.

feed_sql() ->
    <<"SELECT p.id, p.body, p.created_at, u.username, "
      "(SELECT count(*) FROM likes l WHERE l.post_id = p.id)::int AS like_count "
      "FROM posts p JOIN users u ON u.id = p.user_id "
      "ORDER BY p.created_at DESC, p.id DESC LIMIT 20">>.

get_post_sql() ->
    <<"SELECT p.id, p.body, p.created_at, u.username, "
      "(SELECT count(*) FROM likes l WHERE l.post_id = p.id)::int AS like_count "
      "FROM posts p JOIN users u ON u.id = p.user_id "
      "WHERE p.id = $1">>.

insert_post_sql() ->
    <<"INSERT INTO posts (user_id, body) VALUES ($1, $2) RETURNING id, created_at">>.

insert_like_sql() ->
    <<"INSERT INTO likes (user_id, post_id) VALUES ($1, $2) "
      "ON CONFLICT (user_id, post_id) DO NOTHING">>.
