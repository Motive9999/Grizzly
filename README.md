<div align="center">

<img src="Grizzly.png" alt="Grizzly" width="170">

# Grizzly

A compact UCI-compliant chess engine in C with NNUE evaluation.

</div>

NN evaluation and training/datagen tooling are private.<br>
Developed with heavy LLM assistance.<br>
This software is licensed under GPL-3.0 or later.

## Usage

Grizzly implements the UCI protocol and is compatible with standard chess GUIs (such as Arena, CuteChess, or Nibbler) as well as direct command-line use.

Example:

uci
isready
position startpos
go depth 12

## Features

- NNUE evaluation with in-house networks and tooling
- Compact C implementation
- Architecture-specific builds (BMI2, popcnt, etc.)
- Multi-thread support

See the source for implementation details.

## Build
cd src && make
textOptions:
- `make release`
- `make modern`
- `make bmi2`
- `make popcnt`
- `make static`
- `make debug`
- `make profile`
- `make tune`
- `make clean`
- `make distclean`
- `make help`

The Makefile autodetects suitable architecture options. Use `NATIVE=yes` for host-specific optimization.


## License

GPL-3.0. See LICENSE file.

Derivative of Stash by Morgan Houppin. https://github.com/mhouppin/stash-bot