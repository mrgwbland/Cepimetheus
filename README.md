# Cepimetheus

This is my C# engine Epimetheus remade in C. It is a command line chess engine utilising the UCI protocol, this means it requires an external GUI to use easily.
My recommended GUIs are:
-SCID: good program for analysis
-Cute Chess: Good for playing multiple bots against each other in tournaments
-En Croissant: A GUI that I only recently came across but it has the most modern styling and is good for playing and analysis

## Build

Use the provided `Makefile` with a POSIX-style C toolchain:

```sh
make
```

## Run

Start the engine binary and speak UCI over stdin/stdout:

```sh
./Cepimetheus
```
