defmodule Feed.MixProject do
  use Mix.Project

  def project do
    [
      app: :feed,
      version: "1.0.0",
      elixir: "~> 1.20",
      start_permanent: Mix.env() == :prod,
      deps: deps(),
      releases: [feed: [include_executables_for: [:unix]]]
    ]
  end

  def application do
    [extra_applications: [:logger, :crypto], mod: {Feed.Application, []}]
  end

  # Exact versions; mix.lock pins the whole tree.
  defp deps do
    [
      {:bandit, "1.14.0"},
      {:postgrex, "0.22.4"}
    ]
  end
end
