<div align="center">

<img src="Grizzly.png" alt="Grizzly" width="175">

# Grizzly

A UCI compliant chess engine in C with NNUE evaluation.

</div>

Derivative of Stash by Morgan Houppin. https://github.com/mhouppin/stash-bot
NN evaluation and training/datagen tooling are in-house.
Developed with heavy LLM assistance.
Licensed under GPL-3.0.

## Build

```
cd src && make
```

Options:

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
