# Cepimetheus

This is my C# engine Epimetheus remade in C. It is a command line chess engine utilising the UCI protocol, this means it requires an external GUI to use easily.\
My recommended GUIs are:\
-SCID: good program for analysis\
-Cute Chess: Good for playing multiple bots against each other in tournaments\
-En Croissant: A GUI that I only recently came across but it has the most modern styling and is good for playing and analysis\

I run the newest version as a bot on Lichess which you can see here: https://lichess.org/@/EpimetheusBot, you can play it yourself here.\
I will note that the lichess bot pool (especially bullet) seems very underrated, the Lichess bots (Cepimetheus included), in my opinion, play at a much higher level than a human at the same rating.\
A friend of mine could not beat Cepimetheus in bullet after several attempts, he was 2629 at the time whilst the bot was 1850.

## Rating
As of June 6th 2026 this engine is rated on the CCRL:
so far only version 6.4.1 has been tested, it is 1923 (+/- 27) Blitz.

## Features

* Alpha-Beta Negamax Search
* Transposition Table
* Null Move Pruning
* MVV-LVA Move Ordering
* Killer Move Heuristic
* Quiescence Search
* MultiPV Support
* Tapered Evaluation
* PEXT Magic Bitboards

## Download
To use this engine please click on the "releases" section of the github page. Starting from version 7.0.0 I am providing both linux and windows binaries at a variety of instruction set support levels, choose the one that best performs on your machine.
Additionally you can also of course compile it yourself from the source code.
