# This fork: the back end of Fillscope

This is a fork of Packt's repository for Sourav Ghosh's *Building Low Latency Applications with
C++* (Packt, 2023). Its `fillscope` branch is the back end of
[Fillscope](https://github.com/dougdew64/fillscope), an observatory that shows what this trading
system does and explains it from its exact state. `main` follows upstream unchanged.

**What the branch changes** in the code, only in `Chapter12/`, the complete system; the other
chapters are as upstream has them. (At the top level it adds this section and a `CLAUDE.md`.)
- **The build:** `Chapter12/CMakeLists.txt` requires CMake 3.10 rather than 3.0, which CMake 4 no
  longer accepts (and below 3.10, deprecates), and no longer sets `CMAKE_CXX_COMPILER` after
  `project()`, which CMake doesn't support and which broke any project added after it (GoogleTest);
  the compiler is chosen as usual (`CXX`). The book's code builds with GCC 16.2.1 under its own
  `-Werror`, in both configurations of `scripts/build.sh`.
- **Tests,** in `Chapter12/tests/`, built with `-DLOWLATENCY_TESTS=ON` (off by default, so the
  book's own build is unchanged) and run by `ctest` and by this fork's CI, normally and under
  AddressSanitizer and UndefinedBehaviorSanitizer: the market data's layout on the wire; the
  matching (price-time priority, partial fills, sweeps across price levels, cancels and rejected
  cancels); the incremental feed's sequence numbers; a snapshot's content. To call the snapshot
  synthesizer from them, `addToSnapshot()` and `publishSnapshot()` now declare their return type
  (`-> void`), which they deduced, so no other file could call them.
- **An option for tests:** `exchange_main --snapshot-interval-seconds SECONDS` sets how often the
  exchange publishes a snapshot (the book's 60 s by default), so a test of recovering from a
  snapshot need not wait a minute; the snapshot synthesizer and the market data publisher take it
  as a constructor argument with that default. Without arguments the exchange runs as before.
- **Deterministic replay:** `exchange_main --record FILE` records the requests the matching engine
  processes, in the order it processes them (`exchange/matcher/request_recording.h`), and a new
  program, `exchange_replay RECORDING MARKET_DATA_FILE`, feeds them through a fresh matching engine,
  with no clients and no network, and writes the incremental market updates it publishes
  (`exchange/matcher/replay.h`). The matcher is single-threaded and reads no clock, so a recording
  replays to the very updates the live exchange published, byte for byte: Fillscope's system test
  checks it against a live run.
- **Bugs fixed,** each found by a test that failed before its fix:
  - `MEOrderBook`'s table of every client's orders was never initialized, so cancelling an order
    that was never sent read a garbage pointer (the sanitizer build crashed; elsewhere the system's
    zeroed memory hid it). It is now allocated zeroed by `calloc`, which doesn't touch its 2 GB.
  - With that table out of the book, the book's table of price levels turned out never to be
    initialized either: it had read as empty only because the 2 GB table made every book a fresh
    mapping of zeroed memory. It is now value-initialized.
  - `createSocket()` never freed the list `getaddrinfo()` returned (LeakSanitizer).
  - Prices 256 or more ticks apart in one book corrupted it. Price levels were kept in an array of
    256 slots indexed by `price % 256`, so a bid at 356 joined the queue of the level at 100, and
    a sell at 101 then rested instead of trading with it, leaving the book crossed. Levels are now
    found by their exact price, in `PriceLevelTable` (`exchange/matcher/price_level_table.h`): a
    hash table in a fixed array, so it allocates nothing, with linear probing and backward-shift
    deletion, tested against `std::map` through random inserts and removals.

**Known issues,** not fixed (yet):
- **A book holds at most 255 price levels at once.** Its pool of levels has 256 blocks, and taking
  the last one stops the program ("Memory Pool out of space"). The book's clients use a few; a
  deep book of real prices could need more.
- **Threads are never joined.** `createAndStartThread()` allocates each `std::thread` with `new`,
  and the components' `start()` functions drop the pointer. (The tests tell LeakSanitizer to
  ignore these allocations.) It also sleeps a second after starting each thread.
- **Shutdown is slow:** each `MEOrderBook`'s destructor clears its 2 GB table of client orders,
  which takes seconds per book.
- **Market data leaves the machine.** `exchange_main` names the loopback interface (`lo`) for its
  market data, but `createSocket()` uses the interface name only when it is given no address, and
  it always is, and sets no outgoing interface: so the exchange sends its multicast on the interface
  the system routes the groups to (on a machine with Wi-Fi, onto the local network; the default
  time-to-live of 1 keeps it on the subnet), and the system loops a copy back to listeners on the
  machine, which join on the default interface too (checked 2026-10-23: `ip route get 233.252.14.3`
  names the Wi-Fi interface). Fixing it means choosing the interface in the publisher and in every
  listener, the clients and Fillscope's viewer alike.
- **`createSocket()` doesn't check `connect()`:** it compares the result with 1, which `connect()`
  never returns. (Comparing with -1 would fail the non-blocking connect it makes, which reports
  "in progress" as -1.)

More changes follow, each listed here, as Fillscope's
[plan](https://github.com/dougdew64/fillscope/blob/main/docs/PLAN.md) proceeds, next deterministic
replay. A bug found in the book's code is fixed here, with a test, and listed. The code remains
under the original MIT license ([LICENSE](LICENSE)).

**Testing it** (from `Chapter12/`): `cmake -S . -B build -DLOWLATENCY_TESTS=ON && cmake --build build
&& ctest --test-dir build --output-on-failure`. The tests take about 12 seconds and 4.3 GB of memory
(the exchange's objects preallocate their pools), and use multicast, looped back to this machine.

**Running it** (from `Chapter12/`): `bash scripts/build.sh` builds both configurations. Then start
the exchange, `./cmake-build-release/exchange_main &`, wait ten seconds, and start the clients,
`bash scripts/run_clients.sh &`. To finish, stop the exchange with `pkill -2 exchange_main`; it
exited about 45 seconds later, and the clients, each by itself, about a minute after that, once
they had had no market data for 60 seconds (checked 2026-10-23).

`scripts/run_exchange_and_clients.sh`, as published, doesn't finish by itself: it waits for the
clients before it stops the exchange, while the clients wait for the market to fall silent, which
it doesn't while the exchange runs. The clients ignore SIGINT, since the scripts start them as
background jobs of a non-interactive shell.

Upstream's README follows.

---


---

## Join Our Newsletters 📬

### DataPro  
*The future of AI is unfolding. Don’t fall behind.*

<p><a href="https://landing.packtpub.com/subscribe-datapronewsletter/?link_from_packtlink=yes"><img src="https://static.packt-cdn.com/assets/images/DataPro NL QR Code.png" alt="DataPro QR" width="150"/></a></p>

Stay ahead with [**DataPro**](https://landing.packtpub.com/subscribe-datapronewsletter/?link_from_packtlink=yes), the free weekly newsletter for data scientists, AI/ML researchers, and data engineers.  
From trending tools like **PyTorch**, **scikit-learn**, **XGBoost**, and **BentoML** to hands-on insights on **database optimization** and real-world **ML workflows**, you’ll get what matters, fast.

> Stay sharp with [DataPro](https://landing.packtpub.com/subscribe-datapronewsletter/?link_from_packtlink=yes). Join **115K+ data professionals** who never miss a beat.

---

### BIPro  
*Business runs on data. Make sure yours tells the right story.*

<p><a href="https://landing.packtpub.com/subscribe-bipro-newsletter/?link_from_packtlink=yes"><img src="https://static.packt-cdn.com/assets/images/BIPro NL QR Code.png" alt="BIPro QR" width="150"/></a></p>

[**BIPro**](https://landing.packtpub.com/subscribe-bipro-newsletter/?link_from_packtlink=yes) is your free weekly newsletter for BI professionals, analysts, and data leaders.  
Get practical tips on **dashboarding**, **data visualization**, and **analytics strategy** with tools like **Power BI**, **Tableau**, **Looker**, **SQL**, and **dbt**.

> Get smarter with [BIPro](https://landing.packtpub.com/subscribe-bipro-newsletter/?link_from_packtlink=yes). Trusted by **35K+ BI professionals**, see what you’re missing.

# Building Low Latency Applications with C++

<a href="https://www.packtpub.com/product/building-low-latency-applications-with-c/9781837639359"><img src="https://content.packt.com/B19434/cover_image_small.jpg" alt="Building Low Latency Applications with C++" height="256px" align="right"></a>

This is the code repository for [Building Low Latency Applications with C++](https://www.packtpub.com/product/building-low-latency-applications-with-c/9781837639359), published by Packt.

**Develop a complete low latency trading ecosystem from scratch using modern C++**

## What is this book about?
C++ is meticulously designed with efficiency, performance, and flexibility as its core objectives. However, real-time low latency applications demand a distinct set of requirements, particularly in terms of performance latencies.

This book covers the following exciting features: 
* Gain insights into the nature of low latency applications across various industries
* Understand how to design and implement low latency applications
* Explore C++ design paradigms and features for low latency development
* Discover which C++ features are best avoided in low latency development
* Implement best practices and C++ features for low latency
* Measure performance and improve latencies in the trading system

If you feel this book is for you, get your [copy](https://www.amazon.com/dp/1837639353) today!

<a href="https://www.packtpub.com/?utm_source=github&utm_medium=banner&utm_campaign=GitHubBanner"><img src="https://raw.githubusercontent.com/PacktPublishing/GitHub/master/GitHub.png" 
alt="https://www.packtpub.com/" border="5" /></a>


## Instructions and Navigations
All of the code is organized into folders. For example, Chapter02.

The code will look like the following:
```
main:
.LFB1
   Movl $100, %edi
   Call _Z9factorialj
```

**Following is what you need for this book:**
This book is for C++ developers who want to gain expertise in low latency applications and effective design and development strategies. C++ software engineers looking to apply their knowledge to low latency trading systems such as HFT will find this book useful to understand which C++ features matter and which ones to avoid. Quantitative researchers in the trading industry eager to delve into the intricacies of low latency implementation will also benefit from this book. Familiarity with Linux and the C++ programming language is a prerequisite for this book.

With the following software and hardware list you can run all code files present in the book (Chapter 1-15).

### Software and Hardware List

| Chapter  | Software required                   | OS required                        |
| -------- | ------------------------------------| -----------------------------------|
| 1        | C++ 20               | Linux |
| 2        | GCC 11.3.0           |  Linux|
 



We also provide a PDF file that has color images of the screenshots/diagrams used in this book. [Click here to download it](https://packt.link/ulPYN).

### Related products <Other books you may enjoy>
* Software Architecture with C++ [[Packt]](https://www.packtpub.com/product/software-architecture-with-c/9781838554590) [[Amazon]](https://www.amazon.com/dp/1838554599)

* Developing High-Frequency Trading Systems [[Packt]](https://www.packtpub.com/product/developing-high-frequency-trading-systems/9781803242811) [[Amazon]](https://www.amazon.com/dp/1803242817)

## Get to Know the Author
**Sourav Ghosh**
has worked in several proprietary high-frequency algorithmic trading firms over the last decade. He has built and deployed extremely low latency, high throughput automated trading systems for trading exchanges around the world, across multiple asset classes. He specializes in statistical arbitrage market-making, and pairs trading strategies for the most liquid global futures contracts. He works as a Senior Quantitative Developer at a trading firm in Chicago. He holds a Masters in Computer Science from the University of Southern California. His areas of interest include Computer Architecture, FinTech, Probability Theory and Stochastic Processes, Statistical Learning and Inference Methods, and Natural Language Processing.
