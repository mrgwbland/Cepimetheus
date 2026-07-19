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

Cepimetheus is undergoing continuous development, with noticeable strength gains with each major release. The engine is officially tracked on the CCRL Blitz ratings list [here](https://computerchess.org.uk/404/cgi/compare_engines.cgi?family=Cepimetheus).
I have also made an effort to estimate the ratings of other major versions to better show per version improvement and let users know how strong a certain version is.
Estimated ratings are calculated with Ordo on game pools played at the exact CCRL blitz time control framework of 120s + 1s increment, my testing cpu is very similar single thread performance to the CCRL baseline 4770K. 

| Version | CCRL Blitz Rating | Estimated Rating | Error
| :--- | :---: | :---: | :---: |
| **Cepimetheus 11.0.0** | N/A | **2237.5** | +/-14.6
| **Cepimetheus 10.0.0** | **2173** | **2173.0** | (anchor)
| **Cepimetheus 9.0.0** | N/A | **2121.9** | +/-22.8
| **Cepimetheus 8.0.0** | N/A | **2065.5** | +/-34.2
| **Cepimetheus 7.2.0** | N/A | **1996.7** | +/-27.0
| **Cepimetheus 6.4.1** | **1914** | **1914.0** | (anchor)
| **Cepimetheus 5.1.0** | N/A | **1716.3** | +/-26.0
| **Cepimetheus 4.3.1** | N/A | **1620.6** | +/-19.7

> **Note on Accuracy:** All estimated ratings are subject to change as I play more games or add additional CCRL anchor engines to my testing to further calibrate the results.

## Features

* Alpha-Beta Negamax Search
* Quiescence Search
* PV Search
* Aspiration Windows
* Transposition Table
* Incremental Hashing
* MVV-LVA Move Ordering
* Killer Move Heuristic
* Counter Move Heuristic
* History Heuristic
* Null Move Pruning
* MultiPV Support
* Tapered Evaluation
* PEXT Magic Bitboards

## Download
To use this engine please click on the "releases" section of the github page. Starting from version 7.0.0 I am providing both linux and windows binaries at a variety of instruction set support levels, choose the one that best performs on your machine, for most modern cpus this will be AVX2 although AVX512 is superior if your CPU supports it, POPCNT is more compatible, and the basic 64 bit build is for legacy systems.
Additionally you can also of course compile it yourself from the source code.
Please note that the older the version of engine you get, not only will it be weaker, but the number of bugs will increase, versions 6.4.1 and later have no major bugs.
