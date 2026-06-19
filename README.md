# LiquidFlow — Low-Latency Order Matching Engine

A C++17 limit/market order matching engine with a real-time **Liquidity Identifier** that detects bid-ask imbalance and emits directional pressure signals — simulating core components of an exchange matching system.

## Features
- **Price-time priority matching** for limit and market orders
- **Liquidity Identifier** — computes bid-ask depth imbalance ratio, emits `buy_pressure / sell_pressure / neutral` signals in real-time
- **Concurrent order ingestion** via `std::thread` and `std::mutex`
- **TCP socket interface** on port 9090 for external order submission, simulating exchange connectivity

## Build & Run
```bash
g++ -std=c++17 -O2 -pthread -o orderbook orderbook.cpp
./orderbook
```

## Commands
```
BUY LIMIT 100.50 200      # place a limit buy order
SELL MARKET 0 100         # place a market sell order
BOOK                      # print current order book + liquidity signal
QUIT                      # exit
```

## TCP Interface
```bash
echo "BUY LIMIT 99.50 100" | nc localhost 9090
# returns: OK: OrderID=7 Signal=buy_pressure
```

## Liquidity Identifier Logic
```
imbalance = (bid_depth - ask_depth) / (bid_depth + ask_depth)

imbalance >  0.2  →  buy_pressure
imbalance < -0.2  →  sell_pressure
else              →  neutral
```
