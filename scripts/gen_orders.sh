NUM_L=500

PRICE=888
COUNT=10

INSTR="GOOG"
ORD_TYPE="S"

FIRST_EXEC_ID=500

FILE_NAME="orders.out"

for ((i=FIRST_EXEC_ID;i<NUM_L+FIRST_EXEC_ID;i++));
do
        # Write orders
        echo "${ORD_TYPE} ${i} ${INSTR} ${PRICE} ${COUNT}" >> ${FILE_NAME}
done
