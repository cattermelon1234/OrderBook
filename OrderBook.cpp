#include <algorithm>
#include <cmath>
#include <functional>
#include <iomanip>
#include <iostream>
#include <list>
#include <map>
#include <optional>
#include <set>
#include <stdexcept>
#include <tuple>
#include <unordered_map>
#include <vector>

enum class Side { BUY, SELL };

enum class OrderType { LIMIT, MARKET };

using OrderId = uint64_t;
using Price = uint32_t;
using Qty = uint32_t;

class Order {
public:
  Order(Side side, OrderId id, OrderType type, Qty qty)
      : side_{side}, id_{id}, type_{type}, initialQty_{qty},
        remainingQty_{qty} {}
  Order(Side side, OrderId id, OrderType type, Price price, Qty qty)
      : side_{side}, id_{id}, type_{type}, price_{price}, initialQty_{qty},
        remainingQty_{qty} {}

  OrderId getOrderId() { return id_; }
  Side getSide() { return side_; }
  std::optional<Price> getPrice() { return price_; }
  Qty getInitialQty() { return initialQty_; }
  Qty getRemainingQty() { return remainingQty_; }
  Qty getFilledQty() { return initialQty_ - remainingQty_; }

  void fill(Qty exec) {
    if (exec > remainingQty_)
      throw std::logic_error("overfill");
    remainingQty_ -= exec;
  }
  bool isFilled() { return remainingQty_ == 0; }

private:
  OrderId id_;
  OrderType type_;
  Side side_;
  std::optional<Price> price_;
  Qty initialQty_;
  Qty remainingQty_;
};

// for logging purposes
struct TradeInfo {
  OrderId id_;
  Price price_;
  Qty qty_;
};

class Trade {
public:
  Trade(const TradeInfo &buy, const TradeInfo &sell)
      : buy_{buy}, sell_{sell} {};
  const TradeInfo &getBuy() { return buy_; }
  const TradeInfo &getSell() { return sell_; }

private:
  const TradeInfo buy_;
  const TradeInfo sell_;
};

using Trades = std::vector<Trade>;
using Level = std::list<Order>;
using Handler = std::list<Order>::iterator;
struct Handle {
  Side side;
  Price price;
  Handler it;
};

class OrderBook {
public:
  OrderBook() : asks_{}, bids_{}, orderIdToIterator_{} {}

  OrderId nextId() {
    return nextOrderId_.fetch_add(1, std::memory_order_relaxed);
  }
  Trades add_limit(Side side, Price price, Qty qty) {
    OrderId id = nextId();
    Order order(side, id, OrderType::LIMIT, price, qty);
    Handler iterator;

    if (side == Side::BUY) {
      auto &orders = bids_[price];
      orders.push_back(order);
      iterator = std::prev(orders.end());
    } else {
      auto &orders = asks_[price];
      orders.push_back(order);
      iterator = std::prev(orders.end());
    }
    Handle handle{side, price, iterator};
    orderIdToIterator_.insert({id, handle});
    return matchOrders();
  }

  Trades add_market(Side side, Qty qty) {
    Trades trades{};
    if (qty == 0)
      return trades;

    OrderId marketId = nextId(); // synthetic id for the market order

    auto consume = [&](auto &opp) {
      while (qty > 0 && !opp.empty()) {
        auto best = opp.begin();
        Price price = best->first;
        Level &level = best->second;

        for (auto it = level.begin(); it != level.end() && qty > 0;) {
          Order &resting = *it;
          Qty exec = std::min(qty, resting.getRemainingQty());

          OrderId restingId = resting.getOrderId();
          if (side == Side::BUY) {
            trades.emplace_back(TradeInfo{marketId, price, exec}, // buy
                                TradeInfo{restingId, price, exec} // sell
            );
          } else {
            trades.emplace_back(TradeInfo{restingId, price, exec}, // buy
                                TradeInfo{marketId, price, exec}   // sell
            );
          }

          resting.fill(exec);
          qty -= exec;

          if (resting.isFilled()) {
            orderIdToIterator_.erase(restingId);
            it = level.erase(it);
          } else {
            ++it;
          }
        }

        if (level.empty())
          opp.erase(best);
      }
    };

    if (side == Side::BUY)
      consume(asks_); // buy market consumes asks
    else
      consume(bids_); // sell market consumes bids

    return trades;
  }

  bool cancel(OrderId id) {}

  Trades matchOrders() {}

private:
  std::map<Price, Level, std::less<Price>> asks_;
  std::map<Price, Level, std::greater<Price>> bids_;
  std::map<OrderId, Handle> orderIdToIterator_;
  std::atomic<OrderId> nextOrderId_{1};
};
