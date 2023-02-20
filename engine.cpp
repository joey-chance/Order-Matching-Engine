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
#include <algorithm>
#include <semaphore.h>

#include "io.hpp"
#include "engine.hpp"
#include "lightswitch.hpp"

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

struct Orders 
{
	Buys buys;
	Sells sells;
};

using Order_And_Set = std::pair<std::shared_ptr<Order>, Orders*>;

static uint64_t timestamp = 0;
static std::unordered_map<std::string, Orders*> order_book;

//Synchronization Variables for order_book
sem_t ext_sem;
int sem_init_temp = sem_init(&ext_sem, 0, 1);
std::mutex turnstile;
Lightswitch room_switch(ext_sem);
//END: Synchronization Variables for order_book 

void Engine::accept(ClientConnection connection)
{
	auto thread = std::thread(&Engine::connection_thread, this, std::move(connection));
	thread.detach();
}

void Engine::connection_thread(ClientConnection connection)
{
	std::unordered_map<int, Order_And_Set> my_orders;
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
					// std::cout << "CANNOT FIND " << input.order_id << std::endl;
					Output::OrderDeleted(input.order_id, false, timestamp++);
					break;
				}

				Order_And_Set order_set = my_orders[input.order_id];
				std::shared_ptr<Order> orderptr = order_set.first;
				if (orderptr->info.type == input_buy) 
				{
					auto iter = (order_set.second)->buys.pq.find(*orderptr.get());
					if (iter == (order_set.second)->buys.pq.end()) {
						// std::cout << "ORDER IS DELETED RIGHT BEFORE THIS\n";
						my_orders.erase(input.order_id);
						Output::OrderDeleted(input.order_id, false, timestamp++);
						break;
					}
					(order_set.second)->buys.pq.erase(iter);
					// std::cout << "HOORAYYY\n";
					Output::OrderDeleted(input.order_id, true, timestamp++);
				} else 
				{
					auto iter = (order_set.second)->sells.pq.find(*orderptr.get());
					if (iter == (order_set.second)->sells.pq.end()) {
						// std::cout << "ORDER IS DELETED RIGHT BEFORE THIS\n";
						my_orders.erase(input.order_id);
						Output::OrderDeleted(input.order_id, false, timestamp++);
						break;
					}
					(order_set.second)->sells.pq.erase(iter);
					Output::OrderDeleted(input.order_id, true, timestamp++);
				}
				break;
			}
			case input_buy: {
				bool instr_exists;

				//Reader Lock, check if instr exists
				turnstile.lock();
				turnstile.unlock();
				room_switch.enter();
					instr_exists = order_book.find(input.instrument) != order_book.end();
				room_switch.exit();
				
				if (!instr_exists)
				{ //If instr does not exist, writer lock for order_book writing
					turnstile.lock();
					sem_wait(&ext_sem);
						//critical section
						//Create instrument orders
						// std::cout << "Adding new instrument\n";
						order_book[input.instrument] = new Orders(); //write order_book
						//Create Order
						Order order {input, timestamp++};
						//Insert order into Buys PQ
						//TODO: Lock Buys.mutex
						order_book[input.instrument]->buys.pq.insert(order); //write to order_book
						Output::OrderAdded(input.order_id, input.instrument, input.price, input.count, false, order.time);
						
						auto ptr = std::make_shared<Order>(order);
						my_orders[input.order_id] = Order_And_Set(ptr, order_book[input.instrument]); //read order_book
						//END: critical section
					turnstile.unlock();
					sem_post(&ext_sem);
				} else 
				{ //Else, reader lock for matching
					turnstile.lock();
					turnstile.unlock();
					room_switch.enter();
						//critical section
						//Get list of resting orders for this instrument
						Orders *orders = order_book[input.instrument];//read order_book
						uint32_t execution_id = 1;
						//Read sells order
						//TODO: Lock instr sells mutex
						while (!orders->sells.pq.empty() && input.count > 0) 
						{
							if ((orders->sells.pq.begin())->info.price > input.price) {
								//Best sell price too high for active buy, break
								break;
							} else 
							{

								auto sell = orders->sells.pq.begin();
								if (input.count < sell->info.count)
								{
									auto node = orders->sells.pq.extract(sell);
									node.value().info.count -= input.count;
									orders->sells.pq.insert(std::move(node));
									Output::OrderExecuted(node.value().info.order_id, input.order_id, execution_id++, node.value().info.price, input.count, timestamp++);
									input.count = 0;
								} else {
									input.count -= sell->info.count;
									Output::OrderExecuted(sell->info.order_id, input.order_id, execution_id++, sell->info.price, sell->info.count, timestamp++);
									orders->sells.pq.erase(sell);
								}
							}
						}
						//Handle partially filled active buy order
						if (input.count > 0)
						{
							if (!order_book.contains(input.instrument)) //read order_book
								order_book[input.instrument] = new Orders(); //write order_book
							//Create new Buy order
							//TODO: Lock instr.buys.mutex
							Order order {input, timestamp++};
							order_book[input.instrument]->buys.pq.insert(order);//read order_book
							Output::OrderAdded(input.order_id, input.instrument, input.price, input.count, false, order.time);
							auto ptr = std::make_shared<Order>(order);
							my_orders[input.order_id] = Order_And_Set(ptr, order_book[input.instrument]); //read order_book
						}
						//END: critical section
					room_switch.exit();
				}
				break;
			}
			case input_sell: {
				if (order_book.find(input.instrument) == order_book.end())
				{
					// std::cout << "Adding new instrument\n";
					order_book[input.instrument] = new Orders();
					Order order {input, timestamp++};
					order_book[input.instrument]->sells.pq.insert(order);
					Output::OrderAdded(input.order_id, input.instrument, input.price, input.count, true, order.time);
					auto ptr = std::make_shared<Order>(order);
					my_orders[input.order_id] = Order_And_Set(ptr, order_book[input.instrument]);
				} else 
				{
					Orders *orders = order_book[input.instrument];
					uint32_t execution_id = 1;
					while (!orders->buys.pq.empty() && input.count > 0) 
					{
						if ((orders->buys.pq.begin())->info.price < input.price) {
							break;
						} else 
						{
							auto buy = orders->buys.pq.begin();
							if (input.count < buy->info.count)
							{
								auto node = orders->buys.pq.extract(buy);
								node.value().info.count -= input.count;
								orders->buys.pq.insert(std::move(node));
								Output::OrderExecuted(node.value().info.order_id, input.order_id, execution_id++, node.value().info.price, input.count, timestamp++);
								input.count = 0;
							} else {
								input.count -= buy->info.count;
								Output::OrderExecuted(buy->info.order_id, input.order_id, execution_id++, buy->info.price, buy->info.count, timestamp++);
								orders->buys.pq.erase(buy);
							}
						}
					}
					if (input.count > 0)
					{
						if (!order_book.contains(input.instrument))
							order_book[input.instrument] = new Orders();
						Order order {input, timestamp++};
						order_book[input.instrument]->sells.pq.insert(order);
						Output::OrderAdded(input.order_id, input.instrument, input.price, input.count, true, order.time);
						auto ptr = std::make_shared<Order>(order);
						my_orders[input.order_id] = Order_And_Set(ptr, order_book[input.instrument]);
					}
				}
				break;
			}

			default: {
				SyncCerr {}
				    << "Got order: " << static_cast<char>(input.type) << " " << input.instrument << " x " << input.count << " @ "
				    << input.price << " ID: " << input.order_id << std::endl;

				// Remember to take timestamp at the appropriate time, or compute
				// an appropriate timestamp!
				// auto output_time = getCurrentTimestamp();
				// Output::OrderAdded(input.order_id, input.instrument, input.price, input.count, input.type == input_sell,
				//     output_time);
				break;
			}
		}

		// Additionally:

		// Remember to take timestamp at the appropriate time, or compute
		// an appropriate timestamp!
		// intmax_t output_time = getCurrentTimestamp();

		// Check the parameter names in `io.hpp`.
		// Output::OrderExecuted(123, 124, 1, 2000, 10, output_time);
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

//To test the data structures
/*
ClientCommand cmd {input_buy, 125, 1, 1, "AAPL"};
Order order {cmd, 1};
order_book["AAPL"] = new Orders();
(*order_book["AAPL"]).buys.pq.insert(order);
auto ptr = std::make_shared<Order>(order);
std::weak_ptr<Order> wptr = ptr;
my_orders[125] = std::pair<std::weak_ptr<Order>, Orders*>(wptr, order_book["AAPL"]);
*/