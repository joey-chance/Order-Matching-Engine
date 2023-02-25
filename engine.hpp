// This file contains declarations for the main Engine class. You will
// need to add declarations to this file as you develop your Engine.

#ifndef ENGINE_HPP
#define ENGINE_HPP

#include <chrono>

#include <unordered_map>
#include <memory>
#include <set>

#include "io.hpp"



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

using Order_And_Set = std::pair<std::shared_ptr<Order>, std::shared_ptr<Orders>>;

struct Engine
{
public:
	void accept(ClientConnection conn);
	// Engine();
private:
	void connection_thread(ClientConnection conn);
	// inline static std::unordered_map<std::string, std::shared_ptr<Orders>> order_book = std::unordered_map<std::string, std::shared_ptr<Orders>>();
	std::unordered_map<std::string, std::shared_ptr<Orders>> order_book;
};

inline std::chrono::microseconds::rep getCurrentTimestamp() noexcept
{
	return std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now().time_since_epoch()).count();
}

#endif
