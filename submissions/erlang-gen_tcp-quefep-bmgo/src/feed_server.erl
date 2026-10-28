-module(feed_server).
-behaviour(gen_server).

-export([start_link/1]).
-export([init/1, handle_call/3, handle_cast/2, handle_info/2, terminate/2, code_change/3]).

-record(state, {
    listeners = [],
    acceptors = #{}
}).

start_link(Config) ->
    gen_server:start_link({local, ?MODULE}, ?MODULE, Config, []).

init(Config) ->
    process_flag(trap_exit, true),
    Ip = parse_ip(maps:get(host, Config)),
    Ports = case maps:get(extra_port, Config, undefined) of
        undefined -> [maps:get(port, Config)];
        ExtraPort -> [maps:get(port, Config), ExtraPort]
    end,
    Listeners = [open_listener(Ip, Port) || Port <- Ports],
    Acceptors = lists:foldl(
        fun({ListenSocket, _Port}, Acc0) ->
            Pids = [start_acceptor(ListenSocket) || _ <- lists:seq(1, 4)],
            lists:foldl(
                fun(Pid, Acc1) -> maps:put(Pid, ListenSocket, Acc1) end,
                Acc0,
                Pids
            )
        end,
        #{},
        Listeners
    ),
    {ok, #state{listeners = Listeners, acceptors = Acceptors}}.

handle_call(_Request, _From, State) ->
    {reply, ok, State}.

handle_cast(_Request, State) ->
    {noreply, State}.

handle_info({'EXIT', Pid, Reason}, State) ->
    case maps:take(Pid, State#state.acceptors) of
        {ListenSocket, Acceptors1} ->
            case Reason of
                normal ->
                    {noreply, State#state{acceptors = Acceptors1}};
                _ ->
                    NewPid = start_acceptor(ListenSocket),
                    {noreply, State#state{
                        acceptors = maps:put(NewPid, ListenSocket, Acceptors1)
                    }}
            end;
        error ->
            {noreply, State}
    end;
handle_info(_Info, State) ->
    {noreply, State}.

terminate(_Reason, State) ->
    [catch gen_tcp:close(ListenSocket) || {ListenSocket, _Port} <- State#state.listeners],
    ok.

code_change(_OldVsn, State, _Extra) ->
    {ok, State}.

open_listener(Ip, Port) ->
    Options = [
        binary,
        {packet, raw},
        {active, false},
        {reuseaddr, true},
        {backlog, 65535},
        {nodelay, true},
        {send_timeout, 30000},
        {send_timeout_close, true},
        {ip, Ip},
        {exit_on_close, false}
    ],
    case gen_tcp:listen(Port, Options) of
        {ok, ListenSocket} ->
            {ListenSocket, Port};
        {error, Reason} ->
            erlang:error({listen_failed, Port, Reason})
    end.

start_acceptor(ListenSocket) ->
    spawn_link(fun() -> feed_acceptor:loop(ListenSocket) end).

parse_ip(Host) ->
    case inet:parse_address(Host) of
        {ok, Ip} ->
            Ip;
        {error, _} ->
            case inet:getaddr(Host, inet) of
                {ok, Ip} -> Ip;
                {error, _} -> {0, 0, 0, 0}
            end
    end.
