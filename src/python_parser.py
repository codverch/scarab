import struct
import argparse
from tqdm import tqdm

addr_to_counter_dict = {}
fusable = []
nopable = []
mapping = {}
mask = 0

def increment_counter_per_instaddr(addr : int) -> int:
    global addr_to_counter_dict
    if not addr in addr_to_counter_dict:
        addr_to_counter_dict[addr] = 1
        return 1
    # old_count = addr_to_counter_dict[addr]
    addr_to_counter_dict[addr] += 1
    return addr_to_counter_dict[addr]

def get_cacheline(M):
    return (M['read_address'] // 64);

def get_cacheline_and_baseregs(M):
    num_l1d_regs = M['num_l1d_regs']
    l1d_regs = M['l1d_regs']
    combined_tuple = tuple(l1d_regs[:num_l1d_regs])
    return (M['read_address'] // 64, combined_tuple);

def get_bit(mask: int, offset: int) -> int:
    if offset < 0 or offset >= 64:
        raise ValueError(f"Error: offset {offset} is out of bounds (0–63)")
    return (mask >> offset) & 1

class outentry:
    def __init__(self):
        self.counter = -1
        self.addr = -1
        self.dsts = set()
        self.type = -1
        self.bregs = []

    def __str__(self):
        return f"outentry(counter={self.counter}, addr={self.addr}, dsts={self.dsts}, type={self.type}, bregs={self.bregs})"

    def __repr__(self):
        return self.__str__() 

def register_instruction(M):
    global mapping
    global fusable
    global nopable
    global mask

    cacheline_being_hit = get_cacheline(M)
    instruction_addr = M['instruction_addr']
    my_counter = increment_counter_per_instaddr(instruction_addr) 

    # was the previous access to the mapping a memory load
    if cacheline_being_hit in mapping:
        prev_entry = mapping[cacheline_being_hit]
        del mapping[cacheline_being_hit] # delete it
        if prev_entry.type == 0 and M['type'] == 0: # we are both memory loads accessing the same cacheline
            fusable.append(prev_entry) # append old as a fusable type
            nopable_inst = outentry() #
            nopable_inst.counter = my_counter
            nopable_inst.addr = instruction_addr
            nopable.append(nopable_inst) # and myself as a nopable type
        elif prev_entry.type == 1 and M['type'] == 1 and prev_entry.bregs == M['l1d_regs']: # we are both stores with matching base registers
            fusable.append(prev_entry) # append old as a fusable type
            nopable_inst = outentry() #
            nopable_inst.counter = my_counter
            nopable_inst.addr = instruction_addr
            nopable.append(nopable_inst) # and myself as a nopable type
        else: # something went wrong -- the previous entry is invalid and the current entry is the latest
            new_entry = outentry()
            new_entry.counter = my_counter
            new_entry.addr = instruction_addr
            new_entry.dsts = set() # TODO actually add regs
            new_entry.bregs = M['l1d_regs']
            new_entry.type = M['type']
    else: # the entry doesn't exist
        new_entry = outentry()
        new_entry.counter = my_counter
        new_entry.addr = instruction_addr
        new_entry.dsts = set() # TODO actually add regs
        new_entry.bregs = M['l1d_regs']
        new_entry.type = M['type']
        mapping[cacheline_being_hit] = new_entry
    return
# def register_instruction(M):
#     global mapping
#     global fusable
#     global nopable
#     global mask

#     cacheline_being_hit = get_cacheline(M)
#     instruction_addr = M['instruction_addr']
#     my_counter = increment_counter_per_instaddr(instruction_addr) 

#     if cacheline_being_hit in mapping: # trivial to modify to include l1d regs as the key to ensure same base reg as well or within a distance of 10
#         fusable_old_inst = mapping[cacheline_being_hit]
#         del mapping[cacheline_being_hit]
#         for i in range(M['num_dst_regs']):
#             fusable_old_inst.dsts.add(M['dst_regs'][i])
#         fusable.append(fusable_old_inst)
#         nopable_inst = outentry()
#         nopable_inst.counter = my_counter
#         nopable_inst.addr = instruction_addr
#         nopable.append(nopable_inst)
#     else: # a memory load is hitting this cacheline for the first time
#         new_inst = outentry()
#         new_inst.counter = my_counter
#         new_inst.addr = instruction_addr
#         for i in range(M['num_dst_regs']):
#             new_inst.dsts.add(M['dst_regs'][i])
#         mapping[cacheline_being_hit] = new_inst
#     return

# store pairs have single base register
# load pairs can have whatever
# register renaming handles everything but we don't want to introduce a new stall 
#     to prevent this, in scarab, rename everything dependent on the tail in the catalyst 
#     to instead depend 
# we don't want a store and a load to hit the same cacheline, interfering with each other

def read_struct_from_file(file_name):
    global fusable
    global mapping
    global mapping
    struct_format = 'q q q q 2q q 8B q q'  # Corresponds to your struct
    with open(file_name, 'rb') as file:
        file_size = len(file.read())
        file.seek(0)  # Reset the pointer to the beginning of the file
        
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
                    'read_address': unpacked_data[2],
                    'num_l1d_regs': unpacked_data[3],
                    'l1d_regs': unpacked_data[4:6],  # 2 L1D registers
                    'num_dst_regs': unpacked_data[6],
                    'dst_regs': unpacked_data[7:15],  # 8 destination registers
                    'reg_use_mask': unpacked_data[15],
                    'type': unpacked_data[16]
                }
                # print(M)
                register_instruction(M)
                
                pbar.update(struct.calcsize(struct_format))  

    print(len(fusable), len(nopable))

    return fusable, nopable

        # for fusable_inst in fusable:
        #     print("fusable", fusable_inst['instruction_addr'])
        # for nopable_inst in nopable:
        #     print("nopable", nopable_inst['instruction_addr'])

def write_results_to_file(file_name, fusable, nopable):
    struct_format = 'q q q 8B q'
    with open(file_name, 'wb') as file:
        for i, fusable_inst in enumerate(tqdm(fusable)):
            list_of_reg = list(fusable_inst.dsts)
            if(len(list_of_reg) < 8):
                list_of_reg += [0] * (8 - len(list_of_reg))
            packed_data = struct.pack(struct_format, fusable_inst.addr, fusable_inst.counter, len(fusable_inst.dsts),*list_of_reg, 0)
            # print("Counter[F] : ", fusable_inst)
            file.write(packed_data)
        for i, nopable_inst in enumerate(tqdm(nopable)):
            packed_data = struct.pack(struct_format, nopable_inst.addr, nopable_inst.counter, 0, *([0] * 8), 1)
            # print("Counter[N] : ", nopable_inst)
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



