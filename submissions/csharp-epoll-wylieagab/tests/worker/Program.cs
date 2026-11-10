using System.Text;
using MillionReactor;

int checks = 0;
foreach (var operation in Enum.GetValues<DbOperation>())
{
    var response = Worker.AdmissionFailure(operation);
    if (response.Status != 503) throw new Exception($"{operation}: overload must return HTTP 503");
    string expected = operation == DbOperation.Health
        ? "{\"status\":\"degraded\",\"db\":\"unreachable\",\"error\":\"server busy\"}"
        : "{\"error\":\"server busy\"}";
    if (!response.Body.AsSpan().SequenceEqual(Encoding.UTF8.GetBytes(expected)))
        throw new Exception($"{operation}: incorrect ordered overload response");
    checks += 2;
}
Console.WriteLine($"Admission response contract: {checks} checks passed (no native calls)");
