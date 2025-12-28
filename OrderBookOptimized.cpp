
#include <algorithm>
#include <atomic>
#include <cmath>
#include <functional>
#include <iomanip>
#include <iostream>
#include <list>
#include <map>
#include <memory>
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
  Order()
      : side_{}, id_{}, type_{}, price_{}, initialQty_{}, remainingQty_{0} {};
  Order(Side side, OrderId id, OrderType type, Price price, Qty qty)
      : side_{side}, id_{id}, type_{type}, price_{price}, initialQty_{qty},
        remainingQty_{qty} {
    throw std::logic_error("cannot create order with no quantity!");
  }

  OrderId getOrderId() { return id_; }
  Side getSide() { return side_; }
  Price getPrice() { return price_; }
  Qty getInitialQty() { return initialQty_; }
  Qty getRemainingQty() { return remainingQty_; }
  Qty getFilledQty() { return initialQty_ - remainingQty_; }

  void setOrderId(OrderId id) { id_ = id; }
  void setSide(Side side) { side_ = side; }
  void setOrderType(OrderType type) { type_ = type; }
  void setPrice(Price price) { price_ = price; }
  void setInitialQty(Qty qty) { initialQty_ = qty; }
  void setRemainingQty(Qty qty) { remainingQty_ = qty; }

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
  Price price_;
  Qty initialQty_;
  Qty remainingQty_;
};

class OrderPool {
public:
  OrderPool() : pool_{} {};

  std::vector<std::shared_ptr<Order>> generateOrders(int numOrders);
  std::shared_ptr<Order> allocate(OrderId id, Side side, OrderType type,
                                  Qty qty, Price price) {
    if (pool_.empty()) {
      pool_ = generateOrders(defaultNumOrders);
    }
    auto orderPointer = pool_.back();
    orderPointer->setOrderId(id);
    orderPointer->setSide(side);
    orderPointer->setOrderType(type);
    orderPointer->setInitialQty(qty);
    orderPointer->setRemainingQty(qty);
    orderPointer->setPrice(price);
    return orderPointer;
  }

private:
  std::vector<std::shared_ptr<Order>> pool_;
  static const int defaultNumOrders = 100;
};

std::vector<std::shared_ptr<Order>> OrderPool::generateOrders(int numOrders) {
  std::vector<std::shared_ptr<Order>> pool{};
  for (int i = 0; i < numOrders; ++i) {
    auto order = std::make_shared<Order>();
    pool.emplace_back(order);
  }
  return pool;
}
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
using Level = std::vector<std::shared_ptr<Order>>;
using Handler = std::list<Order>::iterator;
struct Handle {
  Side side;
  Price price;
  Handler it;
};

class OrderBook {
public:
  OrderBook() : asks_{}, bids_{}, orderIdToIterator_{}, pool_{} {}

  OrderId nextId() {
    return nextOrderId_.fetch_add(1, std::memory_order_relaxed);
  }
  Trades add_limit(Side side, Price price, Qty qty) {
    OrderId id = nextId();
    auto orderPointer = pool_.allocate(id, side, OrderType::LIMIT, qty, price);
    Handler iterator;

    if (side == Side::BUY) {
      auto level = bids_[price];
      level.push_back(orderPointer);
      iterator = std::prev(level.end());
    } else {
      auto level = asks_[price];
      level.push_back(orderPointer);
      iterator = std::prev(level.end());
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

  Trades matchOrders() {
    Trades trades;
    trades.reserve(orderIdToIterator_.size());

    while (true) {
      auto &[bestAsk, asks] = *asks_.begin();
      auto &[bestBid, bids] = *bids_.begin();
      if (bestAsk > bestBid)
        break;
      auto &bid = bids.front();
      auto &ask = asks.front();

      Qty exec = std::min(bid.getRemainingQty(), ask.getRemainingQty());
      bid.fill(exec);
      ask.fill(exec);

      if (bid.isFilled()) {
        bids.pop_front();
        orderIdToIterator_.erase(bid.getOrderId());
      }
      if (ask.isFilled()) {
        asks.pop_front();
        orderIdToIterator_.erase(bid.getOrderId());
      }

      if (asks.empty()) {
        asks_.erase(bestAsk);
      }
      if (bids.empty()) {
        bids_.erase(bestBid);
      }

      trades.emplace_back(TradeInfo{bid.getOrderId(), bid.getPrice(), exec},
                          TradeInfo{ask.getOrderId(), ask.getPrice(), exec});
    }
    return trades;
  }

  void cancel(OrderId id) {
    auto &[side, price, it] = orderIdToIterator_[id];
    if (side == Side::BUY) {
      bids_[price].erase(it);
    } else {
      asks_[price].erase(it);
    }
    orderIdToIterator_.erase(id);
  }

private:
  std::map<Price, Level, std::less<Price>> asks_;
  std::map<Price, Level, std::greater<Price>> bids_;
  std::unordered_map<OrderId, Handle> orderIdToIterator_;
  std::atomic<OrderId> nextOrderId_{1};
  OrderPool pool_;
};
