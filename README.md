# This fork: the back end of Fillscope

This is a fork of Packt's repository for Sourav Ghosh's *Building Low Latency Applications with
C++* (Packt, 2023). Its `fillscope` branch is the back end of
[Fillscope](https://github.com/dougdew64/fillscope), an observatory that shows what this trading
system does and explains it from its exact state. `main` follows upstream unchanged.

**What the branch changes** in the code, only in `Chapter12/`, the complete system; the other
chapters are as upstream has them. (At the top level it adds this section and a `CLAUDE.md`.)
- `Chapter12/CMakeLists.txt` requires CMake 3.10 rather than 3.0, which CMake 4 no longer accepts
  (and below 3.10, deprecates). With that, the book's code builds unchanged with GCC 16.2.1 under
  its own `-Werror`, in both configurations of `scripts/build.sh`, and with no warnings from CMake
  4.3.

More changes follow, each listed here, as Fillscope's
[plan](https://github.com/dougdew64/fillscope/blob/main/docs/PLAN.md) proceeds: tests of what
Fillscope relies on, then deterministic replay. A bug found in the book's code is fixed here, with
a test, and listed. The code remains under the original MIT license ([LICENSE](LICENSE)).

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
