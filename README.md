# Cepimetheus

This is my C# engine Epimetheus remade in C. It is a command line chess engine utilising the UCI protocol, this means it requires an external GUI to use easily.\
My recommended GUIs are:\
-SCID: good program for analysis\
-Cute Chess: Good for playing multiple bots against each other in tournaments\
-En Croissant: A GUI that I only recently came across but it has the most modern styling and is good for playing and analysis

I run the newest version as a bot on Lichess which you can see here: https://lichess.org/@/EpimetheusBot, you can play it yourself here.\
I will note that the lichess bot pool (especially bullet) seems very underrated, the Lichess bots (Cepimetheus included), in my opinion, play at a much higher level than a human at the same rating.\
A friend of mine could not beat Cepimetheus in bullet after several attempts, he was 2629 at the time whilst the bot was 1850 (both ratings lichess bullet).

## Rating

Cepimetheus is undergoing continuous development, with noticeable strength gains with each major release. The engine is officially tracked on the CCRL Blitz ratings list [here](https://computerchess.org.uk/404/cgi/compare_engines.cgi?family=Cepimetheus).\
I have also made an effort to estimate the ratings of other major versions to better show per version improvement and let users know how strong a certain version is.\
Estimated ratings are calculated with Ordo on game pools played at the exact CCRL blitz time control framework of 120s + 1s increment, my testing cpu has very similar single thread performance to the CCRL baseline 4770K.\
My testing games include several third party "anchor engines", these are engines that are rated on the CCRL list, to improve the accuracy of the data.

| Version | CCRL Blitz Rating | Estimated Rating | Error
| :--- | :---: | :---: | :---: |
| **Cepimetheus 15.0.0** | N/A | **2659.5** | +/-20.0
| **Cepimetheus 14.0.0** | N/A | **2573.1** | +/-16.2
| **Cepimetheus 13.0.0** | N/A | **2490.7** | +/-20.4
| **Cepimetheus 12.0.0** | N/A | **2336.4** | +/-22.3
| **Cepimetheus 11.0.0** | N/A | **2237.9** | +/-6.1
| **Cepimetheus 10.0.0** | **2173** | **2173.0** | (anchor)
| **Cepimetheus 9.0.0** | N/A | **2124.2** | +/-13.8
| **Cepimetheus 8.0.0** | N/A | **2065.6** | +/-17.6
| **Cepimetheus 7.2.0** | N/A | **2003.9** | +/-17.9
| **Cepimetheus 6.4.1** | **1914** | **1914.0** | (anchor)
| **Cepimetheus 5.1.0** | N/A | **1704.7** | +/-28.1
| **Cepimetheus 4.3.1** | N/A | **1626.2** | +/-26.7

> **Note on Accuracy:** All estimated ratings are subject to change as I play more games or add additional CCRL anchor engines to my testing to further calibrate the results.

## Features

### Search
* Alpha-Beta Negamax Search
* Quiescence Search
* PV Search
* Aspiration Windows

### Transposition Table
* 4 Value Hash Buckets
* Incremental Hashing

### Move Ordering
* MVV-LVA
* SEE
* Killer Move Heuristic
* Counter Move Heuristic
* History Heuristic

### Selectivity
* Check Extensions
* Late Move Reductions
* Null Move Pruning
* Delta Pruning (QSearch)
* SEE Pruning (QSearch)
* Futility Pruning
* Reverse Futility Pruning
* Late Move Pruning

### Evaluation
* No PST Hand Crafted Evaluation
* Tapered Evaluation
* Custom Automated Tuning
* Specific Endgame Knowledge

### Board Representation
* PEXT/Fancy Magic Bitboards

### Features
* MultiPV Support
* Allocatable Hash Table Size (in megabytes)
* Move Overhead Adjustment

## Download
To use this engine please click on the "releases" section of the github page. For newer versions I have provided a range of binaries for use on Windows or Linux. \
Depending on your cpu, different binaries will perform differently, or will not be supported at all, please refer to the table below which recommends which version to use. \
Alternatively you can also of course compile it yourself from the source code.
### CPU Binary Selection Guide

| Binary Name | Intel | AMD |
| :--- | :--- | :--- |
| **AVX512** | **10th/11th Gen Core** <br>*(Not supported on newer consumer chips)* | **Zen 4 & Zen 5** |
| **BMI2** | **4th Gen Core to 14th Gen Core** & **Core Ultra** | **Zen 3** |
| **AVX2** | **N/A** <br> *(Intel CPUs that support AVX2 also support hardware PEXT)* | **Zen, Zen+, & Zen 2** <br>*(These CPUs are unique in that they support BMI2 but will underperform due to microcoded PEXT)* |
| **POPCNT** | **1st Gen to 3rd Gen Core** | **Phenom II, FX Series** |
| **64** | **Legacy 64-bit CPUs** *(Core 2 Duo, Pentium D)* | **Legacy 64-bit CPUs** *(Athlon 64, Opteron)* |

To avoid confusion, before the release of v15.0.0, I didn't release a binary called BMI2, however the old AVX2 binaries are equivalent to the new BMI2 binaries in that they demand hardware PEXT, for this reason, for older releases, POPCNT binaries are potentially superior on older Zen architectures, although this is still ineffficient as they lack the AVX2 support (and only run on software PEXT rather than magic bitboards), hence my introduction of the new binary beginning with version 15.0.0.

Please note that the older the version of engine you get, not only will it be weaker, but the potential for bugs will increase, versions 6.4.1 and later have no major bugs.
