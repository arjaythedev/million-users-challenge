using System.Diagnostics;
using System.Globalization;
using System.IO.Compression;
using System.Net;
using System.Text;
using Microsoft.AspNetCore.ResponseCompression;
using Microsoft.AspNetCore.Server.Kestrel.Core;
using MillionUsers;

static string Env(string name, string? fallback = null) =>
    Environment.GetEnvironmentVariable(name) is { Length: > 0 } value ? value :
    fallback ?? throw new InvalidOperationException($"Missing {name}");

var cores = Environment.ProcessorCount;
var builder = WebApplication.CreateSlimBuilder(new WebApplicationOptions { Args = [] });
builder.Logging.ClearProviders(); // No per-request formatting, scopes, or console contention.
builder.WebHost.UseSockets(options =>
{
    options.Backlog = 65535;
    options.IOQueueCount = Math.Min(cores, 16);
    options.WaitForDataBeforeAllocatingBuffer = true;
});
builder.WebHost.ConfigureKestrel(options =>
{
    options.AddServerHeader = false;
    options.Limits.KeepAliveTimeout = TimeSpan.FromSeconds(130);
    options.Limits.MaxConcurrentConnections = null;
    options.Limits.MaxRequestBodySize = 16 * 1024;
    options.Limits.MaxRequestBufferSize = 16 * 1024;
    options.Limits.MaxResponseBufferSize = 32 * 1024;
    options.Limits.MaxRequestHeadersTotalSize = 16 * 1024;
    var host = Env("HOST", "127.0.0.1");
    void Bind(string port)
    {
        var number = int.Parse(port, CultureInfo.InvariantCulture);
        void Http1(ListenOptions listen) => listen.Protocols = HttpProtocols.Http1;
        if (host is "0.0.0.0" or "*") options.ListenAnyIP(number, Http1);
        else if (host == "localhost") options.ListenLocalhost(number, Http1);
        else options.Listen(IPAddress.Parse(host), number, Http1);
    }
    Bind(Env("PORT", "3000"));
    Bind(Env("EXTRA_PORT", "3001"));
});
builder.Services.AddResponseCompression(options =>
{
    options.Providers.Add<GzipCompressionProvider>();
    options.MimeTypes = ["application/json"];
});
builder.Services.Configure<GzipCompressionProviderOptions>(options => options.Level = CompressionLevel.Fastest);

await using var db = Database.Create(Env("DATABASE_URL"), cores);
var api = new Api(db, Encoding.UTF8.GetBytes(Env("JWT_SECRET")), Stopwatch.GetTimestamp(), cores);
var app = builder.Build();
app.UseWhen(Compression.AcceptsGzip, branch => branch.UseResponseCompression());
app.Run(api.Handle);
Console.WriteLine($"cores={cores} db_pool={Database.PoolSize(cores)} http1_ports={Env("PORT", "3000")},{Env("EXTRA_PORT", "3001")} server_gc={System.Runtime.GCSettings.IsServerGC}");
await app.RunAsync();
