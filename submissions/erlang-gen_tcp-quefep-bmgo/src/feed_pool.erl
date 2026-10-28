-module(feed_pool).
-behaviour(gen_server).

%% Owns the set of PostgreSQL connections and publishes it as a tuple. Requests
%% pick a connection with a lock-free round-robin counter and call epgsql
%% directly; epgsql's own process serialises concurrent commands on one socket.
%% There is no hot-path checkout/checkin and therefore no central process in the
%% request path.

-export([start_link/1]).
-export([init/1, handle_call/3, handle_cast/2, handle_info/2, terminate/2, code_change/3]).

-record(state, {
    config,
    size,
    conns = #{},
    connecting = 0
}).

start_link(Config) ->
    gen_server:start_link({local, ?MODULE}, ?MODULE, Config, []).

init(Config) ->
    Size = maps:get(pool_size, Config, 128),
    DbConfig = maps:get(db, Config),
    persistent_term:erase({feed, db_conns}),
    persistent_term:put({feed, db_rr}, atomics:new(1, [{signed, false}])),
    Pool = self(),
    [spawn(fun() -> connector(Pool, DbConfig) end) || _ <- lists:seq(1, Size)],
    {ok, #state{config = DbConfig, size = Size, connecting = Size}}.

handle_call(_Request, _From, State) ->
    {reply, ok, State}.

handle_cast(_Request, State) ->
    {noreply, State}.

handle_info({connected, Conn, Statements}, State) ->
    store_statements(Statements),
    Mon = erlang:monitor(process, Conn),
    Conns1 = maps:put(Conn, Mon, State#state.conns),
    Connecting1 = max(0, State#state.connecting - 1),
    State1 = State#state{conns = Conns1, connecting = Connecting1},
    {noreply, maybe_publish(State1)};
handle_info({'DOWN', _Mon, process, Pid, _Reason}, State) ->
    case maps:take(Pid, State#state.conns) of
        {_OldMon, Conns1} ->
            Pool = self(),
            Config = State#state.config,
            spawn(fun() -> connector(Pool, Config) end),
            {noreply, State#state{
                conns = Conns1,
                connecting = State#state.connecting + 1
            }};
        error ->
            {noreply, State}
    end;
handle_info(_Info, State) ->
    {noreply, State}.

terminate(_Reason, State) ->
    [catch epgsql:close(Conn) || Conn <- maps:keys(State#state.conns)],
    persistent_term:erase({feed, db_conns}),
    ok.

code_change(_OldVsn, State, _Extra) ->
    {ok, State}.

maybe_publish(State) ->
    N = State#state.size,
    case State#state.connecting =:= 0 andalso map_size(State#state.conns) =:= N of
        true ->
            ConnTuple = list_to_tuple([Conn || Conn <- maps:keys(State#state.conns)]),
            persistent_term:put({feed, db_conns}, {ConnTuple, tuple_size(ConnTuple)}),
            State;
        false ->
            State
    end.

store_statements(Statements) ->
    case persistent_term:get({feed, statements}, undefined) of
        undefined -> persistent_term:put({feed, statements}, Statements);
        _ -> ok
    end.

connector(Pool, Config) ->
    try feed_db:connect(Config) of
        {ok, Conn, Statements} ->
            unlink(Conn),
            Pool ! {connected, Conn, Statements};
        _ ->
            timer:sleep(500),
            connector(Pool, Config)
    catch
        _:_ ->
            timer:sleep(500),
            connector(Pool, Config)
    end.
