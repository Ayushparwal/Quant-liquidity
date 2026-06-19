#include <iostream>
#include <map>
#include <queue>
#include <vector>
#include <thread>
#include <mutex>
#include <atomic>
#include <chrono>
#include <sstream>
#include <iomanip>
#include <netinet/in.h>
#include <unistd.h>
#include <cstring>
#include <arpa/inet.h>

// ─── Types ────────────────────────────────────────────────────────────────────
using Price  = double;
using Qty    = int;
using TimeNs = long long;

enum class Side { BUY, SELL };
enum class OrderType { LIMIT, MARKET };

struct Order {
    int      id;
    Side     side;
    OrderType type;
    Price    price;
    Qty      qty;
    TimeNs   timestamp;
};

struct Trade {
    int    buy_id, sell_id;
    Price  price;
    Qty    qty;
    TimeNs timestamp;
};

// ─── Liquidity Identifier ─────────────────────────────────────────────────────
struct LiquiditySnapshot {
    Price best_bid   = 0;
    Price best_ask   = 0;
    Qty   bid_depth  = 0;   // total qty on bid side
    Qty   ask_depth  = 0;   // total qty on ask side
    double imbalance = 0;   // (bid_depth - ask_depth) / (bid_depth + ask_depth)
    std::string signal;     // BUY_PRESSURE / SELL_PRESSURE / NEUTRAL
};

// ─── Order Book ───────────────────────────────────────────────────────────────
class OrderBook {
public:
    std::mutex mtx;
    // bids: highest price first
    std::map<Price, std::vector<Order>, std::greater<Price>> bids;
    // asks: lowest price first
    std::map<Price, std::vector<Order>>                      asks;
    std::vector<Trade> trade_log;
    std::atomic<int>   order_id_counter{1};

    int addOrder(Side side, OrderType type, Price price, Qty qty) {
        std::lock_guard<std::mutex> lock(mtx);
        Order o;
        o.id        = order_id_counter++;
        o.side      = side;
        o.type      = type;
        o.price     = price;
        o.qty       = qty;
        o.timestamp = now_ns();

        if (type == OrderType::MARKET) {
            matchMarket(o);
        } else {
            matchLimit(o);
        }
        return o.id;
    }

    LiquiditySnapshot getLiquidity() {
        std::lock_guard<std::mutex> lock(mtx);
        LiquiditySnapshot snap;

        if (!bids.empty()) {
            snap.best_bid = bids.begin()->first;
            for (auto& [p, orders] : bids)
                for (auto& o : orders) snap.bid_depth += o.qty;
        }
        if (!asks.empty()) {
            snap.best_ask = asks.begin()->first;
            for (auto& [p, orders] : asks)
                for (auto& o : orders) snap.ask_depth += o.qty;
        }

        int total = snap.bid_depth + snap.ask_depth;
        if (total > 0) {
            snap.imbalance = (double)(snap.bid_depth - snap.ask_depth) / total;
        }

        if      (snap.imbalance >  0.2) snap.signal = "BUY_PRESSURE";
        else if (snap.imbalance < -0.2) snap.signal = "SELL_PRESSURE";
        else                            snap.signal = "NEUTRAL";

        return snap;
    }

    void printBook() {
        std::lock_guard<std::mutex> lock(mtx);
        std::cout << "\n===== ORDER BOOK =====\n";
        std::cout << "ASKS:\n";
        for (auto it = asks.rbegin(); it != asks.rend(); ++it) {
            Qty total = 0;
            for (auto& o : it->second) total += o.qty;
            std::cout << "  " << std::fixed << std::setprecision(2)
                      << it->first << "  x" << total << "\n";
        }
        std::cout << "  ---- SPREAD ----\n";
        std::cout << "BIDS:\n";
        for (auto& [p, orders] : bids) {
            Qty total = 0;
            for (auto& o : orders) total += o.qty;
            std::cout << "  " << std::fixed << std::setprecision(2)
                      << p << "  x" << total << "\n";
        }

        auto snap = getLiquidity();
        std::cout << "\n--- LIQUIDITY SIGNAL ---\n";
        std::cout << "Bid Depth: " << snap.bid_depth
                  << "  Ask Depth: " << snap.ask_depth << "\n";
        std::cout << "Imbalance: " << std::fixed << std::setprecision(3)
                  << snap.imbalance << "\n";
        std::cout << "Signal: " << snap.signal << "\n";
        std::cout << "========================\n\n";
    }

private:
    TimeNs now_ns() {
        return std::chrono::duration_cast<std::chrono::nanoseconds>(
            std::chrono::high_resolution_clock::now().time_since_epoch()).count();
    }

    void matchLimit(Order& o) {
        if (o.side == Side::BUY) {
            while (o.qty > 0 && !asks.empty() && asks.begin()->first <= o.price) {
                auto& [ask_price, ask_orders] = *asks.begin();
                while (o.qty > 0 && !ask_orders.empty()) {
                    auto& top = ask_orders.front();
                    Qty   fill = std::min(o.qty, top.qty);
                    recordTrade(o.id, top.id, ask_price, fill);
                    o.qty   -= fill;
                    top.qty -= fill;
                    if (top.qty == 0) ask_orders.erase(ask_orders.begin());
                }
                if (ask_orders.empty()) asks.erase(asks.begin());
            }
            if (o.qty > 0) bids[o.price].push_back(o);
        } else {
            while (o.qty > 0 && !bids.empty() && bids.begin()->first >= o.price) {
                auto& [bid_price, bid_orders] = *bids.begin();
                while (o.qty > 0 && !bid_orders.empty()) {
                    auto& top = bid_orders.front();
                    Qty   fill = std::min(o.qty, top.qty);
                    recordTrade(top.id, o.id, bid_price, fill);
                    o.qty   -= fill;
                    top.qty -= fill;
                    if (top.qty == 0) bid_orders.erase(bid_orders.begin());
                }
                if (bid_orders.empty()) bids.erase(bids.begin());
            }
            if (o.qty > 0) asks[o.price].push_back(o);
        }
    }

    void matchMarket(Order& o) {
        if (o.side == Side::BUY) {
            while (o.qty > 0 && !asks.empty()) {
                auto& [ask_price, ask_orders] = *asks.begin();
                while (o.qty > 0 && !ask_orders.empty()) {
                    auto& top = ask_orders.front();
                    Qty   fill = std::min(o.qty, top.qty);
                    recordTrade(o.id, top.id, ask_price, fill);
                    o.qty -= fill; top.qty -= fill;
                    if (top.qty == 0) ask_orders.erase(ask_orders.begin());
                }
                if (ask_orders.empty()) asks.erase(asks.begin());
            }
        } else {
            while (o.qty > 0 && !bids.empty()) {
                auto& [bid_price, bid_orders] = *bids.begin();
                while (o.qty > 0 && !bid_orders.empty()) {
                    auto& top = bid_orders.front();
                    Qty   fill = std::min(o.qty, top.qty);
                    recordTrade(top.id, o.id, bid_price, fill);
                    o.qty -= fill; top.qty -= fill;
                    if (top.qty == 0) bid_orders.erase(bid_orders.begin());
                }
                if (bid_orders.empty()) bids.erase(bids.begin());
            }
        }
    }

    void recordTrade(int buy_id, int sell_id, Price p, Qty q) {
        Trade t{buy_id, sell_id, p, q, now_ns()};
        trade_log.push_back(t);
        std::cout << "[TRADE] Buy#" << buy_id << " x Sell#" << sell_id
                  << "  Price=" << std::fixed << std::setprecision(2) << p
                  << "  Qty=" << q << "\n";
    }
};

// ─── TCP Server ───────────────────────────────────────────────────────────────
// Protocol: "BUY LIMIT 100.50 200\n" or "SELL MARKET 0 100\n" or "BOOK\n"
void tcpServer(OrderBook& book, int port = 9090) {
    int server_fd = socket(AF_INET, SOCK_STREAM, 0);
    int opt = 1;
    setsockopt(server_fd, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));

    sockaddr_in addr{};
    addr.sin_family      = AF_INET;
    addr.sin_addr.s_addr = INADDR_ANY;
    addr.sin_port        = htons(port);

    bind(server_fd, (sockaddr*)&addr, sizeof(addr));
    listen(server_fd, 5);
    std::cout << "[SERVER] Listening on port " << port << "\n";

    while (true) {
        int client = accept(server_fd, nullptr, nullptr);
        std::thread([&book, client]() {
            char buf[256];
            while (true) {
                memset(buf, 0, sizeof(buf));
                int n = recv(client, buf, sizeof(buf)-1, 0);
                if (n <= 0) break;

                std::istringstream ss(buf);
                std::string side_str, type_str;
                double price; int qty;
                ss >> side_str >> type_str >> price >> qty;

                std::string response;
                if (side_str == "BOOK") {
                    book.printBook();
                    response = "OK: book printed to server console\n";
                } else {
                    Side s     = (side_str == "BUY") ? Side::BUY : Side::SELL;
                    OrderType t = (type_str == "MARKET") ? OrderType::MARKET : OrderType::LIMIT;
                    int id     = book.addOrder(s, t, price, qty);
                    auto snap  = book.getLiquidity();
                    response   = "OK: OrderID=" + std::to_string(id) +
                                 " Signal=" + snap.signal + "\n";
                }
                send(client, response.c_str(), response.size(), 0);
            }
            close(client);
        }).detach();
    }
}

// ─── Main ─────────────────────────────────────────────────────────────────────
int main() {
    OrderBook book;

    // Seed some orders so book isn't empty
    book.addOrder(Side::BUY,  OrderType::LIMIT, 99.50, 500);
    book.addOrder(Side::BUY,  OrderType::LIMIT, 99.00, 300);
    book.addOrder(Side::BUY,  OrderType::LIMIT, 98.50, 200);
    book.addOrder(Side::SELL, OrderType::LIMIT, 100.00, 400);
    book.addOrder(Side::SELL, OrderType::LIMIT, 100.50, 250);
    book.addOrder(Side::SELL, OrderType::LIMIT, 101.00, 150);

    book.printBook();

    // Run TCP server in background thread
    std::thread server_thread([&book]() { tcpServer(book); });
    server_thread.detach();

    // Interactive CLI
    std::cout << "Commands: BUY/SELL LIMIT/MARKET <price> <qty>  |  BOOK  |  QUIT\n";
    std::string line;
    while (std::getline(std::cin, line)) {
        if (line == "QUIT") break;
        if (line == "BOOK") { book.printBook(); continue; }

        std::istringstream ss(line);
        std::string side_str, type_str;
        double price; int qty;
        if (!(ss >> side_str >> type_str >> price >> qty)) {
            std::cout << "Invalid command\n"; continue;
        }
        Side s      = (side_str == "BUY") ? Side::BUY : Side::SELL;
        OrderType t = (type_str == "MARKET") ? OrderType::MARKET : OrderType::LIMIT;
        int id      = book.addOrder(s, t, price, qty);
        std::cout << "Order #" << id << " placed\n";
        book.printBook();
    }
    return 0;
}