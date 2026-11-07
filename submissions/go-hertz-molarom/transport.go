package main

import (
	"context"
	"net"

	hconfig "github.com/cloudwego/hertz/pkg/common/config"
	"github.com/cloudwego/hertz/pkg/network"
	hertznetpoll "github.com/cloudwego/hertz/pkg/network/netpoll"
	"github.com/cloudwego/netpoll"
)

type connStateKey struct{}

func netpollConn(c net.Conn) (netpoll.Connection, bool) {
	hc, ok := c.(*hertznetpoll.Conn)
	if !ok {
		return nil, false
	}

	nc, ok := hc.Conn.(netpoll.Connection)
	return nc, ok
}

// parkedConn is a connection whose Close is deferred to parkingTransport.
type parkedConn struct {
	network.Conn
}

func (parkedConn) Close() error { return nil }

// parkingTransport hands idle keep-alive connections back to netpoll instead of
// keeping a goroutine parked on each one.
type parkingTransport struct {
	network.Transporter
	sweeper *idleSweeper
}

func parkingTransportWith(sw *idleSweeper) func(*hconfig.Options) network.Transporter {
	return func(o *hconfig.Options) network.Transporter {
		if sw != nil {
			next := o.OnAccept
			o.OnAccept = func(c net.Conn) context.Context {
				ctx := context.Background()
				if next != nil {
					ctx = next(c)
				}

				if nc, ok := netpollConn(c); ok {
					ctx = context.WithValue(ctx, connStateKey{}, sw.track(nc))
				}
				return ctx
			}
		}
		return parkingTransport{
			Transporter: hertznetpoll.NewTransporter(o),
			sweeper:     sw,
		}
	}
}

func (t parkingTransport) ListenAndServe(onData network.OnData) error {
	return t.Transporter.ListenAndServe(func(ctx context.Context, conn any) error {
		c, ok := conn.(network.Conn)
		if !ok {
			return onData(ctx, conn)
		}

		st, _ := ctx.Value(connStateKey{}).(*connState)
		if st != nil {
			st.begin()
		}

		err := onData(ctx, parkedConn{c})
		if err != nil {
			_ = c.Close()
		}

		if st != nil {
			st.end(t.sweeper.now())
		}
		return err
	})
}
