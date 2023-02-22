NUM_THD=2
NUM_L=500

PRICE=888
COUNT=10

INSTR="GOOG"
ORD_TYPE="B"

FIRST_THD_NUM=6
FIRST_EXEC_ID=1500

FILE_NAME="instr_concurr_test_large.in"

for ((i=FIRST_EXEC_ID;i<NUM_L+FIRST_EXEC_ID;i++));
do
        # Write orders
        echo "$((i%NUM_THD+FIRST_THD_NUM)) ${ORD_TYPE} ${i} ${INSTR} ${PRICE} ${COUNT}" >> ${FILE_NAME}
done
