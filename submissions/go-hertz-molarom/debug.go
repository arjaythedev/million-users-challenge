package main

import (
	"expvar"
	"net/http"
	"net/http/pprof"
	"runtime/metrics"

	"github.com/arl/statsviz"
)

// runtimeMetrics are published at /debug/vars under "runtime" for capacity runs: the live heap
// (marked by the last GC, so no garbage) and stack bytes are what scale per connection.
var runtimeMetrics = []string{
	"/gc/heap/live:bytes",
	"/gc/heap/goal:bytes",
	"/memory/classes/heap/stacks:bytes",
	"/memory/classes/total:bytes",
	"/memory/classes/heap/released:bytes",
	"/sched/goroutines:goroutines",
	"/gc/cycles/total:gc-cycles",
}

func init() {
	expvar.Publish("runtime", expvar.Func(func() any {
		samples := make([]metrics.Sample, len(runtimeMetrics))
		for i, name := range runtimeMetrics {
			samples[i].Name = name
		}
		metrics.Read(samples)
		out := make(map[string]uint64, len(samples))
		for _, s := range samples {
			if s.Value.Kind() == metrics.KindUint64 {
				out[s.Name] = s.Value.Uint64()
			}
		}
		return out
	}))
}

func DebugMux() *http.ServeMux {
	mux := http.NewServeMux()

	mux.HandleFunc("/debug/pprof/", pprof.Index)
	mux.HandleFunc("/debug/pprof/cmdline", pprof.Cmdline)
	mux.HandleFunc("/debug/pprof/profile", pprof.Profile)
	mux.HandleFunc("/debug/pprof/symbol", pprof.Symbol)
	mux.HandleFunc("/debug/pprof/trace", pprof.Trace)
	mux.Handle("/debug/vars", expvar.Handler())

	_ = statsviz.Register(mux)

	return mux
}
