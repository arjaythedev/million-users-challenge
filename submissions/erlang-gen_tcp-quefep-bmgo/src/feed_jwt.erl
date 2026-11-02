-module(feed_jwt).

-export([verify/2]).

%% base64url of {"alg":"HS256","typ":"JWT"}: the header every seed token uses.
%% It is also exactly the only header the fast path accepts, so the alg check
%% is not skipped; unusual headers still take the full parse below.
-define(HS256_HEADER, <<"eyJhbGciOiJIUzI1NiIsInR5cCI6IkpXVCJ9">>).

verify(Token, Secret) ->
    case binary:split(Token, <<".">>, [global]) of
        [Header64, Payload64, Signature64] ->
            verify_parts(Header64, Payload64, Signature64, Secret);
        _ ->
            {error, invalid}
    end.

verify_parts(?HS256_HEADER, Payload64, Signature64, Secret) ->
    verify_hs256(?HS256_HEADER, Payload64, Signature64, Secret);
verify_parts(Header64, Payload64, Signature64, Secret) ->
    try
        HeaderJson = b64url_decode(Header64),
        Header = jsone:decode(HeaderJson, [{object_format, map}]),
        case maps:get(<<"alg">>, Header, undefined) of
            <<"HS256">> ->
                verify_hs256(Header64, Payload64, Signature64, Secret);
            _ ->
                {error, invalid}
        end
    catch
        _:_ ->
            {error, invalid}
    end.

verify_hs256(Header64, Payload64, Signature64, Secret) ->
    try
        Signature = b64url_decode(Signature64),
        Expected = crypto:mac(hmac, sha256, Secret,
            <<Header64/binary, ".", Payload64/binary>>),
        case secure_compare(Expected, Signature) of
            true ->
                PayloadJson = b64url_decode(Payload64),
                Payload = jsone:decode(PayloadJson, [{object_format, map}]),
                check_payload(Payload);
            false ->
                {error, invalid}
        end
    catch
        _:_ ->
            {error, invalid}
    end.

check_payload(Payload) when is_map(Payload) ->
    case {maps:get(<<"sub">>, Payload, undefined),
          maps:get(<<"username">>, Payload, undefined)} of
        {Sub, Username} when is_binary(Sub), is_binary(Username) ->
            case parse_positive(Sub) of
                {ok, UserId} ->
                    case maps:get(<<"exp">>, Payload, undefined) of
                        Exp when is_integer(Exp) ->
                            case Exp > erlang:system_time(second) of
                                true -> {ok, #{id => UserId, username => Username}};
                                false -> {error, invalid}
                            end;
                        _ ->
                            {error, invalid}
                    end;
                error ->
                    {error, payload}
            end;
        _ ->
            {error, payload}
    end;
check_payload(_) ->
    {error, payload}.

parse_positive(Value) ->
    try binary_to_integer(string:trim(Value)) of
        N when N > 0 -> {ok, N};
        _ -> error
    catch
        _:_ -> error
    end.

b64url_decode(Value) ->
    Standard = binary:replace(
        binary:replace(Value, <<"-">>, <<"+">>, [global]),
        <<"_">>, <<"/">>, [global]
    ),
    Padding = case byte_size(Standard) rem 4 of
        0 -> <<>>;
        2 -> <<"==">>;
        3 -> <<"=">>;
        _ -> erlang:error(bad_base64url)
    end,
    base64:decode(<<Standard/binary, Padding/binary>>).

secure_compare(A, B) when byte_size(A) =:= byte_size(B) ->
    secure_compare(A, B, 0) =:= 0;
secure_compare(_, _) ->
    false.

secure_compare(<<X, RestA/binary>>, <<Y, RestB/binary>>, Acc) ->
    secure_compare(RestA, RestB, Acc bor (X bxor Y));
secure_compare(<<>>, <<>>, Acc) ->
    Acc.
