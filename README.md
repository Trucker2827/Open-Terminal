# Open Terminal

Open Terminal is a free, open-source market terminal for personal research,
watchlists, portfolios, charts, news, trading workflows, and analytics.

Current version: `v0.1.0`

## Goals

- Give users full control over their market terminal.
- Keep data sources visible, replaceable, and user-configurable.
- Support local-first workflows with optional external connectors.
- Keep the codebase inspectable, modifiable, and easy to personalize.

## Features

- Multi-asset watchlists and market views
- Portfolio and paper-trading workflows
- Charts, news, economics, and research screens
- Data connector framework
- Local AI and automation surfaces
- Native Qt/C++ desktop app with embedded Python analytics

## Build

```bash
git clone https://github.com/your-org/open-terminal.git
cd open-terminal/openmarketterminal-qt
cmake --preset macos-release
cmake --build --preset macos-release
```

## License

Open Terminal is free and open source under the MIT License. See [LICENSE](LICENSE).

## Status

This is an early personal/open-source build cloned from the OpenMarketTerminal
app codebase and rebranded for the Open Terminal project. Expect rough edges
while the project is being cleaned up and personalized.
