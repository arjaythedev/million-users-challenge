-module(feed_conn).

-export([start/0]).

-define(REQUEST_TIMEOUT, 120000).
-define(HEADER_TIMEOUT, 30000).
-define(BODY_TIMEOUT, 5000).
-define(MAX_BODY, 16384).
-define(MAX_HEADER, 32768).
-define(GZIP_MIN_SIZE, 1024).
-define(HEADER_END, <<"\r\n\r\n">>).

start() ->
    receive
        {go, Socket} ->
            loop(Socket, <<>>)
    after 5000 ->
        ok
    end.

loop(Socket, Buffer) ->
    erlang:garbage_collect(),
    case read_request(Socket, Buffer) of
        {ok, Method, Path, Version, Headers, Body, Rest} ->
            {Status, ResponseBody} = try
                feed_logic:handle(Method, Path, Headers, Body)
            catch
                _:_ -> {500, feed_json:error(<<"internal server error">>)}
            end,
            KeepAlive = keep_alive(Version, Headers),
            Response = build_response(Status, ResponseBody, Headers, KeepAlive),
            case gen_tcp:send(Socket, Response) of
                ok ->
                    case KeepAlive of
                        true -> loop(Socket, Rest);
                        false -> gen_tcp:close(Socket)
                    end;
                {error, _Reason} ->
                    gen_tcp:close(Socket)
            end;
        {error, closed} ->
            ok;
        {error, _Reason} ->
            gen_tcp:close(Socket)
    end.

read_request(Socket, Buffer) ->
    case binary:match(Buffer, ?HEADER_END) of
        {Start, 4} when Start =< ?MAX_HEADER ->
            HeaderBlock = binary:part(Buffer, 0, Start),
            After = binary:part(Buffer, Start + 4, byte_size(Buffer) - Start - 4),
            case parse_headers(HeaderBlock) of
                {ok, Method, Target, Version, Headers} ->
                    read_body(Socket, Method, Target, Version, Headers, After);
                error ->
                    {error, bad_request}
            end;
        {_Start, 4} ->
            {error, header_too_large};
        nomatch when byte_size(Buffer) > ?MAX_HEADER ->
            {error, header_too_large};
        nomatch ->
            Timeout = case Buffer of
                <<>> -> ?REQUEST_TIMEOUT;
                _ -> ?HEADER_TIMEOUT
            end,
            case gen_tcp:recv(Socket, 0, Timeout) of
                {ok, Data} -> read_request(Socket, <<Buffer/binary, Data/binary>>);
                {error, Reason} -> {error, Reason}
            end
    end.

parse_headers(HeaderBlock) ->
    [RequestLine | HeaderLines] = binary:split(HeaderBlock, <<"\r\n">>, [global]),
    case binary:split(RequestLine, <<" ">>, [global]) of
        [Method, Target, VersionBin] ->
            case version(VersionBin) of
                {ok, Version} ->
                    {ok, Method, path(Target), Version, parse_header_lines(HeaderLines, #{})};
                error ->
                    error
            end;
        _ ->
            error
    end.

version(<<"HTTP/1.1">>) -> {ok, {1, 1}};
version(<<"HTTP/1.0">>) -> {ok, {1, 0}};
version(_) -> error.

path(<<"http://", Rest/binary>>) -> uri_path(Rest);
path(<<"https://", Rest/binary>>) -> uri_path(Rest);
path(<<"/", _/binary>> = Path) -> Path;
path(<<"*">>) -> <<"*">>;
path(_) -> <<"/">>.

uri_path(Bin) ->
    case binary:match(Bin, <<"/">>) of
        {Pos, _} -> binary:part(Bin, Pos, byte_size(Bin) - Pos);
        nomatch -> <<"/">>
    end.

parse_header_lines([], Headers) ->
    Headers;
parse_header_lines([<<>> | Rest], Headers) ->
    parse_header_lines(Rest, Headers);
parse_header_lines([Line | Rest], Headers) ->
    case binary:split(Line, <<":">>) of
        [Name, Value] ->
            parse_header_lines(Rest, put_header(ascii_lower(Name), string:trim(Value), Headers));
        _ ->
            parse_header_lines(Rest, Headers)
    end.

put_header(<<"authorization">> = Name, Value, Headers) ->
    Headers#{Name => Value};
put_header(<<"connection">> = Name, Value, Headers) ->
    Headers#{Name => Value};
put_header(<<"content-length">> = Name, Value, Headers) ->
    Headers#{Name => Value};
put_header(<<"transfer-encoding">> = Name, Value, Headers) ->
    Headers#{Name => Value};
put_header(<<"accept-encoding">> = Name, Value, Headers) ->
    Headers#{Name => Value};
put_header(<<"expect">> = Name, Value, Headers) ->
    Headers#{Name => Value};
put_header(_Name, _Value, Headers) ->
    Headers.

ascii_lower(Bin) ->
    << <<(lower_byte(C))>> || <<C>> <= Bin >>.

lower_byte(C) when C >= $A, C =< $Z -> C + 32;
lower_byte(C) -> C.

read_body(Socket, Method, Target, Version, Headers, After) ->
    case body_length(Headers) of
        {ok, Length} ->
            maybe_continue(Socket, Headers),
            case read_exact_body(Socket, After, Length) of
                {ok, Body, Rest} ->
                    {ok, Method, Target, Version, Headers, Body, Rest};
                {error, Reason} ->
                    {error, Reason}
            end;
        error ->
            {error, bad_body}
    end.

body_length(Headers) ->
    case maps:get(<<"transfer-encoding">>, Headers, undefined) of
        undefined ->
            content_length(Headers);
        Value ->
            case binary:match(ascii_lower(Value), <<"chunked">>) of
                nomatch -> content_length(Headers);
                _ -> error
            end
    end.

content_length(Headers) ->
    case maps:get(<<"content-length">>, Headers, undefined) of
        undefined ->
            {ok, 0};
        Value ->
            try binary_to_integer(string:trim(Value)) of
                Length when Length >= 0, Length =< ?MAX_BODY ->
                    {ok, Length};
                _ ->
                    error
            catch
                _:_ ->
                    error
            end
    end.

read_exact_body(_Socket, Buffer, Length) when byte_size(Buffer) >= Length ->
    <<Body:Length/binary, Rest/binary>> = Buffer,
    {ok, Body, Rest};
read_exact_body(Socket, Buffer, Length) ->
    Missing = Length - byte_size(Buffer),
    case gen_tcp:recv(Socket, Missing, ?BODY_TIMEOUT) of
        {ok, Data} ->
            {ok, <<Buffer/binary, Data/binary>>, <<>>};
        {error, Reason} ->
            {error, Reason}
    end.

maybe_continue(Socket, Headers) ->
    case maps:get(<<"expect">>, Headers, undefined) of
        undefined ->
            ok;
        Value ->
            case ascii_lower(Value) of
                <<"100-continue">> ->
                    gen_tcp:send(Socket, <<"HTTP/1.1 100 Continue\r\n\r\n">>);
                _ ->
                    ok
            end
    end.

keep_alive({1, 1}, Headers) ->
    not connection_has(Headers, <<"close">>);
keep_alive({1, 0}, Headers) ->
    connection_has(Headers, <<"keep-alive">>);
keep_alive(_, _) ->
    false.

connection_has(Headers, Token) ->
    case maps:get(<<"connection">>, Headers, undefined) of
        undefined -> false;
        Value -> binary:match(ascii_lower(Value), Token) =/= nomatch
    end.

build_response(Status, Body, Headers, KeepAlive) ->
    {ResponseBody, EncodingHeaders} = maybe_gzip(Body, Headers),
    Size = iolist_size(ResponseBody),
    Connection = case KeepAlive of
        true -> <<"Connection: keep-alive\r\n">>;
        false -> <<"Connection: close\r\n">>
    end,
    [
        status_line(Status),
        <<"Content-Type: application/json\r\n">>,
        <<"Content-Length: ">>, integer_to_binary(Size), <<"\r\n">>,
        Connection,
        EncodingHeaders,
        <<"\r\n">>,
        ResponseBody
    ].

maybe_gzip(Body, Headers) ->
    case wants_gzip(Headers) andalso iolist_size(Body) > ?GZIP_MIN_SIZE of
        true ->
            {feed_gzip:compress(Body),
             <<"Content-Encoding: gzip\r\nVary: Accept-Encoding\r\n">>};
        false ->
            {Body, <<>>}
    end.

wants_gzip(Headers) ->
    case maps:get(<<"accept-encoding">>, Headers, undefined) of
        undefined -> false;
        Value -> binary:match(ascii_lower(Value), <<"gzip">>) =/= nomatch
    end.

status_line(200) -> <<"HTTP/1.1 200 OK\r\n">>;
status_line(201) -> <<"HTTP/1.1 201 Created\r\n">>;
status_line(400) -> <<"HTTP/1.1 400 Bad Request\r\n">>;
status_line(401) -> <<"HTTP/1.1 401 Unauthorized\r\n">>;
status_line(404) -> <<"HTTP/1.1 404 Not Found\r\n">>;
status_line(500) -> <<"HTTP/1.1 500 Internal Server Error\r\n">>;
status_line(503) -> <<"HTTP/1.1 503 Service Unavailable\r\n">>;
status_line(_) -> <<"HTTP/1.1 200 OK\r\n">>.
