<div align="center">

<img src="Grizzly.png" alt="Grizzly" width="200">

# Grizzly

A UCI chess engine in C with an NNUE evaluation.

</div>

Derivative of Stash (https://github.com/mhouppin/stash-bot) by Morgan Houppin.
Engine core is Stash; the NNUE eval and training/datagen tooling are mine.
GPL-3.0-or-later, same as Stash.

## Build

cd src && make

Options:

make release\
make modern\
make bmi2\
make popcnt\
make static\
make debug\
make profile\
make tune\
make clean\
make distclean\
make help
