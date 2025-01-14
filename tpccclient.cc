#include "tpccclient.h"

#include <chrono>
#include <cstdio>
#include <vector>

#include "assert.h"
#include "clock.h"
#include "randomgenerator.h"
#include "tpccdb.h"

using std::vector;

TPCCClient::TPCCClient(Clock *clock, tpcc::RandomGenerator *generator, TPCCDB *db, int num_items, int num_warehouses,
                       int districts_per_warehouse, int customers_per_district, const char *now)
        : clock_(clock),
          generator_(generator),
          db_(db),
          num_items_(num_items),
          num_warehouses_(num_warehouses),
          districts_per_warehouse_(districts_per_warehouse),
          customers_per_district_(customers_per_district),
          remote_item_milli_p_(OrderLine::REMOTE_PROBABILITY_MILLIS),
          bound_warehouse_(0),
          bound_district_(0) {
    ASSERT(clock_ != NULL);
    ASSERT(generator_ != NULL);
    ASSERT(db_ != NULL);
    ASSERT(1 <= num_items_ && num_items_ <= Item::NUM_ITEMS);
    ASSERT(1 <= num_warehouses_ && num_warehouses_ <= Warehouse::MAX_WAREHOUSE_ID);
    ASSERT(1 <= districts_per_warehouse_ &&
           districts_per_warehouse_ <= District::NUM_PER_WAREHOUSE);
    ASSERT(1 <= customers_per_district_ && customers_per_district_ <= Customer::NUM_PER_DISTRICT);
    strcpy(now_, now);
}

TPCCClient::~TPCCClient() {
    delete clock_;
    delete generator_;
    delete db_;
}

uint64_t TPCCClient::doStockLevel() {
    int32_t threshold = generator_->number(MIN_STOCK_LEVEL_THRESHOLD, MAX_STOCK_LEVEL_THRESHOLD);
    int32_t w_id = generateWarehouse();
    int32_t d_id = generateDistrict();
    auto beg = std::chrono::high_resolution_clock::now();
    int result = db_->stockLevel(w_id, d_id, threshold);
    ASSERT(result >= 0);
    auto end = std::chrono::high_resolution_clock::now();
    return std::chrono::duration_cast<std::chrono::nanoseconds>(end - beg).count();
}

uint64_t TPCCClient::doOrderStatus() {
    OrderStatusOutput output;
    int y = generator_->number(1, 100);
    int32_t w_id = generateWarehouse();
    int32_t d_id = generateDistrict();
    if (y <= 60) {
        // 60%: order status by last name
        char c_last[Customer::MAX_LAST + 1];
        generator_->lastName(c_last, customers_per_district_);

        auto beg = std::chrono::high_resolution_clock::now();
        db_->orderStatus(w_id, d_id, c_last, &output);
        auto end = std::chrono::high_resolution_clock::now();
        return std::chrono::duration_cast<std::chrono::nanoseconds>(end - beg).count();
    } else {
        // 40%: order status by id
        ASSERT(y > 60);

        int32_t c_id = generateCID();
        auto beg = std::chrono::high_resolution_clock::now();
        db_->orderStatus(w_id, d_id, c_id, &output);
        auto end = std::chrono::high_resolution_clock::now();
        return std::chrono::duration_cast<std::chrono::nanoseconds>(end - beg).count();
    }
}

uint64_t TPCCClient::doDelivery() {
    int carrier = generator_->number(Order::MIN_CARRIER_ID, Order::MAX_CARRIER_ID);
    char now[Clock::DATETIME_SIZE + 1];
    // clock_->getDateTimestamp(now);
    strcpy(now, now_);

    int32_t w_id = generateWarehouse();
    auto beg = std::chrono::high_resolution_clock::now();
    vector<DeliveryOrderInfo> orders;
    db_->delivery(w_id, carrier, now, &orders, NULL);
    if (orders.size() != District::NUM_PER_WAREHOUSE) {
        printf("Only delivered from %zd districts\n", orders.size());
    }
    auto end = std::chrono::high_resolution_clock::now();
    return std::chrono::duration_cast<std::chrono::nanoseconds>(end - beg).count();
}

uint64_t TPCCClient::doPayment() {
    PaymentOutput output;
    int x = generator_->number(1, 100);
    int y = generator_->number(1, 100);

    int32_t w_id = generateWarehouse();
    int32_t d_id = generateDistrict();

    int32_t c_w_id;
    int32_t c_d_id;
    if (num_warehouses_ == 1 || x <= 85) {
        // 85%: paying through own warehouse (or there is only 1 warehouse)
        c_w_id = w_id;
        c_d_id = d_id;
    } else {
        // 15%: paying through another warehouse:
        // select in range [1, num_warehouses] excluding w_id
        c_w_id = generator_->numberExcluding(1, num_warehouses_, w_id);
        ASSERT(c_w_id != w_id);
        c_d_id = generateDistrict();
    }
    float h_amount = generator_->fixedPoint(2, MIN_PAYMENT_AMOUNT, MAX_PAYMENT_AMOUNT);

    char now[Clock::DATETIME_SIZE + 1];
    strcpy(now, now_);
    // clock_->getDateTimestamp(now);
    if (y <= 60) {
        // 60%: payment by last name
        char c_last[Customer::MAX_LAST + 1];
        generator_->lastName(c_last, customers_per_district_);

        auto beg = std::chrono::high_resolution_clock::now();
        db_->payment(w_id, d_id, c_w_id, c_d_id, c_last, h_amount, now, &output, NULL);
        auto end = std::chrono::high_resolution_clock::now();
        return std::chrono::duration_cast<std::chrono::nanoseconds>(end - beg).count();
    } else {
        // 40%: payment by id
        ASSERT(y > 60);
        int32_t cid = generateCID();
        auto beg = std::chrono::high_resolution_clock::now();
        db_->payment(w_id, d_id, c_w_id, c_d_id, cid, h_amount, now, &output, NULL);
        auto end = std::chrono::high_resolution_clock::now();
        return std::chrono::duration_cast<std::chrono::nanoseconds>(end - beg).count();
    }
}

uint64_t TPCCClient::doNewOrder() {
    int32_t w_id = generateWarehouse();
    int ol_cnt = generator_->number(Order::MIN_OL_CNT, Order::MAX_OL_CNT);

    // 1% of transactions roll back
    bool rollback = generator_->number(1, 100) == 1;

    vector<NewOrderItem> items(ol_cnt);
    for (int i = 0; i < ol_cnt; ++i) {
        if (rollback && i + 1 == ol_cnt) {
            items[i].i_id = Item::NUM_ITEMS + 1;
        } else {
            items[i].i_id = generateItemID();
        }

        // TPC-C suggests generating a number in range (1, 100) and selecting remote on 1
        // This provides more variation, and lets us tune the fraction of "remote" transactions.
        bool remote = generator_->number(1, 1000) <= remote_item_milli_p_;
        if (num_warehouses_ > 1 && remote) {
            items[i].ol_supply_w_id = generator_->numberExcluding(1, num_warehouses_, w_id);
        } else {
            items[i].ol_supply_w_id = w_id;
        }
        items[i].ol_quantity = generator_->number(1, MAX_OL_QUANTITY);
    }

    char now[Clock::DATETIME_SIZE + 1];
    strcpy(now, now_);
    // clock_->getDateTimestamp(now);
    NewOrderOutput output;
    int32_t dist = generateDistrict();
    int32_t cid = generateCID();
    auto beg = std::chrono::high_resolution_clock::now();
    db_->newOrder(w_id, dist, cid, items, now, &output, nullptr);
    auto end = std::chrono::high_resolution_clock::now();
    return std::chrono::duration_cast<std::chrono::nanoseconds>(end - beg).count();
}

uint64_t TPCCClient::doOne() {
    // This is not strictly accurate: The requirement is for certain *minimum* percentages to be
    // maintained. This is close to the right thing, but not precisely correct.
    // See TPC-C 5.2.4 (page 68).
    uint64_t nanos;
    int x = generator_->number(1, 100);
    if (x <= 4) {// 4%
        nanos = doStockLevel();
    } else if (x <= 8) {// 4%
        nanos = doDelivery();
    } else if (x <= 12) {// 4%
        nanos = doOrderStatus();
    } else if (x <= 12 + 43) {// 43%
        nanos = doPayment();
    } else {// 45%
        ASSERT(x > 100 - 45);
        nanos = doNewOrder();
    }

    return nanos;
}

void TPCCClient::remote_item_milli_p(int remote_item_milli_p) {
    assert(0 <= remote_item_milli_p && remote_item_milli_p <= 1000);
    remote_item_milli_p_ = remote_item_milli_p;
}

void TPCCClient::bindWarehouseDistrict(int warehouse_id, int district_id) {
    assert(0 <= warehouse_id && warehouse_id <= num_warehouses_);
    assert(0 <= district_id && district_id <= districts_per_warehouse_);
    bound_warehouse_ = warehouse_id;
    bound_district_ = district_id;
}

int32_t TPCCClient::generateWarehouse() {
    if (bound_warehouse_ == 0) {
        return generator_->number(1, num_warehouses_);
    } else {
        return bound_warehouse_;
    }
}

int32_t TPCCClient::generateDistrict() {
    if (bound_district_ == 0) {
        return generator_->number(1, districts_per_warehouse_);
    } else {
        return bound_district_;
    }
}

int32_t TPCCClient::generateCID() {
    return generator_->NURand(1023, 1, customers_per_district_);
}

int32_t TPCCClient::generateItemID() {
    return generator_->NURand(8191, 1, num_items_);
}
