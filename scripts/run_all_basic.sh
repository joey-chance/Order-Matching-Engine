#!/bin/bash

BASIC_TESTS="../tests/basic*.in"

for testcase in $BASIC_TESTS
do
	echo "========================================================="
    echo "Running testcase: $testcase"
    
    # This line does the running of tests
    # https://stackoverflow.com/questions/31381373/get-last-line-of-shell-output-as-a-variable
    # OUTPUT=$(../grader ../engine < $testcase 2>&1 >/dev/null | tail -1) 

    # Prints only stderr, not stdout
    OUTPUT=$(../grader ../engine < $testcase 2>&1 >/dev/null) # | tr '\0' '\n')
    # OUTPUT=$(../grader ../engine < $testcase)
    
    echo "${OUTPUT}"
    #echo "${OUTPUT:(-12)}"
    
    echo "Finished testcase: $testcase"
    echo "========================================================="
done
