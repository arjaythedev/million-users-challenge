-module(feed_sup).
-behaviour(supervisor).

-export([start_link/1, init/1]).

start_link(Config) ->
    supervisor:start_link({local, ?MODULE}, ?MODULE, Config).

init(Config) ->
    Pool = #{
        id => feed_pool,
        start => {feed_pool, start_link, [Config]},
        restart => permanent,
        shutdown => 5000,
        type => worker,
        modules => [feed_pool]
    },
    Server = #{
        id => feed_server,
        start => {feed_server, start_link, [Config]},
        restart => permanent,
        shutdown => 5000,
        type => worker,
        modules => [feed_server]
    },
    {ok, {{one_for_one, 5, 10}, [Pool, Server]}}.
