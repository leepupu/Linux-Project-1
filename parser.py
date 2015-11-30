lines = open('/proc/8776/maps', 'r').readlines()

vmas = []

for line in lines:
	s = int(line[:line.index('-')], 16)
	e = int(line[line.index('-')+1:line.index('-')+9], 16)
	# print hex(s), hex(e)
	vmas.append({'s': s, 'e': e})


lines = open('log1.txt', 'r').readlines()
lines = lines[1:-1]
print lines[-1]

lines = [int(s[6:s.index(',')], 16) for s in lines]

counter = 0
size = len(lines)
for vma in vmas:
	while counter < size and lines[counter] < vma['e']:
		vma['size'] = vma.get('size', 0) + 1
		counter += 1

print vmas

for vma in vmas:
	print "%08X-%08X size: %u" % (vma['s'], vma['e'], vma.get('size',0)*4)

