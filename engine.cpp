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

std::shared_mutex oob_mutex;

std::atomic<uint64_t> timestamp {0};
std::atomic<int> concurr_orders {0}; //at most 3
std::atomic<bool> isBuySide {false};
/*
Thread 1: C1, B1
Thread 2: C2
Thread 3: C3

	match     execute      enqueue
T1: C1
T2: C2        C1
T3: C3        C2           ..
T4: B1        C3           ..

isBuySide -> false
*/


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
							Output::OrderDeleted(input.order_id, true, timestamp++);
							concurr_orders--;
							(order_set.second)->buys.erase(*orderptr.get());
							break;
						} else
						{
							Output::OrderDeleted(input.order_id, false, timestamp++);
							concurr_orders--;
							my_orders.erase(input.order_id);
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
							Output::OrderDeleted(input.order_id, true, timestamp++);
							concurr_orders--;
							(order_set.second)->sells.erase(*orderptr.get());
							break;
						} else
						{
							Output::OrderDeleted(input.order_id, false, timestamp++);
							concurr_orders--;
							my_orders.erase(input.order_id);
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
						int concurr_orders_now = concurr_orders.load();
						bool isBuySideNow = isBuySide.load();
						while (!(concurr_orders_now == 0 || (concurr_orders_now < 3 && isBuySideNow))) 
						{
							canEnter.wait(match_lk);
						}
						//acquired match mutex
						//buy order and a sell order reach here at the same time
						isBuySide.store(true);
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
									temp_count = 0;
									orders->sells.insert(std::move(node));
								} else {
									temp_count -= sell->info.count;
									orders->sells.insert(std::move(node));
									orders->sells.erase(sell);
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
						int concurr_orders_now = concurr_orders.load();
						bool isBuySideNow = isBuySide.load();
						while (!(concurr_orders_now == 0 || (concurr_orders_now < 3 && isBuySideNow))) 
						{
							canEnter.wait(match_lk);
						}
						isBuySide.store(true);
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
									temp_count = 0;
									orders->sells.insert(std::move(node));
								} else {
									temp_count -= sell->info.count;
									orders->sells.insert(std::move(node));
									orders->sells.erase(sell);
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
			case input_sell: 
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
						int concurr_orders_now = concurr_orders.load();
						bool isBuySideNow = isBuySide.load();
						while (!(concurr_orders_now == 0 || (concurr_orders_now < 3 && !isBuySideNow))) 
						{
							canEnter.wait(match_lk);
						}
						isBuySide.store(false);
						concurr_orders++;
						
						//match_mutex is acquired
						//TODO: implement Grab'N'Go. then canEnter.notify_all()
						std::vector<Order> toMatch;
						uint32_t temp_count = input.count;
						while (!orders->buys.empty() && temp_count > 0) 
						{
							if ((orders->buys.begin())->info.price < input.price) 
							{
								//Best buy price too low for active sell, break
								break;
							} else 
							{
								auto buy = orders->buys.begin();
								auto node = orders->buys.extract(buy);
								node.value().execution_id++;
								toMatch.emplace_back(node.value());
								if (temp_count < buy->info.count)
								{
									node.value().info.count -= temp_count;
									temp_count = 0;
									orders->buys.insert(std::move(node));
								} else {
									temp_count -= buy->info.count;
									orders->buys.insert(std::move(node));
									orders->buys.erase(buy);
								}
							}
						}
					std::unique_lock execute_lk(orders->execute_mutex);
					match_lk.unlock();
					canEnter.notify_one();
					//Match phase end
					//Execution phase begin

						for (auto buy: toMatch)
						{
							Output::OrderExecuted(buy.info.order_id, input.order_id, buy.execution_id, buy.info.price, std::min(input.count, buy.info.count), timestamp++);
							if (input.count < buy.info.count)
							{
								input.count = 0;
							} else 
							{
								input.count -= buy.info.count;
							}
						}

					std::unique_lock enqueue_lk(orders->enqueue_mutex);
					execute_lk.unlock();
					//Execution phase end
					//Enqueue phase begin

						if (input.count > 0)
						{
							// Create new Sell order
							Order order {input, timestamp++, 0};
							orders->sells.insert(order);
							Output::OrderAdded(input.order_id, input.instrument, input.price, input.count, true, order.time);
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
						int concurr_orders_now = concurr_orders.load();
						bool isBuySideNow = isBuySide.load();
						while (!(concurr_orders_now == 0 || (concurr_orders_now < 3 && !isBuySideNow))) 
						{
							canEnter.wait(match_lk);
						}
						isBuySide.store(false);
						concurr_orders++;
						//match_mutex is acquired
						//TODO: implement Grab'N'Go. then canEnter.notify_all()
						std::vector<Order> toMatch;
						uint32_t temp_count = input.count;
						while (!orders->buys.empty() && temp_count > 0) 
						{
							if ((orders->buys.begin())->info.price < input.price) 
							{
								//Best sell price too high for active buy, break
								break;
							} else 
							{
								auto buy = orders->buys.begin();
								auto node = orders->buys.extract(buy);
								node.value().execution_id++;
								toMatch.emplace_back(node.value());
								if (temp_count < buy->info.count)
								{
									node.value().info.count -= temp_count;
									temp_count = 0;
									orders->buys.insert(std::move(node));
								} else {
									temp_count -= buy->info.count;
									orders->buys.insert(std::move(node));
									orders->buys.erase(buy);
								}
							}
						}
					std::unique_lock execute_lk(orders->execute_mutex);
					match_lk.unlock();
					canEnter.notify_one();
					//Match phase end
					//Execution phase begin

						for (auto buy: toMatch)
						{
							Output::OrderExecuted(buy.info.order_id, input.order_id, buy.execution_id, buy.info.price, std::min(input.count, buy.info.count), timestamp++);
							if (input.count < buy.info.count)
							{
								input.count = 0;
							} else 
							{
								input.count -= buy.info.count;
							}
						}

					std::unique_lock enqueue_lk(orders->enqueue_mutex);
					execute_lk.unlock();
					//Execution phase end
					//Enqueue phase begin
						if (input.count > 0)
						{
							// Create new Sell order
							Order order {input, timestamp++, 0};
							orders->sells.insert(order);
							Output::OrderAdded(input.order_id, input.instrument, input.price, input.count, true, order.time);
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

			default: {
				SyncCerr {}
				    << "Got order: " << static_cast<char>(input.type) << " " << input.instrument << " x " << input.count << " @ "
				    << input.price << " ID: " << input.order_id << std::endl;
				break;
			}
		}
	}
}
