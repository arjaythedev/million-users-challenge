defmodule Feed.MixProject do
  use Mix.Project
  def project do
    [app: :feed, version: "0.1.0", elixir: "~> 1.17",
     start_permanent: Mix.env() == :prod,
     deps: deps(), releases: [feed: [include_executables_for: [:unix]]]]
  end
  def application do
    [extra_applications: [:logger, :crypto, :ssl], mod: {Feed.Application, []}]
  end
  defp deps do
    [{:bandit, "~> 1.5"}, {:plug, "~> 1.16"}, {:postgrex, "~> 0.21"}, {:jason, "~> 1.4"}]
  end
end
