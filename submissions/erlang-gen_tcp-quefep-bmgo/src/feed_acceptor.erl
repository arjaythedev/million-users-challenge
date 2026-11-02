-module(feed_acceptor).

-export([loop/1]).

loop(ListenSocket) ->
    case gen_tcp:accept(ListenSocket) of
        {ok, Socket} ->
            Pid = spawn(feed_conn, start, []),
            case gen_tcp:controlling_process(Socket, Pid) of
                ok ->
                    Pid ! {go, Socket};
                {error, _Reason} ->
                    gen_tcp:close(Socket),
                    exit(Pid, kill)
            end,
            loop(ListenSocket);
        {error, closed} ->
            ok;
        {error, emfile} ->
            timer:sleep(100),
            loop(ListenSocket);
        {error, enfile} ->
            timer:sleep(100),
            loop(ListenSocket);
        {error, system_limit} ->
            timer:sleep(100),
            loop(ListenSocket);
        {error, _Reason} ->
            timer:sleep(10),
            loop(ListenSocket)
    end.
