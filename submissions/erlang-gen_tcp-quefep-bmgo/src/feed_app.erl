-module(feed_app).
-behaviour(application).

-export([start/2, stop/1]).

start(_StartType, _StartArgs) ->
    Config = feed_config:load(),
    persistent_term:put({feed, jwt_secret}, maps:get(secret, Config)),
    persistent_term:put({feed, start_time}, erlang:monotonic_time(second)),
    persistent_term:put({feed, json_escape_pattern}, binary:compile_pattern(
        [<<"\"">>, <<"\\">> | [<<C>> || C <- lists:seq(0, 31)]]
    )),
    Pad2 = list_to_tuple([
        iolist_to_binary(io_lib:format("~2..0B", [N])) || N <- lists:seq(0, 59)
    ]),
    Pad3 = list_to_tuple([
        iolist_to_binary(io_lib:format("~3..0B", [N])) || N <- lists:seq(0, 999)
    ]),
    StartJulian = epgsql_idatetime:date2j({2024, 1, 1}),
    EndJulian = epgsql_idatetime:date2j({2031, 12, 31}),
    Dates = maps:from_list([
        {J, feed_pg_codec:date_binary(J)} || J <- lists:seq(StartJulian, EndJulian)
    ]),
    persistent_term:put({feed, timestamp_caches}, {Pad2, Pad3, Dates}),
    persistent_term:put({feed, error_count_ref}, atomics:new(1, [{signed, false}])),
    feed_sup:start_link(Config).

stop(_State) ->
    ok.
