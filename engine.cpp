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

#include "io.hpp"
#include "engine.hpp"

struct Order
{
	ClientCommand info;
	uint64_t time;
	uint32_t execution_id;
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
};

struct Sells
{
	std::set<Order, std::greater<Order> > pq;
};

struct Orders 
{
	Buys buys;
	Sells sells;
	std::mutex match_mutex;
	std::mutex execute_mutex;
	std::mutex enqueue_mutex;
	std::mutex instr_mtx;
};

using Order_And_Set = std::pair<std::shared_ptr<Order>, std::shared_ptr<Orders>>;

std::atomic<uint64_t> timestamp {0};
static std::unordered_map<std::string, std::shared_ptr<Orders>> order_book;

//Synchronization Variables for order_book
std::shared_mutex oob_mutex;
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

				if (!my_orders.contains(input.order_id)) 
				{
					// std::cout << "CANNOT FIND " << input.order_id << std::endl;
					Output::OrderDeleted(input.order_id, false, timestamp++);
					break;
				}

				Order_And_Set order_set = my_orders[input.order_id]; //read my_orders
				std::shared_ptr<Order> orderptr = order_set.first;
				if (orderptr->info.type == input_buy) 
				{
					std::unique_lock instr_lk((order_set.second)->instr_mtx);
						if (!(order_set.second)->buys.pq.contains(*orderptr.get())) {
							// std::cout << "ORDER IS DELETED RIGHT BEFORE THIS\n";
							my_orders.erase(input.order_id);
							Output::OrderDeleted(input.order_id, false, timestamp++);
							break;
						}
						(order_set.second)->buys.pq.erase(*orderptr.get());
						// std::cout << "HOORAYYY\n";
						Output::OrderDeleted(input.order_id, true, timestamp++);
				} else 
				{
					std::unique_lock instr_lk((order_set.second)->instr_mtx);
						if (!(order_set.second)->sells.pq.contains(*orderptr.get())) {
							// std::cout << "ORDER IS DELETED RIGHT BEFORE THIS\n";
							my_orders.erase(input.order_id);
							Output::OrderDeleted(input.order_id, false, timestamp++);
							break;
						}
						(order_set.second)->sells.pq.erase(*orderptr.get());
						Output::OrderDeleted(input.order_id, true, timestamp++);
				}
				break;
			}
			case input_buy: {
				bool instr_exists;

				{//Reader Lock, check if instr exists
					std::shared_lock oob_r_lock(oob_mutex);
					instr_exists = order_book.contains(input.instrument);
				}
				if (!instr_exists)
				{ //If instr does not exist, writer lock for order_book writing
					std::unique_lock oob_w_lock(oob_mutex);
						//BEGIN: Writer Critical Section
						//Double check if instr does not exist
						if (order_book.find(input.instrument) == order_book.end()) {
							//Create instrument orders
							// std::cout << "Adding new instrument\n";
							order_book[input.instrument] = std::make_shared<Orders>(); //write order_book
						}
						{
							std::unique_lock instr_lk((order_book[input.instrument])->instr_mtx);
							std::shared_ptr<Orders> orders = order_book[input.instrument];
							//Double check if sells is not empty, sells might have created instr right before buys created instr
							if (!orders->sells.pq.empty()) {
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
											node.value().execution_id++;
											Output::OrderExecuted(node.value().info.order_id, input.order_id, node.value().execution_id, node.value().info.price, input.count, timestamp++);
											orders->sells.pq.insert(std::move(node));
											input.count = 0;
										} else {
											input.count -= sell->info.count;
											Output::OrderExecuted(sell->info.order_id, input.order_id, ((sell->execution_id)+1), sell->info.price, sell->info.count, timestamp++);
											orders->sells.pq.erase(sell);
										}
									}
								}
								//Handle partially filled active buy order
								if (input.count > 0)
								{
									// Create new Buy order
									Order order {input, timestamp++, 0};
									order_book[input.instrument]->buys.pq.insert(order);//read order_book
									Output::OrderAdded(input.order_id, input.instrument, input.price, input.count, false, order.time);
									auto ptr = std::make_shared<Order>(order);
									my_orders[input.order_id] = Order_And_Set(ptr, order_book[input.instrument]); //read order_book
								}
							} else {
								//Add Buy Order
								Order order {input, timestamp++, 0};
								//Insert order into Buys PQ
								order_book[input.instrument]->buys.pq.insert(order); //write to buys pq
								Output::OrderAdded(input.order_id, input.instrument, input.price, input.count, false, order.time);
								
								auto ptr = std::make_shared<Order>(order);
								my_orders[input.order_id] = Order_And_Set(ptr, order_book[input.instrument]); //read order_book
							}
						}
						//END: Writer Critical Section

				} else 
				{ //Else, reader lock for matching
					std::shared_lock oob_r_lock(oob_mutex);
						//BEGIN: Reader Critical Section
						//Get list of resting orders for this instrument
						std::shared_ptr<Orders> orders = order_book[input.instrument];//read order_book
						
						{
							std::unique_lock instr_lk(orders->instr_mtx);
							//Read sells order
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
										node.value().execution_id++;
										Output::OrderExecuted(node.value().info.order_id, input.order_id, node.value().execution_id, node.value().info.price, input.count, timestamp++);
										orders->sells.pq.insert(std::move(node));
										input.count = 0;
									} else {
										input.count -= sell->info.count;
										Output::OrderExecuted(sell->info.order_id, input.order_id, ((sell->execution_id)+1), sell->info.price, sell->info.count, timestamp++);
										orders->sells.pq.erase(sell);
									}
								}
							}
							//Handle partially filled active buy order
							if (input.count > 0)
							{
								// Create new Buy order
								Order order {input, timestamp++, 0};
								order_book[input.instrument]->buys.pq.insert(order);//read order_book
								Output::OrderAdded(input.order_id, input.instrument, input.price, input.count, false, order.time);
								auto ptr = std::make_shared<Order>(order);
								my_orders[input.order_id] = Order_And_Set(ptr, order_book[input.instrument]); //read order_book
							}
						}
						//END: Reader Critical Section
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
						if (order_book.find(input.instrument) == order_book.end()) {
							//Create instrument orders
							// std::cout << "Adding new instrument\n";
							order_book[input.instrument] = std::make_shared<Orders>();
						}
						
						{
							std::unique_lock instr_lk((order_book[input.instrument])->instr_mtx);						
							//Double check if buys is not empty, buys might have created instr right before sells created instr
							std::shared_ptr<Orders> orders = order_book[input.instrument];
							if (!orders->buys.pq.empty()) {
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
											node.value().execution_id++;
											Output::OrderExecuted(node.value().info.order_id, input.order_id, node.value().execution_id, node.value().info.price, input.count, timestamp++);
											orders->buys.pq.insert(std::move(node));
											input.count = 0;
										} else {
											input.count -= buy->info.count;
											Output::OrderExecuted(buy->info.order_id, input.order_id, ((buy->execution_id)+1), buy->info.price, buy->info.count, timestamp++);
											orders->buys.pq.erase(buy);
										}
									}
								}
								if (input.count > 0)
								{
									Order order {input, timestamp++, 0};
									order_book[input.instrument]->sells.pq.insert(order);
									Output::OrderAdded(input.order_id, input.instrument, input.price, input.count, true, order.time);
									auto ptr = std::make_shared<Order>(order);
									my_orders[input.order_id] = Order_And_Set(ptr, order_book[input.instrument]);
								}
							} else {
								//Add Sells Order
								Order order {input, timestamp++, 0};
								order_book[input.instrument]->sells.pq.insert(order);
								Output::OrderAdded(input.order_id, input.instrument, input.price, input.count, true, order.time);
								auto ptr = std::make_shared<Order>(order);
								my_orders[input.order_id] = Order_And_Set(ptr, order_book[input.instrument]);
							}
						}
						//END: Writer Critical Section
				} else 
				{//Else, reader lock for matching
					std::unique_lock oob_r_lock(oob_mutex);
					
						//BEGIN: Reader Critical Section
						std::shared_ptr<Orders> orders = order_book[input.instrument];
						{
							std::unique_lock instr_lk(orders->instr_mtx);

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
											node.value().execution_id++;
											Output::OrderExecuted(node.value().info.order_id, input.order_id, node.value().execution_id, node.value().info.price, input.count, timestamp++);
											orders->buys.pq.insert(std::move(node));
											input.count = 0;
										} else {
											input.count -= buy->info.count;
											Output::OrderExecuted(buy->info.order_id, input.order_id, ((buy->execution_id)+1), buy->info.price, buy->info.count, timestamp++);
											orders->buys.pq.erase(buy);
										}
									}
								}
								if (input.count > 0)
								{
									Order order {input, timestamp++, 0};
									order_book[input.instrument]->sells.pq.insert(order);
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