#include <atomic>
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
#include <shared_mutex>
#include <condition_variable>
#include <vector>

#include "io.hpp"
#include "engine.hpp"

struct Order
{
	ClientCommand info;
	uint64_t time; //TODO: atomic?
	uint32_t execution_id; //TODO: atomic? --> 1 order will update this at a time so it's ok
	bool operator==(const Order &other) const
	{
		return info.order_id == other.info.order_id;
	}
	bool operator<(const Order& a) const
	{
		// Used for sorting Buys
		if (info.price == a.info.price) 
			return time < a.time;

		return a.info.price < info.price;
	}
	bool operator>(const Order& a) const
	{
		// Used for sorting Sells
		if (info.price == a.info.price) 
			return time < a.time;
		
		return a.info.price > info.price;
	}
};

using Buys = std::set<Order>;
using Sells = std::set<Order, std::greater<Order> >;

struct Orders 
{
	Buys buys;
	Sells sells;
	std::mutex match_mutex;
	std::mutex execute_mutex;
	std::mutex enqueue_mutex;
	std::mutex instr_mtx;
};
/*

TODO: paste on readme
3 buy orders: B1, B2, B3
3 stages: match, execute, enqueue

	mutex     mutex        mutex
	match     execute      enqueue
T1: B1
T2: B2        B1
T3: B3        B2           B1
...

B1 is done matching. it releases match mutex. it tries to acquire execute mutex, but suddenly it gets context switched out
B2 now matches. B2 finish matching. B2 can acquire execute mutex

S1 --> B1
B2 --> execute 

B1 is done matching. Acquire execute mutex. Release match mutex.
B2 really gets match mutex, B1 definitely already has the execute mutex.

Thread 1: B1, CB1
Thread 2: S1, CS1
S1 --> B1
	match     execute      enqueue
T1: CS1
T2: B1        CS1
T3: 


*/
using Order_And_Set = std::pair<std::shared_ptr<Order>, std::shared_ptr<Orders>>;

std::atomic<uint64_t> timestamp {0};
static std::unordered_map<std::string, std::shared_ptr<Orders>> order_book;
std::atomic<int> concurr_orders {0}; //at most 3
std::atomic<bool> isBuySide {false};
std::atomic<bool> isSellSide {false};

//Synchronization Variables for order_book
std::shared_mutex oob_mutex;
//END: Synchronization Variables for order_book 

//Helper function declarations
// void cancel(CommandType, uint32_t, Order_And_Set&, std::unordered_map<int, Order_And_Set>&);
//END: Helper function declarations
void Engine::accept(ClientConnection connection)
{
	auto thread = std::thread(&Engine::connection_thread, this, std::move(connection));
	thread.detach();
}

void Engine::connection_thread(ClientConnection connection)
{
	std::unordered_map<int, Order_And_Set> my_orders;
	std::condition_variable canEnter;
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
			case input_cancel: 
			{
				SyncCerr {} << "Got cancel: ID: " << input.order_id << std::endl;

				if (!my_orders.contains(input.order_id)) 
				{
					Output::OrderDeleted(input.order_id, false, timestamp++);
					break;
				}

				Order_And_Set order_set = my_orders[input.order_id]; //read my_orders
				std::shared_ptr<Order> orderptr = order_set.first;
				if (orderptr->info.type == input_buy) 
				{
					std::unique_lock match_lk((order_set.second)->match_mutex);
						concurr_orders++; //implicit know it's < 3
						bool exists = (order_set.second)->buys.contains(*orderptr.get());
					std::unique_lock execute_lk((order_set.second)->execute_mutex);
					match_lk.unlock();
						if (exists) 
						{
							(order_set.second)->buys.erase(*orderptr.get());
							Output::OrderDeleted(input.order_id, true, timestamp++);
							concurr_orders--;
							break;
						} else
						{
							my_orders.erase(input.order_id);
							Output::OrderDeleted(input.order_id, false, timestamp++);
							concurr_orders--;
							break;
						}
				} else 
				{
					std::unique_lock match_lk((order_set.second)->match_mutex);
						concurr_orders++; //implicit know it's < 3
						bool exists = (order_set.second)->sells.contains(*orderptr.get());
					std::unique_lock execute_lk((order_set.second)->execute_mutex);
					match_lk.unlock();
						if (exists) 
						{
							(order_set.second)->sells.erase(*orderptr.get());
							Output::OrderDeleted(input.order_id, true, timestamp++);
							concurr_orders--;
							break;
						} else
						{
							my_orders.erase(input.order_id);
							Output::OrderDeleted(input.order_id, false, timestamp++);
							concurr_orders--;
							break;
						}
				}
			}
			case input_buy: 
			{
				bool instr_exists;
				std::shared_ptr<Orders> orders;
				{//Reader Lock, check if instr exists
				std::shared_lock oob_r_lock(oob_mutex);
					instr_exists = order_book.contains(input.instrument);
				}
				
				if (!instr_exists)
				{ //If instr does not exist, writer lock for order_book writing
				std::unique_lock oob_w_lock(oob_mutex);
					//BEGIN: Writer Critical Section
					//Double check if instr does not exist
					if (!order_book.contains(input.instrument)) 
					{
						//Create instrument orders
						// std::cout << "Adding new instrument\n";
						order_book[input.instrument] = std::make_shared<Orders>(); //write order_book
					}
					orders = order_book[input.instrument];
				oob_w_lock.unlock();
					{
					//Match phase
					std::unique_lock match_lk(orders->match_mutex);
						while (!(concurr_orders == 0 || (concurr_orders < 3 && isBuySide))) 
						{
							canEnter.wait(match_lk);
						}
						isBuySide = true;
						concurr_orders++;
						
						//match_mutex is acquired
						//TODO: implement Grab'N'Go. then canEnter.notify_all()
						std::vector<Order> toMatch;
						uint32_t temp_count = input.count;
						while (!orders->sells.empty() && temp_count > 0) 
						{
							if ((orders->sells.begin())->info.price > input.price) 
							{
								//Best sell price too high for active buy, break
								break;
							} else 
							{
								auto sell = orders->sells.begin();
								auto node = orders->sells.extract(sell);
								node.value().execution_id++;
								toMatch.emplace_back(node.value());
								if (temp_count < sell->info.count)
								{
									node.value().info.count -= temp_count;
									orders->sells.insert(std::move(node));
									temp_count = 0;
								} else {
									orders->sells.erase(sell);
									temp_count -= sell->info.count;
								}
							}
						}
					std::unique_lock execute_lk(orders->execute_mutex);
					match_lk.unlock();
					canEnter.notify_one();
					//Match phase end
					//Execution phase begin

						for (auto sell: toMatch)
						{
							Output::OrderExecuted(sell.info.order_id, input.order_id, sell.execution_id, sell.info.price, std::min(input.count, sell.info.count), timestamp++);
							input.count -= sell.info.count;
						}

					std::unique_lock enqueue_lk(orders->enqueue_mutex);
					execute_lk.unlock();
					//Execution phase end
					//Enqueue phase begin

						if (input.count > 0)
						{
							// Create new Buy order
							Order order {input, timestamp++, 0};
							orders->buys.insert(order);
							Output::OrderAdded(input.order_id, input.instrument, input.price, input.count, false, order.time);
							auto ptr = std::make_shared<Order>(order);
							my_orders[input.order_id] = Order_And_Set(ptr, orders);
						}

					concurr_orders--;
					enqueue_lk.unlock();
					//Enqueue phase end
					}
					//END: Writer Critical Section

				} else 
				{ //Else, reader lock for matching
				std::shared_lock oob_r_lock(oob_mutex);
				//BEGIN: Reader Critical Section
				//Get list of resting orders for this instrument
					std::shared_ptr<Orders> orders = order_book[input.instrument];//read order_book
				oob_r_lock.unlock();
				//END: Reader Critical Section
					{
					//Match phase
					std::unique_lock match_lk(orders->match_mutex);
						while (!(concurr_orders == 0 || (concurr_orders < 3 && isBuySide))) 
						{
							canEnter.wait(match_lk);
						}
						isBuySide = true;
						concurr_orders++;
						//match_mutex is acquired
						//TODO: implement Grab'N'Go. then canEnter.notify_all()
						std::vector<Order> toMatch;
						uint32_t temp_count = input.count;
						while (!orders->sells.empty() && temp_count > 0) 
						{
							if ((orders->sells.begin())->info.price > input.price) 
							{
								//Best sell price too high for active buy, break
								break;
							} else 
							{
								auto sell = orders->sells.begin();
								auto node = orders->sells.extract(sell);
								node.value().execution_id++;
								toMatch.emplace_back(node.value());
								if (temp_count < sell->info.count)
								{
									node.value().info.count -= temp_count;
									orders->sells.insert(std::move(node));
									temp_count = 0;
								} else {
									orders->sells.erase(sell);
									temp_count -= sell->info.count;
								}
							}
						}
					std::unique_lock execute_lk(orders->execute_mutex);
					match_lk.unlock();
					canEnter.notify_one();
					//Match phase end
					//Execution phase begin

						for (auto sell: toMatch)
						{
							Output::OrderExecuted(sell.info.order_id, input.order_id, sell.execution_id, sell.info.price, std::min(input.count, sell.info.count), timestamp++);
							input.count -= sell.info.count;
						}

					std::unique_lock enqueue_lk(orders->enqueue_mutex);
					execute_lk.unlock();
					//Execution phase end
					//Enqueue phase begin
						if (input.count > 0)
						{
							// Create new Buy order
							Order order {input, timestamp++, 0};
							orders->buys.insert(order);
							Output::OrderAdded(input.order_id, input.instrument, input.price, input.count, false, order.time);
							auto ptr = std::make_shared<Order>(order);
							my_orders[input.order_id] = Order_And_Set(ptr, orders);
						}
					concurr_orders--;
					enqueue_lk.unlock();
					//Enqueue phase end
					}
				}
				break;
			}
			case input_sell: {
				bool instr_exists;

				{//Reader Lock, check if instr exists
				std::shared_lock lock(oob_mutex);
					instr_exists = order_book.contains(input.instrument);
				}
				if (!instr_exists)
				{ //If instr does not exist, writer lock for order_book writing
				std::unique_lock oob_w_lock(oob_mutex);
					//BEGIN: Writer Critical Section
					//Double check if instr does not exist
					if (order_book.find(input.instrument) == order_book.end()) 
					{
						//Create instrument orders
						// std::cout << "Adding new instrument\n";
						order_book[input.instrument] = std::make_shared<Orders>();
					}
					
					{
					std::unique_lock instr_lk((order_book[input.instrument])->instr_mtx);						
						std::shared_ptr<Orders> orders = order_book[input.instrument];
						//Double check if buys is not empty, buys might have created instr right before sells created instr
						// if (!orders->buys.empty()) {
							while (!orders->buys.empty() && input.count > 0) 
							{
								if ((orders->buys.begin())->info.price < input.price) 
								{
									break;
								} else 
								{
									auto buy = orders->buys.begin();
									if (input.count < buy->info.count)
									{
										auto node = orders->buys.extract(buy);
										node.value().info.count -= input.count;
										node.value().execution_id++;
										Output::OrderExecuted(node.value().info.order_id, input.order_id, node.value().execution_id, node.value().info.price, input.count, timestamp++);
										orders->buys.insert(std::move(node));
										input.count = 0;
									} else {
										input.count -= buy->info.count;
										Output::OrderExecuted(buy->info.order_id, input.order_id, ((buy->execution_id)+1), buy->info.price, buy->info.count, timestamp++);
										orders->buys.erase(buy);
									}
								}
							}
							if (input.count > 0)
							{
								Order order {input, timestamp++, 0};
								order_book[input.instrument]->sells.insert(order);
								Output::OrderAdded(input.order_id, input.instrument, input.price, input.count, true, order.time);
								auto ptr = std::make_shared<Order>(order);
								my_orders[input.order_id] = Order_And_Set(ptr, order_book[input.instrument]);
							}
						// } else {
						// 	//Add Sells Order
						// 	Order order {input, timestamp++, 0};
						// 	order_book[input.instrument]->sells.insert(order);
						// 	Output::OrderAdded(input.order_id, input.instrument, input.price, input.count, true, order.time);
						// 	auto ptr = std::make_shared<Order>(order);
						// 	my_orders[input.order_id] = Order_And_Set(ptr, order_book[input.instrument]);
						// }
					}
					//END: Writer Critical Section
				} else 
				{//Else, reader lock for matching
				std::unique_lock oob_r_lock(oob_mutex);
				
					//BEGIN: Reader Critical Section
					std::shared_ptr<Orders> orders = order_book[input.instrument];
					{
					std::unique_lock instr_lk(orders->instr_mtx);

						while (!orders->buys.empty() && input.count > 0) 
						{
							if ((orders->buys.begin())->info.price < input.price) 
							{
								break;
							} else 
							{
								auto buy = orders->buys.begin();
								if (input.count < buy->info.count)
								{
									auto node = orders->buys.extract(buy);
									node.value().info.count -= input.count;
									node.value().execution_id++;
									Output::OrderExecuted(node.value().info.order_id, input.order_id, node.value().execution_id, node.value().info.price, input.count, timestamp++);
									orders->buys.insert(std::move(node));
									input.count = 0;
								} else 
								{
									input.count -= buy->info.count;
									Output::OrderExecuted(buy->info.order_id, input.order_id, ((buy->execution_id)+1), buy->info.price, buy->info.count, timestamp++);
									orders->buys.erase(buy);
								}
							}
						}
						if (input.count > 0)
						{
							Order order {input, timestamp++, 0};
							order_book[input.instrument]->sells.insert(order);
							Output::OrderAdded(input.order_id, input.instrument, input.price, input.count, true, order.time);
							auto ptr = std::make_shared<Order>(order);
							my_orders[input.order_id] = Order_And_Set(ptr, order_book[input.instrument]);
						}
					}
					//END: Reader Critical Section
				}
				break;
			}

			default: {
				SyncCerr {}
				    << "Got order: " << static_cast<char>(input.type) << " " << input.instrument << " x " << input.count << " @ "
				    << input.price << " ID: " << input.order_id << std::endl;
				break;
			}
		}
	}
}

// void cancel(CommandType type, uint32_t order_id, Order_And_Set& order_set, std::unordered_map<int, Order_And_Set>& my_orders)
// {
// 	std::shared_ptr<Order> orderptr = order_set.first;
// 	if (type == input_buy)
// 	{
// 		std::unique_lock match_lk((order_set.second)->match_mutex);
// 			bool exists = (order_set.second)->buys.contains(*orderptr.get());
// 		std::unique_lock execute_lk((order_set.second)->execute_mutex);
// 		match_lk.unlock();
// 			if (!exists) 
// 			{
// 				my_orders.erase(order_id);
// 				Output::OrderDeleted(order_id, false, timestamp++);
// 				return;
// 			} else 
// 			{
// 				(order_set.second)->buys.erase(*orderptr.get());
// 				Output::OrderDeleted(order_id, true, timestamp++);
// 				return;
// 			}
// 	} else {
// 		std::unique_lock match_lk((order_set.second)->match_mutex);
// 			bool exists = (order_set.second)->sells.contains(*orderptr.get());
// 		std::unique_lock execute_lk((order_set.second)->execute_mutex);
// 		match_lk.unlock();
// 			if (!exists) 
// 			{
// 				my_orders.erase(order_id);
// 				Output::OrderDeleted(order_id, false, timestamp++);
// 				return;
// 			} else 
// 			{
// 				(order_set.second)->sells.erase(*orderptr.get());
// 				Output::OrderDeleted(order_id, true, timestamp++);
// 				return;
// 			}
// 	}
// }

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
	buys.insert(order1);
	buys.insert(order2);
	buys.insert(order3);
	buys.insert(order4);
	Order top = *buys.begin();
	std::cout << top.info.price << top.info.count << std::endl;
	Sells sells;
	sells.insert(order2);
	sells.insert(order1);
	sells.insert(order3);
	sells.insert(order4);
	Order bottom = *sells.begin();
	std::cout << bottom.info.price << bottom.info.count << std::endl;
	return;
*/

//To test the data structures
/*
ClientCommand cmd {input_buy, 125, 1, 1, "AAPL"};
Order order {cmd, 1};
order_book["AAPL"] = new Orders();
(*order_book["AAPL"]).buys.insert(order);
auto ptr = std::make_shared<Order>(order);
std::weak_ptr<Order> wptr = ptr;
my_orders[125] = std::pair<std::weak_ptr<Order>, Orders*>(wptr, order_book["AAPL"]);
*/