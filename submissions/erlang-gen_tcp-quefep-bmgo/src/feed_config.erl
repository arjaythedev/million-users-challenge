-module(feed_config).

-export([load/0]).

load() ->
    DbUrl = getenv("DATABASE_URL"),
    Secret = list_to_binary(getenv("JWT_SECRET")),
    Host = getenv_or("HOST", "0.0.0.0"),
    Port = port_env("PORT", 80),
    ExtraPort = optional_port_env("EXTRA_PORT"),
    Db = parse_db_url(DbUrl),
    #{
        db => Db,
        secret => Secret,
        host => Host,
        port => Port,
        extra_port => ExtraPort,
        pool_size => pool_size_env()
    }.

getenv(Name) ->
    case os:getenv(Name) of
        false -> erlang:error({missing_env, Name});
        "" -> erlang:error({missing_env, Name});
        Value -> Value
    end.

getenv_or(Name, Default) ->
    case os:getenv(Name) of
        false -> Default;
        "" -> Default;
        Value -> Value
    end.

port_env(Name, Default) ->
    case os:getenv(Name) of
        false -> Default;
        "" -> Default;
        Value -> parse_port(Name, Value)
    end.

optional_port_env(Name) ->
    case os:getenv(Name) of
        false -> undefined;
        "" -> undefined;
        Value -> parse_port(Name, Value)
    end.

parse_port(Name, Value) ->
    try list_to_integer(Value) of
        Port when Port > 0, Port =< 65535 -> Port;
        _ -> erlang:error({invalid_env, Name, Value})
    catch
        error:badarg -> erlang:error({invalid_env, Name, Value})
    end.

pool_size_env() ->
    Default = 128,
    case os:getenv("DB_POOL") of
        false -> Default;
        "" -> Default;
        Value ->
            try list_to_integer(Value) of
                N when N > 0, N =< 512 -> N;
                _ -> Default
            catch
                error:badarg -> Default
            end
    end.

parse_db_url(Url) ->
    case uri_string:parse(Url) of
        #{scheme := "postgres", userinfo := UserInfo, host := Host, path := Path} = Parsed ->
            {User, Pass} = split_userinfo(UserInfo),
            DbName = case Path of
                [$/ | Rest] -> uri_string:percent_decode(Rest);
                _ -> erlang:error({invalid_database_url, missing_database})
            end,
            Port = maps:get(port, Parsed, 5432),
            #{
                host => Host,
                port => Port,
                user => User,
                password => Pass,
                database => DbName
            };
        _ ->
            erlang:error({invalid_database_url, Url})
    end.

split_userinfo(UserInfo) ->
    Decoded = uri_string:percent_decode(UserInfo),
    case string:split(Decoded, ":", leading) of
        [User, Pass] -> {User, Pass};
        [User] -> {User, ""}
    end.
