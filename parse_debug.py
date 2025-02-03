import re
    # debug_icache_stage();
    # debug_decode_stage();
    # debug_map_stage();
    # debug_node_stage();
    # debug_exec_stage();
    # debug_dcache_stage();

global_op_dict = set()
global_icache = []
global_decode = []
global_map = []
global_node = []
global_exec = []
global_dcache = []
TABLE_HEIGHT = 6


class Op:
	def __init__(self):
		self.type = ""
		self.address = 0
		self.va = 0
		self.icache = 0
		self.decode = 0
		self.map = 0
		self.node = 0
		self.exec = 0
		self.dcache = 0

	def __str__(self):
		return self.type + "\t" + str(hex(self.address)) + "\t" + str(hex(self.va))

	def __hash__(self):
		return f"{self.type} {hex(self.address)}".__hash__()


def parse_op_table(lines, offset, slots=6):
	op_array = [Op() for _ in range(slots)]
	if slots == 0:
		return []
	#line 1, op type
	op_type_arr = lines[offset].split("|")
	# print(op_type_arr)
	for i, op_type in enumerate(op_type_arr[1:-1]):
		if i >= slots:
			break
		op_array[i].type = op_type.strip()
	#line 2, address
	# print(op_array)
	# print(lines[offset])
	op_addr_arr = lines[offset+1].split("|")
	for i, op_addr_str in enumerate(op_addr_arr[1:-1]):
		if i >= slots:
			break
		match = re.search("a:([0-9a-f]+)", op_addr_str)
		if match is not None:
			op_array[i].address = int(match.group(1), 16)

	return op_array


def parse_icache(lines, offset):
	# print(lines[offset])
	# first line format: Stage opcount:[] fetch_addr:[] path:[] state:[] next_state:[]
	hdr_re = r"# (\S+)\s+op_count:(\d+) fetch_addr:(0x[0-9a-f]+)"
	match = re.search(hdr_re, lines[offset])
	op_count = match.group(2)
	# op_count =0 
	global_icache.append(op_count)
	#table parsing

	ops = parse_op_table(lines, offset+3, slots=int(op_count))
	for op in ops:
		if str(op) in global_op_dict:
			real_op = global_op_dict[str(op)]
			real_op.icache += 1

	return offset+TABLE_HEIGHT + 2

def parse_decode(lines, offset):
	# print(lines[offset])
	hdr_re = r"# DECODE \d\s+op_count:(\d+)"
	total_ops = 0
	for i in range(7):
		# DECODE 6    op_count:0
		# print(lines[offset])
		# match = re.search(hdr_re, lines[offset])
		# op_count = int(match.group(1))
		op_count = int(lines[offset][23:])

		total_ops += op_count
		ops = parse_op_table(lines, offset+1, slots=int(op_count))
		for op in ops:
			if str(op) in global_op_dict:
				real_op = global_op_dict[str(op)]
				real_op.decode += 1
		offset += TABLE_HEIGHT+1
	global_decode.append(str(total_ops))
	return offset

def parse_map(lines, offset):
	# print(lines[offset])
	hdr_re = r"# MAP \d\s+op_count:(\d+)"
	total_ops = 0
	# MAP 4       op_count:0
	for i in range(5):
		# print(lines[offset])
		# match = re.search(hdr_re, lines[offset])
		# op_count = int(match.group(1))
		op_count = int(lines[offset][23:])
		total_ops += op_count
		ops = parse_op_table(lines, offset+1, slots=int(op_count))
		for op in ops:
			if str(op) in global_op_dict:
				real_op = global_op_dict[str(op)]
				real_op.map += 1
		offset += TABLE_HEIGHT+1
	global_map.append(str(total_ops))
	return offset

def parse_node(lines, offset):
	hdr_re = r"# NODE\s+node_count:(\d+)"
	# NODE        node_count:
	total_ops = 0
	# print(lines[offset])
	# match = re.search(hdr_re, lines[offset])
	# op_count = int(match.group(1))
	op_count = int(lines[offset][25:])
	global_node.append(str(op_count))
	total_ops += op_count
	offset += 2
	# print(total_ops)
	while total_ops > 0:
		# print(lines[offset])
		ops = parse_op_table(lines, offset, slots=min(total_ops, 6))
		for op in ops:
			if str(op) in global_op_dict:
				real_op = global_op_dict[str(op)]
				real_op.map += 1
		total_ops -= 6
		offset += TABLE_HEIGHT-1
	while lines[offset][2:6] != "EXEC":
		offset += 1
	# offset += 1
	# offset += 4
	# offset += TABLE_HEIGHT
	return offset

def parse_exec(lines, offset):
# first line format: Stage opcount:[] fetch_addr:[] path:[] state:[] next_state:[]
	hdr_re = r"# (\S+)\s+op_count:(\d+)"
	# print(lines[offset])
	# EXEC        op_count:0  busy: 0000 0000  mem_stalls: 0000 0000
	# match = re.search(hdr_re, lines[0])
	# op_count = match.group(2)
	op_count = int(lines[offset][23:24])
	global_exec.append(str(op_count))
	#table parsing

	ops = parse_op_table(lines, offset+3, slots=int(op_count))
	for op in ops:
		if str(op) in global_op_dict:
			real_op = global_op_dict[str(op)]
			real_op.exec += 1

	return offset+TABLE_HEIGHT+1

def parse_dcache(lines, offset):
	hdr_re = r"# (\S+)\s+op_count:(\d+)"
	# match = re.search(hdr_re, lines[0])
	# op_count = match.group(2)
	op_count = int(lines[offset][23:24])
	global_dcache.append(str(op_count))
	#table parsing

	ops = parse_op_table(lines, offset+3, slots=int(op_count))
	for op in ops:
		if str(op) in global_op_dict:
			real_op = global_op_dict[str(op)]
			real_op.dcache += 1

	return offset+TABLE_HEIGHT+1

    # debug_icache_stage();
    # debug_decode_stage();
    # debug_map_stage();
    # debug_node_stage();
    # debug_exec_stage();
    # debug_dcache_stage();

with open("src/out", "r") as file:
	lines = file.readlines()
	cycle = 0
	offset = 0
	while cycle < 2000:
		# print(cycle)
		lines = lines[1:]
		# print(lines)
		offset = parse_icache(lines, offset)
		offset = parse_decode(lines, offset)
		offset = parse_map(lines, offset)
		offset = parse_node(lines, offset)
		offset = parse_exec(lines, offset)
		offset = parse_dcache(lines, offset)
		cycle += 1

	print("\t".join(global_icache))
	print("\t".join(global_decode))
	print("\t".join(global_map))
	print("\t".join(global_node))
	print("\t".join(global_exec))
	print("\t".join(global_dcache))

