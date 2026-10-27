# This fork: the back end of Fillscope

**Purpose:** what this fork changes in the book's code, why, its known issues, and how to test and
run it. **Status:** reference; each change is listed here as it lands. **Read when:** using or
changing the fork, or before relying on the book's code as published.

This is a fork of Packt's repository for Sourav Ghosh's *Building Low Latency Applications with
C++* (Packt, 2023). Its `fillscope` branch is the back end of
[Fillscope](https://github.com/dougdew64/fillscope), an observatory that shows what this trading
system does and explains it from its exact state. `main` follows upstream unchanged.

**What the branch changes** in the code, only in `Chapter12/`, the complete system; the other
chapters are as upstream has them. (At the top level it adds this section, a `CLAUDE.md`,
`ATTRIBUTION.md` with its script in `tools/`, and its CI in `.github/`.)
- **The build:** `Chapter12/CMakeLists.txt` requires CMake 3.10 rather than 3.0, which CMake 4 no
  longer accepts (and below 3.10, deprecates), and no longer sets `CMAKE_CXX_COMPILER` after
  `project()`, which CMake doesn't support and which broke any project added after it (GoogleTest);
  the compiler is chosen as usual (`CXX`). The book's code builds with GCC 16.2.1 under its own
  `-Werror`, in both configurations of `scripts/build.sh`.
- **Tests,** in `Chapter12/tests/`, built with `-DLOWLATENCY_TESTS=ON` (off by default, so the
  book's own build is unchanged) and run by `ctest` and by this fork's CI, normally and under
  AddressSanitizer and UndefinedBehaviorSanitizer: the market data's layout on the wire; the
  matching (price-time priority, partial fills, sweeps across price levels, cancels and rejected
  cancels); the incremental feed's sequence numbers; a snapshot's content; `PriceLevelTable`;
  recording requests, the running matcher's recording, replaying them, and what each request did,
  checked against the running matcher's own updates and responses; the lockstep anchor's
  tracer check; the order server refusing requests the matcher can't process, and the trading
  client's handling of a refusal; the lock-free queue's order and its stop when full; multicast staying on the interface a
  socket is given; the options for where the programs meet; and, in a program of their own
  (`lowlatency_allocation_tests`), that the queues, the pools, the price levels and the snapshot's
  updates allocate nothing. To call the snapshot
  synthesizer from them, `addToSnapshot()` and `publishSnapshot()` now declare their return type
  (`-> void`), which they deduced, so no other file could call them.
- **An option for tests:** `exchange_main --snapshot-interval-seconds SECONDS` sets how often the
  exchange publishes a snapshot (the book's 60 s by default; from 0.001 to 1,000,000), so a test of recovering from a
  snapshot need not wait a minute; the snapshot synthesizer and the market data publisher take it
  as a constructor argument with that default. Without arguments the exchange runs as before.
  `trading_main --warm-up-seconds SECONDS` (anywhere on its command line; from 0 to 3600) sets how
  long the client waits after starting before it trades, the book's 10 s by default, so a test
  need not wait as long.
- **Options for where the programs meet:** `exchange_main` and `trading_main` both take
  `--order-port PORT`, `--snapshot GROUP:PORT` and `--incremental GROUP:PORT`
  (`common/endpoints.h`; `trading_main` takes them anywhere on its command line, and reads its
  other arguments as before), the book's addresses by default. Tests run their exchange and client
  on ports of their own, so nothing else on the machine using the book's (a replay stepped in a
  debugger, another run of the tests) reaches them. The tests' own sockets take ports the same
  way (`testPort()`, in `tests/test_support.h`).
- **Deterministic replay:** `exchange_main --record FILE` records the requests the matching engine
  processes, in the order it processes them (`exchange/matcher/request_recording.h`), and a new
  program, `exchange_replay RECORDING MARKET_DATA_FILE`, feeds them through a fresh matching engine,
  with no clients and no network, and writes the incremental market updates it publishes
  (`exchange/matcher/replay.h`). The matcher is single-threaded and reads no clock, so a recording
  replays to the very updates the live exchange published, byte for byte: Fillscope's system test
  checks it against a live run. A recording is opened only once the whole command line has been
  read; if a write to it fails, it stops there and the exchange says so on stderr, and runs on. A
  write cut off part-way would leave part of a request, which makes a recording unreadable, so the
  file is cut back to the last whole request: what was recorded still replays. Reading one
  refuses a request the matcher can't process (a ticker, client or order id past its tables, an
  unknown type or a new order's unknown side), naming it.
- **Debugger lockstep on a replay:** `exchange_replay RECORDING --publish GROUP:PORT` sends each
  request's market updates on that multicast group, as the exchange's publisher would, then calls
  a lockstep anchor, `fillscope_request_anchor(request_number, updates_published)`
  (`exchange/matcher/lockstep_anchor.h`), where a debugger stops (by a breakpoint set on it by name,
  or, with `--self-stop`, by the anchor itself when the replay is traced). A viewer listening to the
  group then shows the books after each request as the debugger stops after it; Fillscope's
  debugger tests check this under GDB and LLDB, at -O0 and -O2. `--interval-ms` paces a replay to
  watch without a debugger; the market data file is now optional when publishing.
- **What each request did:** `exchange_replay RECORDING --steps FILE` also writes, for each request,
  the market updates it made (by number) and the responses the matcher sent to clients for it
  (accepted, filled, canceled, cancel rejected), which the market data alone doesn't say; the
  format is in `exchange/matcher/replay.h`. It is what lets Fillscope step through a recording a
  request at a time and say who was told what. The running matcher's own updates and responses for
  the same requests are the same, which the tests check.
- **A planted slicer:** a fourth strategy, `trading_main CLIENT SLICER --slice TICKER BUY|SELL TOTAL
  PIECE GAP`, which sends a large order in pieces, each a set number of market updates on its ticker
  after the one before, priced to take and cancelled as soon as it is accepted, as an
  immediate-or-cancel order would be; `--gap-jitter` and `--size-spread` vary the gaps and the
  sizes, from a seeded generator, and several `--slice` options, each on a ticker of its own, are
  worked at once (`trading/strategy/slicer.h`). It is there for Fillscope to find in the market
  data. The trade engine takes the orders as a new, optional argument; the book's strategies are
  unchanged.
- **Who sent what:** `exchange_replay RECORDING --truth FILE` also writes the replay's truth: every
  order accepted, with its client, and every trade, with the clients and order ids of the incoming
  and the resting order, from the matcher's responses; the format is in `exchange/matcher/replay.h`.
  The market data never names a client; Fillscope reads the truth only to show what really happened
  and to score what it infers from the market data.
- **Attribution:** `ATTRIBUTION.md` counts each file's lines by who wrote them: upstream (this
  repository's commits before the fork), Fillscope's owner, or Claude, co-authoring. Generated by
  `tools/attribution/attribution.py`, a copy of Fillscope's, and checked by this fork's CI.
- **Bugs fixed,** each found by a test that failed before its fix:
  - `MEOrderBook`'s table of every client's orders was never initialized, so cancelling an order
    that was never sent read a garbage pointer (the sanitizer build crashed; elsewhere the system's
    zeroed memory hid it). It is now allocated zeroed by `calloc`, which doesn't touch its 2 GB.
  - With that table out of the book, the book's table of price levels turned out never to be
    initialized either: it had read as empty only because the 2 GB table made every book a fresh
    mapping of zeroed memory. It is now value-initialized.
  - `createSocket()` never freed the list `getaddrinfo()` returned (LeakSanitizer).
  - One request could end the exchange. The matcher indexes its tables by a request's ids
    unchecked: a ticker of 8 or more was undefined behaviour, and a client id of 256 or more or an
    order id of 1,048,576 or more threw inside a `noexcept` function. The trading client numbers its
    orders from 1 up, so a long enough run ended the exchange by itself. The order server now
    checks each request as it arrives (`unprocessable()`, in `exchange/order_server/client_request.h`,
    the check a recording is read with) and refuses one the matcher can't process before the
    matcher, or its recording, sees it: a new order with a new response, `REJECTED`, and a cancel
    with `CANCEL_REJECTED`, on the client's own response numbers. The trading client marks a
    rejected order dead, so its strategy can send another. A client id of 256 or more is dropped and
    logged instead, since the exchange keeps no connection or numbers to answer it with; the order
    server's own tables, indexed by it, were undefined behaviour too.
  - Prices 256 or more ticks apart in one book corrupted it. Price levels were kept in an array of
    256 slots indexed by `price % 256`, so a bid at 356 joined the queue of the level at 100, and
    a sell at 101 then rested instead of trading with it, leaving the book crossed. Levels are now
    found by their exact price, in `PriceLevelTable` (`exchange/matcher/price_level_table.h`): a
    hash table in a fixed array, so it allocates nothing, with linear probing and backward-shift
    deletion, tested against `std::map` through random inserts and removals.
  - The market data left the machine. The exchange and the trading client name the loopback
    interface (`lo`) for their multicast, but the sockets ignored the name: `createSocket()` set no
    outgoing interface, so the exchange sent on the interface the system routes the groups to (on
    a machine with Wi-Fi, every update onto the local network), and `join()` joined on the default
    interface. With no network at all, neither worked (the same calls, made in a network namespace
    with only loopback: the join failed with "No such device", the send with "Network is
    unreachable").
    Now a multicast sender sends on the interface it is given (`setMulticastInterface()`, in
    `common/socket_utils.h`), stopping the program as it starts if that interface has no IPv4
    address, and `McastSocket::join()` joins on the interface `init()` was given. Multicast over
    loopback needs no privileges: `lo` lacks the `MULTICAST` flag, and it works anyway (checked
    2026-10-24, Linux 7.2). Fillscope's viewer joins on loopback too, and its system test checks
    that a listener joined on the default interface receives nothing from the exchange.
  - `ASSERT` (`common/macros.h`) was a function taking a `std::string`, so every call built its
    message, even when the check held, and a message longer than a string's own buffer (15
    characters) allocated: on every message read from a queue, every allocation and release from a
    memory pool, and every update the snapshot synthesizer applies, in code whose pools exist so
    that trading never allocates. It is now a macro that evaluates its message only when the check
    fails. `lowlatency_allocation_tests`, a program of its own that counts every `operator new`,
    checks that these allocate nothing.
  - A full lock-free queue (`common/lf_queue.h`) overwrote the oldest message its reader hadn't
    read yet, silently: a reader 262,144 messages behind, such as a stalled market data publisher,
    lost them, and since the publisher numbers updates as it sends them, a lost update left no gap
    in the sequence numbers. A write to a full queue now stops the program ("LFQueue full";
    Fillscope's owner chose this, 2026-10-24, over making the writer wait, which would change the
    latency the book measures and can hang the order server and the matcher on each other's full
    queues). The loggers' queues are the same queue, so a logger 8,388,608 values behind (each
    character of a message is one, and each number) stops the program too.
  - A new thread could read freed stack. `createAndStartThread()` (`common/thread_utils.h`) gave
    the thread references to the function, its arguments and the thread's name, all in the
    caller's frame, then slept a second so the thread would have used them before the call
    returned; a thread not scheduled within the second read them after they were gone (under
    AddressSanitizer, without the sleep: `stack-use-after-scope`). The thread now gets its own
    copies, as `std::thread`'s would, and the call returns at once. The second was paid for every
    thread: the exchange took about 9 s to open its port, and now takes about one.
  - The trading client's copy of the books (`trading/strategy/market_order_book.*`), whose best
    bid and ask the market maker and the liquidity taker trade on, had three defects
    (`tests/market_order_book_test.cpp`). It recomputed a side's best price only if the update's
    price was at least as good as that side's best order, decided before the update was applied:
    on an empty side there was none, so the first order there left the best unset until an update
    at that order's price or better came, and until then the strategies wouldn't trade the ticker. A clear (which starts a client's recovery
    from a snapshot) has no side, so it left the old best prices standing, and it also left the
    book's table of levels by price pointing at the levels it freed, so an order at one of those
    prices after the clear joined a freed level, outside the book. Now an update on an empty side
    and a clear recompute the best, and a clear empties the table. And the book's two tables
    (orders by id, levels by price) were never initialized, so a book made in reused memory held
    stale pointers; the client makes its books once, in fresh memory, which hid it. Both now
    start empty. Fillscope checks every order the two strategies send against their rules, on
    the real exchange (its `session_system_tests`).

**Known issues,** not fixed (yet):
- **A book holds at most 255 price levels at once.** Its pool of levels has 256 blocks, and taking
  the last one stops the program ("Memory Pool out of space"). The book's clients use a few; a
  deep book of real prices could need more.
- **Threads are never joined.** `createAndStartThread()` allocates each `std::thread` with `new`,
  and the components' `start()` functions drop the pointer. (The tests tell LeakSanitizer to
  ignore these allocations.)
- **Shutdown is slow:** each `MEOrderBook`'s destructor clears its 2 GB table of client orders,
  which takes seconds per book.
- **A client has at most 1,048,576 order ids.** The matcher's table of a client's orders is indexed
  by the order id, so the order server refuses an id of 1,048,576 or more. The trading client
  numbers its orders from 1 up and never reuses one, so a long enough run of a strategy has every
  further order rejected. Raising the limit means a table not indexed by the id.
- **A new order that reuses a live order's id** overwrites its slot, and the older order can no
  longer be cancelled. The order server can't see it: whether an id is live is the matcher's.
- **A backlog of market data can be lost whole.** The publisher sends everything queued in one
  datagram, and `McastSocket::sendAndRecv()` only logs a failed `send()`: more than about 1,559
  updates (65,507 bytes) at once fail with `EMSGSIZE`, and a listener sees a gap.
- **A recording that can't be written ends, and the exchange runs on** (Fillscope's owner chose
  this, 2026-10-24: the recording is for studying a run, and shouldn't stop the trading). It says
  so on stderr, with how many requests it holds; nothing in the file says it ends early, and the
  rest of the run can't be replayed.
- **`createSocket()` doesn't check `connect()`:** it compares the result with 1, which `connect()`
  never returns. (Comparing with -1 would fail the non-blocking connect it makes, which reports
  "in progress" as -1.)

More changes follow, each listed here, as Fillscope's
[plan](https://github.com/dougdew64/fillscope/blob/main/docs/PLAN.md) proceeds. A bug found in the book's code is fixed here, with a test, and listed. The code remains
under the original MIT license ([LICENSE](LICENSE)).

**Testing it** (from `Chapter12/`): `cmake -S . -B build -DLOWLATENCY_TESTS=ON && cmake --build build
&& ctest --test-dir build --output-on-failure`. The tests take about 17 seconds and 4.3 GB of memory
(the exchange's objects preallocate their pools), and use multicast on the loopback interface.

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
