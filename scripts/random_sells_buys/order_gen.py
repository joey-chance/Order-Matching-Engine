import random as ran
import sys

# Set default order values
order_type = "S"
order_id_start = 1500
num_orders = 500
instr_name = "APPL"
# Set price
price = 2700
price_is_random = True
price_variance = 100
# Set Total Qty
total_qty = 59999
# Set output filename
out_file = "random_c4_sell_1500.in"

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


with open(out_file, 'a+') as f:
    for idx, qty in enumerate(qtys):
        if price_is_random:
            price = ran.randint(price-price_variance, price+price_variance)
        f.write('{0} {1} {2} {3} {4}\n'.format(order_type, order_id_start+idx, instr_name, price, qty))
        #print('{0} {1} {2} {3} {4}'.format(order_type, order_id_start+idx, instr_name, price, qty))
