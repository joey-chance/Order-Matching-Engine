filenames = [
    "random_c1_buy_0.in",
    "random_c2_sell_500.in",
    "random_c3_buy_1000.in",
    "random_c4_sell_1500.in"
]

out_filename = "random_combined.in"

def main():
    thread_num = 0

    for filename in filenames:
        with open(filename,"r") as in_file:
            lines=in_file.readlines()

        out_lines = ["{0} {1}\n".format(thread_num, line.strip()) for line in lines]

        with open(out_filename, "a+") as out_file:
            out_file.writelines(out_lines)
        
        thread_num += 1

main()