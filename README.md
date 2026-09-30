# Aegisub - nyaa's edition

[New features – English](docs/new-features/en/README.md) | [Új funkciók – Magyar](docs/new-features/hu/README.md)

Other information is on the main page.

Building the integrated scuisei scene detector requires Rust/Cargo 1.93 or
newer, in addition to the existing Meson/C++ dependencies. Meson builds the
pinned Rust library automatically; no separate scuisei executable is needed
at runtime. See [the integration notes](docs/scuisei-integration-analysis.md)
for the processing path, platform support and validation details.
