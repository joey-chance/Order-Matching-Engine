#include <iostream>
#include <thread>
#include <unordered_map>
#include <string>
#include <set>
#include <utility>
#include <unordered_set>

#include "io.hpp"
#include "engine.hpp"

struct OrderList {
	std::set<ClientCommand&> orders;
	std::mutex match_mutex;
	std::mutex execute_mutex;
	std::mutex enqueue_mutex;
};

struct Orders {
	OrderList buys;
	OrderList sells;
};

struct compareBuys {
	bool operator()(const ClientCommand& a, const ClientCommand &b) {
		// TODO: implement based on price-time
		return true;
	}
};
struct compareSells {
	bool operator()(const ClientCommand& a, const ClientCommand &b) {
		// TODO: implement based on price-time
		return false;
	}
};
static size_t order_id = 0;
static std::unordered_map<std::string, Orders> order_book;

void Engine::accept(ClientConnection connection)
{
	auto thread = std::thread(&Engine::connection_thread, this, std::move(connection));
	thread.detach();
}

void Engine::connection_thread(ClientConnection connection)
{
	std::unordered_map<int, std::weak_ptr<ClientCommand>> my_orders();
	//std::unordered_map<int, iter of order_book> my_orders();
	while(true)
	{
		ClientCommand input {};
		switch(connection.readInput(input))
		{
			case ReadResult::Error: SyncCerr {} << "Error reading input" << std::endl;
			case ReadResult::EndOfFile: return;
			case ReadResult::Success: break;
		}

		// Functions for printing output actions in the prescribed format are
		// provided in the Output class:
		switch(input.type)
		{
			case input_cancel: {
				SyncCerr {} << "Got cancel: ID: " << input.order_id << std::endl;

				// Remember to take timestamp at the appropriate time, or compute
				// an appropriate timestamp!
				auto output_time = getCurrentTimestamp();
				// TODO:
				// 1. check if order is in my_orders, else don't do anything
				// 2. check if can cancel order (e.g. not being executed), else wait
				
				Output::OrderDeleted(input.order_id, true, output_time);
				break;
			}
			case input_buy: {
				
				my_orders[input.order_id];
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
