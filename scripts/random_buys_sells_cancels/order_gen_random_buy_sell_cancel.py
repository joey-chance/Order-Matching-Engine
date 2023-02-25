import random as ran
import sys

# Set default order values
client_number = 0
num_orders = 1000
order_id_start = client_number*num_orders
instr_name = "GOOG"
# Set price
price = 2700
price_is_random = True
price_variance = 100
# Set Total Qty
total_qty = 59999
# Set output filename
out_file = 'random_c{0}_{1}.in'.format(client_number, order_id_start)
# Set weighted probability of buy, sells, cancels
buys_weight = 45
sells_weight = 45
cancels_weight = 10
total_weight = buys_weight + sells_weight + cancels_weight

# Generate num_orders of qtys that sum to total_qty, i.e. number of orders = num_orders
if num_orders > total_qty:
    print("Error!")
    sys.exit()

qtys = []
leftover_check = 0

s = 0
for i in range(num_orders):
    
    r = ran.randint(5,10)
    while (r == 0.0 or r == 1):
        r = ran.randint(5,10)
    s += r
    qtys.append(r)

for idx, r in enumerate(qtys):
    qtys[idx] = round(r/s*total_qty)
    leftover_check += qtys[idx]

while(leftover_check != total_qty):
    if (leftover_check < total_qty):
        iter = 0
        while(leftover_check != total_qty):
            # print(total_qty - leftover_check)
            qtys[iter%len(qtys)] += 1 #increment qty by 1
            leftover_check +=1
            iter += leftover_check #hopefully random jumps to increment values
        break
    else: 
        for idx, r in enumerate(qtys):
            if qtys[idx] > 0:
                qtys[idx] -= 1
        leftover_check = sum(qtys)

order_list = []

with open(out_file, 'a+') as f:
    for idx, qty in enumerate(qtys):
        if price_is_random:
            order_price = ran.randint(price-price_variance, price+price_variance)
        
        # order_type = "B" if ran.randint(0,1) else "S"
        order_num = ran.randint(1, total_weight)
        if (order_num <= buys_weight):
            order_type = "B"
            order_list.append(order_id_start+idx)
            f.write('{0} {1} {2} {3} {4}\n'.format(order_type, order_id_start+idx, instr_name, order_price, qty))
        elif ( buys_weight < order_num <= (buys_weight+sells_weight)):
            order_type = "S"
            order_list.append(order_id_start+idx)
            f.write('{0} {1} {2} {3} {4}\n'.format(order_type, order_id_start+idx, instr_name, order_price, qty))
        else:
            order_type = "C"
            cancel_order_num = ran.choice(order_list)
            f.write('{0} {1}\n'.format(order_type, cancel_order_num))

        
        #print('{0} {1} {2} {3} {4}'.format(order_type, order_id_start+idx, instr_name, price, qty))
