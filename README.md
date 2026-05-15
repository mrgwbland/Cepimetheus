# Cepimetheus

This is my C# engine Epimetheus remade in C. It is a command line chess engine utilising the UCI protocol, this means it requires an external GUI to use easily.\
My recommended GUIs are:\
-SCID: good program for analysis\
-Cute Chess: Good for playing multiple bots against each other in tournaments\
-En Croissant: A GUI that I only recently came across but it has the most modern styling and is good for playing and analysis\

I run the newest version as a bot on Lichess which you can see here: https://lichess.org/@/EpimetheusBot, you can play it yourself here.\
I will note that the lichess bot pool (especially bullet) seems very underrated, the Lichess bots (Cepimetheus included), in my opinion, play at a much higher level than a human at the same rating.\
A friend of mine could not beat Cepimetheus in bullet after several attempts, he was 2629 at the time whilst the bot was 1850.

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
