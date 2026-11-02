-module(feed_json).

-export([post_obj/1, feed/1, error/1, health_ok/1, health_err/1, like/2]).

post_obj(Post) ->
    [<<"{\"post\":">>, post(Post), <<"}">>].

feed(Posts) ->
    Pattern = persistent_term:get({feed, json_escape_pattern}),
    [<<"{\"posts\":[">>, join([post(P, Pattern) || P <- Posts]), <<"]}">>].

post(Post) ->
    post(Post, persistent_term:get({feed, json_escape_pattern})).

post({Id, Body, CreatedAt, Author, LikeCount}, Pattern) ->
    [
        <<"{\"id\":">>, integer_to_binary(Id),
        <<",\"body\":">>, json_string(Body, Pattern),
        <<",\"created_at\":\"">>, CreatedAt,
        <<"\",\"author\":">>, json_string(Author, Pattern),
        <<",\"like_count\":">>, integer_to_binary(LikeCount),
        <<"}">>
    ].

error(Message) ->
    [<<"{\"error\":\"">>, Message, <<"\"}">>].

health_ok(UptimeSeconds) ->
    [
        <<"{\"status\":\"ok\",\"db\":\"ok\",\"uptime_s\":">>,
        integer_to_binary(UptimeSeconds),
        <<"}">>
    ].

health_err(Message) ->
    [
        <<"{\"status\":\"degraded\",\"db\":\"unreachable\",\"error\":">>,
        json_string(Message),
        <<"}">>
    ].

like(PostId, AlreadyLiked) ->
    [
        <<"{\"liked\":true,\"already_liked\":">>,
        bool(AlreadyLiked),
        <<",\"post_id\":">>, integer_to_binary(PostId), <<"}">>
    ].

bool(true) -> <<"true">>;
bool(false) -> <<"false">>.

json_string(Bin) ->
    json_string(Bin, persistent_term:get({feed, json_escape_pattern})).

json_string(Bin, Pattern) ->
    case binary:match(Bin, Pattern) of
        nomatch ->
            [<<"\"">>, Bin, <<"\"">>];
        _ ->
            [<<"\"">>, escape(Bin, []), <<"\"">>]
    end.

escape(<<>>, Acc) ->
    lists:reverse(Acc);
escape(<<$", Rest/binary>>, Acc) ->
    escape(Rest, [<<"\\\"">> | Acc]);
escape(<<$\\, Rest/binary>>, Acc) ->
    escape(Rest, [<<"\\\\">> | Acc]);
escape(<<$\b, Rest/binary>>, Acc) ->
    escape(Rest, [<<"\\b">> | Acc]);
escape(<<$\f, Rest/binary>>, Acc) ->
    escape(Rest, [<<"\\f">> | Acc]);
escape(<<$\n, Rest/binary>>, Acc) ->
    escape(Rest, [<<"\\n">> | Acc]);
escape(<<$\r, Rest/binary>>, Acc) ->
    escape(Rest, [<<"\\r">> | Acc]);
escape(<<$\t, Rest/binary>>, Acc) ->
    escape(Rest, [<<"\\t">> | Acc]);
escape(<<C, Rest/binary>>, Acc) when C < 16#20 ->
    escape(Rest, [unicode_escape(C) | Acc]);
escape(<<C, Rest/binary>>, Acc) ->
    escape(Rest, [C | Acc]).

unicode_escape(C) ->
    <<"\\u00", (hex(C div 16)), (hex(C rem 16))>>.

hex(N) when N < 10 -> $0 + N;
hex(N) -> $A + N - 10.

join([]) ->
    [];
join([One]) ->
    One;
join([One | Rest]) ->
    [One, <<",">>, join(Rest)].
