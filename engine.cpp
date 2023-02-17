#include <iostream>
#include <thread>
#include <unordered_map>
#include <string>
#include <set>
#include <utility>
#include <unordered_set>
#include <functional>
#include <set>
#include <memory>

#include "io.hpp"
#include "engine.hpp"

struct Order
{
	ClientCommand info;
	uint64_t time;
	bool operator==(const Order &other) const
	{
		return info.order_id == other.info.order_id;
	}
	bool operator<(const Order& a) const
	{
		// Used for sorting Buys
		if (info.price == a.info.price) 
		{
			return time < a.time;
		}
		return a.info.price < info.price;
	}
	bool operator>(const Order& a) const
	{
		// Used for sorting Sells
		if (info.price == a.info.price) 
		{
			return time < a.time;
		}
		return a.info.price > info.price;
	}
};

struct Buys
{
	std::set<Order> pq;
	std::mutex match_mutex;
	std::mutex execute_mutex;
	std::mutex enqueue_mutex;
};

struct Sells
{
	std::set<Order, std::greater<Order> > pq;
	std::mutex match_mutex;
	std::mutex execute_mutex;
	std::mutex enqueue_mutex;
};

using Orders = std::pair<Buys, Sells>;
using Order_Set = std::pair<std::weak_ptr<Order>, Orders*>;

static uint64_t timestamp = 0;
static uint32_t order_id = 0;
static std::unordered_map<std::string, Orders> order_book;

void Engine::accept(ClientConnection connection)
{
	auto thread = std::thread(&Engine::connection_thread, this, std::move(connection));
	thread.detach();
}

void Engine::connection_thread(ClientConnection connection)
{
	std::unordered_map<int, Order_Set> my_orders;
	while(true)
	{
		ClientCommand input {};
		switch(connection.readInput(input))
		{
			case ReadResult::Error: SyncCerr {} << "Error reading input" << std::endl;
			case ReadResult::EndOfFile: return;
			case ReadResult::Success: break;
		}

		switch(input.type)
		{
			case input_cancel: {
				SyncCerr {} << "Got cancel: ID: " << input.order_id << std::endl;

				if (my_orders.find(input.order_id) == my_orders.end()) 
				{
					Output::OrderDeleted(input.order_id, false, timestamp++);
					break;
				}

				Order_Set order_set = my_orders[input.order_id];
				std::shared_ptr<Order> orderptr = order_set.first.lock();
				if (!orderptr)
				{
					Output::OrderDeleted(input.order_id, false, timestamp++);
					break;
				}
				if (orderptr->info.type == input_buy) 
				{
					auto iter = (*(order_set.second)).first.pq.find(*orderptr.get());
					if (iter == (*(order_set.second)).first.pq.end()) {
						Output::OrderDeleted(input.order_id, false, timestamp++);
						break;
					}
					(*(order_set.second)).first.pq.erase(iter);
					Output::OrderDeleted(input.order_id, true, timestamp++);
					break;

				} else 
				{
					auto iter = (*(order_set.second)).second.pq.find(*orderptr.get());
					if (iter == (*(order_set.second)).second.pq.end()) {
						Output::OrderDeleted(input.order_id, false, timestamp++);
						break;
					}
					(*(order_set.second)).second.pq.erase(iter);
					Output::OrderDeleted(input.order_id, true, timestamp++);
					break;
				}
			}
			case input_buy: {
				break;
			}
			case input_sell: {
				break;
			}

			default: {
				SyncCerr {}
				    << "Got order: " << static_cast<char>(input.type) << " " << input.instrument << " x " << input.count << " @ "
				    << input.price << " ID: " << input.order_id << std::endl;

				// Remember to take timestamp at the appropriate time, or compute
				// an appropriate timestamp!
				auto output_time = getCurrentTimestamp();
				Output::OrderAdded(input.order_id, input.instrument, input.price, input.count, input.type == input_sell,
				    output_time);
				break;
			}
		}

		// Additionally:

		// Remember to take timestamp at the appropriate time, or compute
		// an appropriate timestamp!
		intmax_t output_time = getCurrentTimestamp();

		// Check the parameter names in `io.hpp`.
		Output::OrderExecuted(123, 124, 1, 2000, 10, output_time);
	}
}


// Testing priority queue
/*
ClientCommand input {};
	connection.readInput(input);
	ClientCommand input1 {input_buy, order_id++, 5, 2, "AAPL"};
	Order order1 {input1, 3};
	ClientCommand input2 {input_buy, order_id++, 1, 1, "AAPL"};
	Order order2 {input2, 3};
	ClientCommand input3 {input_buy, order_id++, 1, 3, "AAPL"};
	Order order3 {input3, 2};
	ClientCommand input4 {input_buy, order_id++, 5, 4, "AAPL"};
	Order order4 {input4, 1};
	Buys buys;
	buys.pq.insert(order1);
	buys.pq.insert(order2);
	buys.pq.insert(order3);
	buys.pq.insert(order4);
	Order top = *buys.pq.begin();
	std::cout << top.info.price << top.info.count << std::endl;
	Sells sells;
	sells.pq.insert(order2);
	sells.pq.insert(order1);
	sells.pq.insert(order3);
	sells.pq.insert(order4);
	Order bottom = *sells.pq.begin();
	std::cout << bottom.info.price << bottom.info.count << std::endl;
	return;
*/