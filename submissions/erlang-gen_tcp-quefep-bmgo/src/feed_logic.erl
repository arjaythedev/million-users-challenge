-module(feed_logic).
-include_lib("epgsql/include/epgsql.hrl").

-export([handle/4]).

handle(Method, Path, Headers, Body) ->
    case route(Method, Path) of
        health ->
            health();
        feed ->
            feed();
        {get_post, IdBin} ->
            get_post(IdBin);
        create_post ->
            create_post(Headers, Body);
        {like_post, Rest} ->
            like_post(Rest, Headers);
        not_found ->
            {404, feed_json:error(<<"not found">>)}
    end.

route(<<"GET">>, <<"/health">>) ->
    health;
route(<<"GET">>, <<"/feed">>) ->
    feed;
route(<<"GET">>, <<"/posts/", Id/binary>>) ->
    {get_post, Id};
route(<<"POST">>, <<"/posts">>) ->
    create_post;
route(<<"POST">>, <<"/posts/", Rest/binary>>) ->
    {like_post, Rest};
route(_Method, _Path) ->
    not_found.

health() ->
    case feed_db:health() of
        ok ->
            {200, feed_json:health_ok(uptime())};
        {error, _Reason} ->
            {503, feed_json:health_err(<<"database unreachable">>)}
    end.

feed() ->
    case rows(feed_db:query(feed, [])) of
        {ok, Posts} ->
            {200, feed_json:feed(Posts)};
        error ->
            {500, feed_json:error(<<"internal server error">>)}
    end.

get_post(IdBin) ->
    case binary:match(IdBin, <<"/">>) of
        nomatch ->
            case parse_id(IdBin) of
                {ok, PostId} ->
                    case one_row(feed_db:query(get_post, [PostId])) of
                        {ok, Post} ->
                            {200, feed_json:post_obj(Post)};
                        not_found ->
                            {404, feed_json:error(<<"post not found">>)};
                        {error, _Reason} ->
                            {500, feed_json:error(<<"internal server error">>)}
                    end;
                error ->
                    {400, feed_json:error(<<"invalid post id">>)}
            end;
        _ ->
            {404, feed_json:error(<<"not found">>)}
    end.

create_post(Headers, RawBody) ->
    case auth(Headers) of
        {ok, #{id := UserId, username := Username}} ->
            case decode_post_body(RawBody) of
                {ok, Body} ->
                    create_post(UserId, Username, Body);
                {error, Message} ->
                    {400, feed_json:error(Message)}
            end;
        {error, missing} ->
            {401, feed_json:error(<<"missing bearer token">>)};
        {error, invalid} ->
            {401, feed_json:error(<<"invalid or expired token">>)};
        {error, payload} ->
            {401, feed_json:error(<<"invalid token payload">>)}
    end.

create_post(UserId, Username, Body) ->
    case one_row(feed_db:query(insert_post, [UserId, Body])) of
        {ok, {NewId, CreatedAt}} ->
            Post = {NewId, Body, CreatedAt, Username, 0},
            {201, feed_json:post_obj(Post)};
        _ ->
            {500, feed_json:error(<<"internal server error">>)}
    end.

like_post(Rest, Headers) ->
    case auth(Headers) of
        {ok, #{id := UserId}} ->
            case like_id(Rest) of
                {ok, PostId} ->
                    do_like_post(UserId, PostId);
                not_found ->
                    {404, feed_json:error(<<"not found">>)};
                error ->
                    {400, feed_json:error(<<"invalid post id">>)}
            end;
        {error, missing} ->
            {401, feed_json:error(<<"missing bearer token">>)};
        {error, invalid} ->
            {401, feed_json:error(<<"invalid or expired token">>)};
        {error, payload} ->
            {401, feed_json:error(<<"invalid token payload">>)}
    end.

do_like_post(UserId, PostId) ->
    case affected(feed_db:query(insert_like, [UserId, PostId])) of
        {ok, Count} when Count > 0 ->
            {201, feed_json:like(PostId, false)};
        {ok, 0} ->
            {200, feed_json:like(PostId, true)};
        {error, #error{code = <<"23503">>}} ->
            {404, feed_json:error(<<"post not found">>)};
        {error, _Reason} ->
            {500, feed_json:error(<<"internal server error">>)};
        _ ->
            {500, feed_json:error(<<"internal server error">>)}
    end.

auth(Headers) ->
    case maps:get(<<"authorization">>, Headers, undefined) of
        <<"Bearer ", Token/binary>> ->
            feed_jwt:verify(string:trim(Token), secret());
        _ ->
            {error, missing}
    end.

decode_post_body(<<>>) ->
    {error, <<"body is required">>};
decode_post_body(RawBody) ->
    try jsone:decode(RawBody, [{object_format, map}]) of
        Map when is_map(Map) ->
            case maps:get(<<"body">>, Map, undefined) of
                Value when is_binary(Value) ->
                    Body = string:trim(Value),
                    case Body of
                        <<>> ->
                            {error, <<"body is required">>};
                        _ ->
                            case string:length(Body) > 500 of
                                true ->
                                    {error, <<"body must be at most 500 characters">>};
                                false ->
                                    {ok, Body}
                            end
                    end;
                _ ->
                    {error, <<"body is required">>}
            end;
        _ ->
            {error, <<"body is required">>}
    catch
        _:_ ->
            {error, <<"malformed JSON body">>}
    end.

like_id(Rest) ->
    Suffix = <<"/like">>,
    SuffixLength = byte_size(Suffix),
    case byte_size(Rest) >= SuffixLength andalso
         binary:part(Rest, byte_size(Rest) - SuffixLength, SuffixLength) =:= Suffix of
        true ->
            IdBin = binary:part(Rest, 0, byte_size(Rest) - SuffixLength),
            case binary:match(IdBin, <<"/">>) of
                nomatch -> parse_id(IdBin);
                _ -> not_found
            end;
        false ->
            not_found
    end.

parse_id(Bin) ->
    try binary_to_integer(Bin) of
        Id when Id > 0 -> {ok, Id};
        _ -> error
    catch
        _:_ -> error
    end.

rows({ok, _Count, _Columns, Rows}) when is_list(Rows) ->
    {ok, Rows};
rows({ok, _Columns, Rows}) when is_list(Rows) ->
    {ok, Rows};
rows({ok, _Count}) ->
    {ok, []};
rows(_) ->
    error.

one_row({ok, _Count, _Columns, [Row]}) ->
    {ok, Row};
one_row({ok, _Count, _Columns, []}) ->
    not_found;
one_row({ok, _Columns, [Row]}) ->
    {ok, Row};
one_row({ok, _Columns, []}) ->
    not_found;
one_row({ok, _Count}) ->
    not_found;
one_row({error, Reason}) ->
    {error, Reason};
one_row(_) ->
    {error, unexpected}.

affected({ok, Count, _Columns, _Rows}) when is_integer(Count) ->
    {ok, Count};
affected({ok, Count}) when is_integer(Count) ->
    {ok, Count};
affected({error, Reason}) ->
    {error, Reason};
affected(_) ->
    {error, unexpected}.

secret() ->
    persistent_term:get({feed, jwt_secret}).

uptime() ->
    case persistent_term:get({feed, start_time}, undefined) of
        undefined -> 0;
        Start -> erlang:monotonic_time(second) - Start
    end.
