import struct
import argparse
from tqdm import tqdm



def get_cacheline(val):
    return val // 64;

def register_instruction(M, mapping, fusable, nopable):
    # ignore counter because ideal oracle fusion
    value = M
    key = get_cacheline(M['read_addresses'][0]) # because only one num ld

    if key in mapping:
        old_inst = mapping[key]
        set_of_registers_old = set(old_inst['dst_regs'][0:old_inst['num_dst']])
        set_of_registers_new = set(M['dst_regs'][0:M['num_dst']])
        after_fuse_dst_regs = set_of_registers_old | set_of_registers_new;
        fusable.append((old_inst['counter'], after_fuse_dst_regs))
        nopable.append((M['counter'], set()))
        del mapping[key]
    else:
        mapping[key] = value

    return

def read_struct_from_file(file_name):
    struct_format = 'q q q 8q q 8B'  # Corresponds to your struct
    with open(file_name, 'rb') as file:
        file_size = len(file.read())
        file.seek(0)  # Reset the pointer to the beginning of the file

        mapping = {}
        fusable = []
        nopable = []
        
        with tqdm(total=file_size, unit='B', unit_scale=True, desc="Reading struct data") as pbar:
            while True:
                data = file.read(struct.calcsize(struct_format))
                if not data:
                    break  # Stop if no more data to read

                unpacked_data = struct.unpack(struct_format, data)
                # print(unpacked_data)
                M = {
                    'counter': unpacked_data[0],
                    'instruction_addr': unpacked_data[1],
                    'num_ld': unpacked_data[2],
                    'read_addresses': unpacked_data[3:11],  # Slice for the 8 read addresses
                    'num_dst': unpacked_data[11],
                    'dst_regs': unpacked_data[12:20],  # Slice for the 8 destination registers
                }
                counter = unpacked_data[0]
                instruction_addr = unpacked_data[1]
                num_ld = unpacked_data[2]
                read_addresses = unpacked_data[3:11]
                num_dst = unpacked_data[11]
                dst_regs = unpacked_data[12:20]

                if num_ld == 1:
                    register_instruction(M, mapping, fusable, nopable)
                
                pbar.update(struct.calcsize(struct_format))  

        print(len(fusable), len(nopable))

        return fusable, nopable

        # for fusable_inst in fusable:
        #     print("fusable", fusable_inst['instruction_addr'])
        # for nopable_inst in nopable:
        #     print("nopable", nopable_inst['instruction_addr'])

def write_results_to_file(file_name, fusable, nopable):
    struct_format = 'q q 8q b 7x'
    with open(file_name, 'wb') as file:
        for fusable_inst in tqdm(fusable):
            list_of_reg = list(fusable_inst[1])
            if(len(list_of_reg) < 8):
                list_of_reg += [0] * (8 - len(list_of_reg))
            packed_data = struct.pack(struct_format, fusable_inst[0], len(fusable_inst[1]), *list_of_reg, 0)
            # print("Counter[F] : ", fusable_inst[0])
            file.write(packed_data)
        for nopable_inst in tqdm(nopable):
            packed_data = struct.pack(struct_format, nopable_inst[0], 0, *([0] * 8), 1)
            # print("Counter[N] : ", nopable_inst[0])
            file.write(packed_data)

# Set up argument parsing
def main():
    parser = argparse.ArgumentParser(description="Read binary struct data from a file.")
    parser.add_argument('--record_file', default='record.log.bin', help="Path to the binary log file (default: 'record.log.bin')")
    parser.add_argument('--output_file', default='record.out.bin', help="Path to save parsed results to")
    args = parser.parse_args()

    # Read the struct from the file
    fusable, nopable = read_struct_from_file(args.record_file)
    write_results_to_file(args.output_file, fusable, nopable)


if __name__ == '__main__':
    main()
