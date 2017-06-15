#!/usr/bin/python

import os
import signal
import hashlib
import subprocess
import inspect
import multiprocessing

# global stuffs
TestDir = "testdir"
LoDir = TestDir + "/lower"
UpDir = TestDir + "/upper"
MntDir = TestDir + "/mnt"
LoFile = LoDir + "/file1"
UpFile = UpDir + "/file1"
MntFile = MntDir + "/file1"
OrigSize = 4096
DbgFile = TestDir + "/log"

# path to unionfs executable
UfsBin = "../src/unionfs"

# original data at the lower branch
OrigData = bytearray(['o']*OrigSize)

FailCnt = 0

# various utility functions

def do_check(cond, lineno):
	global FailCnt
	if cond == False:
		print "FAILURE: line " + str(lineno)
		FailCnt += 1

def create_file(fname, data):
	fh = open(fname, 'w+')
	fh.write(data)
	fh.close()

def write_file(fname, data, offset = 0):
	fh = open(fname, 'r+')
	fh.seek(offset)
	fh.write(data)
	fh.close()

def read_file(fname, offset = 0):
	fh = open(fname, 'r')
	fh.seek(offset)
	data = fh.read()
	fh.close()
	return data

def trunc_file(fname, size):
	fh = open(fname, 'r+')
	fh.truncate(size)
	fh.close()

def create_testbed():
	os.system("rm -rf " + TestDir)
	os.mkdir(TestDir)
	os.mkdir(LoDir)
	os.mkdir(UpDir)
	os.mkdir(MntDir)
	create_file(LoFile, OrigData)

def do_setup():
	print "Creating test setup..."
	create_testbed()
	os.system(UfsBin
		+ " -o cow,cowolf,cowolf_file_size=10 -o auto_unmount,debug_file=" + DbgFile
		+ " "
		+ UpDir + "=RW:" + LoDir + "=RO "
		+ MntDir)

def undo_setup():
	print "Destroying test setup..."
	res = subprocess.check_output(["pgrep", "-f", UfsBin, "-u",  str(os.getuid())])
	os.kill(int(res), signal.SIGTERM)

# test code
def test_func(fc):

	# set it to negative value to indicate abort
	fc.value = -1

	# initial check
	fdata = read_file(MntFile)
	do_check(len(fdata) == OrigSize, inspect.currentframe().f_lineno)
	m1 = hashlib.md5(fdata).hexdigest()
	m2 = hashlib.md5(OrigData).hexdigest()
	do_check(m1 == m2, inspect.currentframe().f_lineno)

	# write 10 bytes and check
	offset = 100
	size = 10
	w1 = bytearray(['A']*size)
	refdata = OrigData
	write_file(MntFile, w1, offset)
	refdata[offset:offset+len(w1)] = w1
	fdata = read_file(MntFile)
	do_check(len(fdata) == OrigSize, inspect.currentframe().f_lineno)
	m1 = hashlib.md5(fdata).hexdigest()
	m2 = hashlib.md5(refdata).hexdigest()
	do_check(m1 == m2, inspect.currentframe().f_lineno)

	# check upper level sparse data
	sparsedata = bytearray([0]*OrigSize)
	sparsedata[offset:offset+len(w1)] = w1
	fdata = read_file(UpFile)
	do_check(len(fdata) == OrigSize, inspect.currentframe().f_lineno)
	m1 = hashlib.md5(fdata).hexdigest()
	m2 = hashlib.md5(sparsedata).hexdigest()
	do_check(m1 == m2, inspect.currentframe().f_lineno)

	# write 10 bytes and check
	offset = 1000
	size = 1000
	w1 = bytearray(['B']*size)
	refdata = OrigData
	write_file(MntFile, w1, offset)
	refdata[offset:offset+len(w1)] = w1
	fdata = read_file(MntFile)
	do_check(len(fdata) == OrigSize, inspect.currentframe().f_lineno)
	m1 = hashlib.md5(fdata).hexdigest()
	m2 = hashlib.md5(refdata).hexdigest()
	do_check(m1 == m2, inspect.currentframe().f_lineno)

	# overlapping write with earlier write
	offset = 1800
	size = 500
	w1 = bytearray(['C']*size)
	refdata = OrigData
	write_file(MntFile, w1, offset)
	refdata[offset:offset+len(w1)] = w1
	fdata = read_file(MntFile)
	do_check(len(fdata) == OrigSize, inspect.currentframe().f_lineno)
	m1 = hashlib.md5(fdata).hexdigest()
	m2 = hashlib.md5(refdata).hexdigest()
	do_check(m1 == m2, inspect.currentframe().f_lineno)

	# insert a write at the middle
	offset = 200
	size = 100
	w1 = bytearray(['D']*size)
	refdata = OrigData
	write_file(MntFile, w1, offset)
	refdata[offset:offset+len(w1)] = w1
	fdata = read_file(MntFile)
	do_check(len(fdata) == OrigSize, inspect.currentframe().f_lineno)
	m1 = hashlib.md5(fdata).hexdigest()
	m2 = hashlib.md5(refdata).hexdigest()
	do_check(m1 == m2, inspect.currentframe().f_lineno)

	# a write encompassing earlier writes
	offset = 100 
	size = 400
	w1 = bytearray(['E']*size)
	refdata = OrigData
	write_file(MntFile, w1, offset)
	refdata[offset:offset+len(w1)] = w1
	fdata = read_file(MntFile)
	do_check(len(fdata) == OrigSize, inspect.currentframe().f_lineno)
	m1 = hashlib.md5(fdata).hexdigest()
	m2 = hashlib.md5(refdata).hexdigest()
	do_check(m1 == m2, inspect.currentframe().f_lineno)

	# insert a write and merge two writes at the left and right
	offset = 500 
	size = 500
	w1 = bytearray(['F']*size)
	refdata = OrigData
	write_file(MntFile, w1, offset)
	refdata[offset:offset+len(w1)] = w1
	fdata = read_file(MntFile)
	do_check(len(fdata) == OrigSize, inspect.currentframe().f_lineno)
	m1 = hashlib.md5(fdata).hexdigest()
	m2 = hashlib.md5(refdata).hexdigest()
	do_check(m1 == m2, inspect.currentframe().f_lineno)

	# write at the beginning of file
	offset = 0
	size = 50
	w1 = bytearray(['G']*size)
	refdata = OrigData
	write_file(MntFile, w1, offset)
	refdata[offset:offset+len(w1)] = w1
	fdata = read_file(MntFile)
	do_check(len(fdata) == OrigSize, inspect.currentframe().f_lineno)
	m1 = hashlib.md5(fdata).hexdigest()
	m2 = hashlib.md5(refdata).hexdigest()
	do_check(m1 == m2, inspect.currentframe().f_lineno)

	# append
	size = 200
	offset = OrigSize
	w1 = bytearray(['H']*size)
	refdata = OrigData
	write_file(MntFile, w1, offset)
	refdata += w1
	fdata = read_file(MntFile)
	do_check(len(fdata) == (OrigSize + size), inspect.currentframe().f_lineno)
	m1 = hashlib.md5(fdata).hexdigest()
	m2 = hashlib.md5(refdata).hexdigest()
	do_check(m1 == m2, inspect.currentframe().f_lineno)
	new_fsize = OrigSize + size

	# append overlapping existing data
	appsz = 100
	size = 500
	offset = new_fsize + appsz - size
	w1 = bytearray(['I']*size)
	write_file(MntFile, w1, offset)
	refdata = refdata[:offset]
	refdata += w1
	fdata = read_file(MntFile)
	do_check(len(fdata) == (new_fsize + appsz), inspect.currentframe().f_lineno)
	m1 = hashlib.md5(fdata).hexdigest()
	m2 = hashlib.md5(refdata).hexdigest()
	do_check(m1 == m2, inspect.currentframe().f_lineno)
	new_fsize += appsz

	# sparse write (making a hole)
	holesz = 1000
	size = 500
	offset = new_fsize + holesz
	holedata = bytearray([0]*holesz)
	w1 = bytearray(['J']*size)
	write_file(MntFile, w1, offset)
	refdata[new_fsize:new_fsize+len(holedata)] = holedata
	refdata[offset:offset+len(w1)] = w1
	fdata = read_file(MntFile)
	do_check(len(fdata) == (new_fsize + holesz + size), inspect.currentframe().f_lineno)
	m1 = hashlib.md5(fdata).hexdigest()
	m2 = hashlib.md5(refdata).hexdigest()
	do_check(m1 == m2, inspect.currentframe().f_lineno)
	new_fsize += (holesz + size)

	# truncate to size bigger than lower branch
	new_fsize = OrigSize + 311
	fdata = read_file(MntFile)
	do_check(len(fdata) > new_fsize, inspect.currentframe().f_lineno)
	refdata = fdata[:new_fsize]
	trunc_file(MntFile, new_fsize)
	fdata = read_file(MntFile)
	do_check(len(fdata) == new_fsize, inspect.currentframe().f_lineno)
	m1 = hashlib.md5(fdata).hexdigest()
	m2 = hashlib.md5(refdata).hexdigest()
	do_check(m1 == m2, inspect.currentframe().f_lineno)

	# truncate to size smaller than lower branch
	new_fsize = (OrigSize/2) + 17
	fdata = read_file(MntFile)
	do_check(len(fdata) > new_fsize, inspect.currentframe().f_lineno)
	refdata = fdata[:new_fsize]
	trunc_file(MntFile, new_fsize)
	fdata = read_file(MntFile)
	do_check(len(fdata) == new_fsize, inspect.currentframe().f_lineno)
	m1 = hashlib.md5(fdata).hexdigest()
	m2 = hashlib.md5(refdata).hexdigest()
	do_check(m1 == m2, inspect.currentframe().f_lineno)

	# truncate to bigger size (creating sparse area)
	old_size = new_fsize
	new_fsize = (6*OrigSize) + 29
	refdata = read_file(MntFile)
	do_check(len(fdata) < new_fsize, inspect.currentframe().f_lineno)
	sparse_size = new_fsize - old_size
	sparse_data = bytearray([0]*sparse_size)
	refdata += sparse_data
	trunc_file(MntFile, new_fsize)
	fdata = read_file(MntFile)
	do_check(len(fdata) == new_fsize, inspect.currentframe().f_lineno)
	m1 = hashlib.md5(fdata).hexdigest()
	m2 = hashlib.md5(refdata).hexdigest()
	do_check(m1 == m2, inspect.currentframe().f_lineno)

	# truncate to zero size
	trunc_file(MntFile, 0)
	fdata = read_file(MntFile)
	do_check(len(fdata) == 0, inspect.currentframe().f_lineno)

	# append data in for-loop
	cur_size = 0
	refdata = bytearray()
	for index in range(100):
		w1 = bytearray([index+1]*(index+1))
		write_file(MntFile, w1, cur_size)
		cur_size += len(w1)
		refdata += w1
		fdata = read_file(MntFile)
		do_check(len(fdata) == cur_size, inspect.currentframe().f_lineno)
		m1 = hashlib.md5(fdata).hexdigest()
		m2 = hashlib.md5(refdata).hexdigest()
		do_check(m1 == m2, inspect.currentframe().f_lineno)

	# very last statement of test func
	fc.value = FailCnt
##### end of test_func ####

# set up function: do setup and fork a process to execute the tests
def main():
	fail_count = multiprocessing.Value('i', 0)
	do_setup()
	p = multiprocessing.Process(target=test_func, args=(fail_count,))
	p.start()
	p.join()
	undo_setup()
	if fail_count.value == 0:
		print "***** All tests passed. *****"
	elif fail_count.value < 0:
		print "****** Test Aborted. *******"
	else:
		print "***** Test failed in " + str(fail_count.value) + " occasions. *******"

if __name__ == "__main__":
	main()

