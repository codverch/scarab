
class PredictorEntry():
	def __init__(self):
		self.pc = 0
		self.offset = 0
		self.confidence = 0
	def __str__(self):
		return hex(self.pc) + "\t" + str(self.offset) + "\t" + str(self.confidence)


class UCHEntry():

	def __init__(self):
		self.va = 0
		self.age = 0
		self.valid = False

	def __str__(self):
		return hex(self.va) + "\t" + str(self.age) + "\t" + str(self.valid)

predictor = []
UCH = [UCHEntry() for i in range(6)]
def get_cache_tag(addr):
	## just strip off the bottom 6 bits
	return (addr >> 6) << 6

def search_predictor(addr):
	for entry in predictor:
		if entry.pc == addr:
			return entry
	return None

def search_uch(ip, va):
	tag = get_cache_tag(va)
	for entry in UCH:
		if entry.valid and get_cache_tag(entry.va) == tag:
			# update predictor confidence
			ip_tag = get_cache_tag(ip)
			pred_entry = search_predictor(ip)
			if pred_entry is None:
				e = PredictorEntry()
				e.pc = ip_tag
				e.offset = va - entry.va
				predictor.append(e)
			else:
				pred_entry.confidence = min(pred_entry.confidence+1, 3)
			return entry
	for entry in UCH:
		if not entry.valid:
			return entry
	return None

def print_uch():
	i = 0
	for entry in UCH:
		print(f"Entry {i}\t{str(entry)}")
		i += 1

def parse_trace(file):
	with open(file) as f:
		lines = f.readlines()
		# va's are at 9 mod 10
		offset = 0
		addrs = []
		while offset+9 < len(lines):
			ip_line = lines[offset+1]
			addr_line = lines[offset+9]
			ip_str = ip_line[5:].strip()
			addr_str = addr_line[3:15]
			addrs.append((int(ip_str, 16), int(addr_str, 16)))
			offset += 10

		return addrs

def predict(ip):
	return
addrs = parse_trace("src/output")
for (ip, va) in addrs:
	predict(ip)
	entry = search_uch(ip, va)
	if entry is None:
		print_uch()
		break
	if not entry.valid:
		entry.va = va
		entry.valid = True
	
print([(hex(ip), hex(va)) for ip, va in addrs[1:10]])