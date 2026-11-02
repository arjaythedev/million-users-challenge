-module(feed_pg_codec).
-behaviour(epgsql_codec).

-export([init/2, names/0, encode/3, decode/3, date_binary/1]).

-define(POSTGRES_EPOCH_JDATE, 2451545).
-define(USECS_PER_DAY, 86400000000).
-define(USECS_PER_HOUR, 3600000000).
-define(USECS_PER_MINUTE, 60000000).
-define(USECS_PER_SECOND, 1000000).

init(_Opts, _Sock) ->
    persistent_term:get({feed, timestamp_caches}).

names() ->
    [timestamptz].

encode(Data, timestamptz, _State) ->
    epgsql_idatetime:encode(timestamptz, Data).

decode(<<N:64/signed>>, timestamptz, {Pad2, Pad3, Dates}) ->
    format_timestamptz(N, Pad2, Pad3, Dates).

format_timestamptz(N, Pad2, Pad3, Dates) ->
    {Days, Usecs} = split_days(N),
    JulianDay = Days + ?POSTGRES_EPOCH_JDATE,
    Date = case maps:get(JulianDay, Dates, undefined) of
        undefined -> date_binary(JulianDay);
        Cached -> Cached
    end,
    Hour = Usecs div ?USECS_PER_HOUR,
    R1 = Usecs rem ?USECS_PER_HOUR,
    Minute = R1 div ?USECS_PER_MINUTE,
    R2 = R1 rem ?USECS_PER_MINUTE,
    Second = R2 div ?USECS_PER_SECOND,
    Millis = (R2 rem ?USECS_PER_SECOND) div 1000,
    <<Date/binary, "T",
      (element(Hour + 1, Pad2))/binary, ":",
      (element(Minute + 1, Pad2))/binary, ":",
      (element(Second + 1, Pad2))/binary, ".",
      (element(Millis + 1, Pad3))/binary, "Z">>.

split_days(N) when N >= 0 ->
    {N div ?USECS_PER_DAY, N rem ?USECS_PER_DAY};
split_days(N) ->
    Days = (N - ?USECS_PER_DAY + 1) div ?USECS_PER_DAY,
    {Days, N - Days * ?USECS_PER_DAY}.

date_binary(JulianDay) ->
    {Year, Month, Day} = epgsql_idatetime:j2date(JulianDay),
    iolist_to_binary(io_lib:format("~4..0B-~2..0B-~2..0B", [Year, Month, Day])).
