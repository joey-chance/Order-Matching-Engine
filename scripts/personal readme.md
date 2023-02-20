## To run with Tsan, Lec 4 Slide 32
1. Add `-fsanitize=thread -fPIE` to CXXFLAGS in makefile
2. make
3. run `TSAN_OPTIONS="history_size=7 force_seq_cst_atomics=1" ./engine socket`


## To run grader, grader_README
0. Note: I run on soctf-pdc-019
1. run `./grader engine < <path to test case input>`
2. eg: `./grader engine < ./grader engine < scripts/concurrent-buy_then_concurrent_sell_medium.in`

Todo:
0. Figure out a way to break instr concurr w/o instr locks (to test that instr locks are useful)
1. Change to timestamp, execution_id std::atomic<int>
2. Fix segfault on partial filling orders



Notes:

1. Partial filling Orders cause segfault
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