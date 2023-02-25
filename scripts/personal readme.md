## To run with Tsan, Lec 4 Slide 32
1. Add `-fsanitize=thread -fPIE` to CXXFLAGS in makefile
2. make
3. run `TSAN_OPTIONS="history_size=7 force_seq_cst_atomics=1" ./engine socket`


## To run grader, grader_README
0. Note: I run on soctf-pdc-019
1. run `./grader engine < <path to test case input>`
2. eg: `./grader engine < ./grader engine < scripts/concurrent-buy_then_concurrent_sell_medium.in`

## To run run_all_basic.sh
0. Ensure cd to scripts folder
1. bash scripts/run_all_basic.sh

## To run test to break instrument concurrency
0. Ensure cd to cs3211-assignment-... folder
1. Run this 5-10 times to check `./grader engine < scripts/instr_concurr_test_medium.in`
2. Run this 5-10times to check `./grader engine < scripts/instr_concurr_test_large_randomised.in`

## To run test to break orderbook concurrency
0. Ensure cd to cs3211-assignment-... folder
1. Run this 5 times to check `./grader engine < scripts/concurrent-buy_then_concurrent_sell_medium.in`
2. Run this 5 times to check `./grader engine < scripts/concurrent-sell_then_concurrent_buy_medium.in`
3. Run this 5 times to check `./grader engine < scripts/concurrent-sell_then_concurrent_buy_large.in`

## To compile & run custom runner
### Compile custom_runner
1. cd to scripts/custom_runner folder
2. clang++ -g -O3 -Wall -Wextra -pedantic -Werror -std=c++20 -pthread custom_runner.cpp
### Run custom_runner
1. Open 2 Shells
2. In Shell 1:
	2a. `./engine socket`
3. In Shell 2:
	3a. cd to custom_runner folder
	3b. `./a.out ../../socket <any number of input files>`
		eg. `./a.out ../../socket small_multi/c1.in`
		eg. `./a.out ../../socket large_multi/c1.in large_multi/c2.in`


Todo:

1. Run Valgrind memcheck (and fix the errors)
2. Run Tsan (and fix the errors)
3. Run Asan (and fix the errors)
4. Run Helgrind (and fix the errors)


Done:
0. Fix segfault on partial filling orders
1. Fix execution id bug
2. Figure out a way to break instr concurr w/o instr locks (to test that instr locks are useful)
3. Change to timestamp,  std::atomic<int>

Notes:
1. New script to generate a bunch of similar orders (gen_test.sh)
2. `cat  instr_concurr_test_large_test.in | shuf > instr_concurr_test_large_randomised.in` shuf command to randomise sequence of orders
3. instr_concurr_test_medium.in produces bug with buy added to order book when matchable sell in order book & vice versa, fix is in this commit
4. instr_concurr_test_medium.in can also produce error occassionally when instr_lks are removed. Thus proving efficacy of instr_lk.


Deprecated Notes:

1. (Solved) Partial filling Orders cause segfault
2. There is shared mutex to do the synchronisation...no longer need my impl :(
3. Error forced on soctf-pdc-019 (since it has 2*10 cores and 40 threads)
	3b. (Now able to) Unable to force an error with 20 writers
		- My rw-sync is only on buys now
		- By right, 20 Buy orders (unique instr) should = 20 writers
			- These should work fine due to rw-sync
		- 20 Sell Orders (unique instr) should = 20 writers
			- should fail due to no rw-sync
		- Problem: Both work fine.
4. Maybe can have 2 semaphores, buy/sell semaphore so only the same kind can go through